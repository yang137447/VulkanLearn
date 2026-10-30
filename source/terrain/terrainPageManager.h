#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainFeedback.h"

namespace VL
{

struct TerrainUploadBatch;

// C2c/P3：World 级地形页管理器的 CPU 侧 residency/eviction 与 streaming 决策。
// 虚拟页（页表项所在位置）与物理页（atlas 槽位）是两套坐标：
//   virtualPage   = registration.pageOrigin + 本地 tile，决定 texCoord1 查表位置；
//   physicalPage  = 0..1023 的 atlas 槽位，由空闲槽位分配器给出。
// 这样淘汰一个虚拟页后，它的物理槽位可以被任意其他虚拟页复用。
// GPU feedback 回读、按帧上传 ring 与物理槽位 quarantine 已由 renderer 接入；
// 低分辨率 fallback/淡入与 256 层纹理 VT 仍留在后续阶段。
class TerrainPageManager
{
public:
    static constexpr uint32_t PageTableSize = TerrainPageTableSize;
    static constexpr uint32_t PageResolution = TerrainPageResolution;
    static constexpr uint32_t AtlasResolution = PageTableSize * PageResolution;
    static constexpr uint32_t PageSlotCount = PageTableSize * PageTableSize;
    // 保留最后一页作为缺页 fallback 的**物理 atlas 槽位**。它不再是虚拟页：
    // 页表里没有 (31,31) 的保留项，所以 32×32 单地形可以合法使用这个虚拟坐标。
    // 可用物理槽位数恒为 PageSlotCount - 1。
    static constexpr uint32_t FallbackPageX = PageTableSize - 1;
    static constexpr uint32_t FallbackPageY = PageTableSize - 1;
    static constexpr uint32_t UsablePhysicalSlotCount = PageSlotCount - 1;
    // CPU 侧淘汰安全窗下限：页最后一次被看到后至少经过这么多 epoch 才允许自动淘汰。
    // 有效窗口由 EvictionWindowFrames() 给出，取 max(下限, framesInFlight + 1)，
    // 同时覆盖“feedback 回读有 2 帧延迟”和“提交队列里仍有 in-flight 帧”。
    // 物理槽位复用另由 quarantine 的 releaseEpoch
    // （submitted + framesInFlight + 1）保证 in-flight 帧已结束。
    static constexpr uint64_t EvictionHysteresisFrames = 2;
    // P3 流式默认预算：必须大于可见 working set，否则被淘汰的页会立刻因 feedback
    // 重新可见，中性页就变成配置结果而不是真实缺页。32×32 宽视野实测约 415 页，
    // 因此默认取 512；它仍小于 1023 个可用物理槽位。
    static constexpr uint32_t DefaultResidentCapacity = 512;
    // 每帧最多把多少个 miss 页转成请求；反馈页按 priority 降序处理。
    static constexpr uint32_t MaxPageRequestsPerFrame = 8;
    // 每帧最多上传多少个 atlas 页；剩余 dirty 页留到下一帧，避免单帧长卡顿。
    static constexpr uint32_t MaxUploadPagesPerFrame = 8;

    struct Registration
    {
        // 注册身份：一次场景摆放一个 placementId；同一 assetPath 可以注册多次。
        std::string placementId;
        // 内容来源，仅用于诊断和烘焙；不参与唯一性判断。
        std::string terrainAssetPath;
        Eigen::Vector2i pageOrigin = Eigen::Vector2i::Zero();
        uint32_t tilesPerSide = 0;
    };

    // 一个虚拟页的身份。全局页表坐标 = registration.pageOrigin + 本地 tile 坐标。
    struct PageKey
    {
        // 摆放身份（placementId），不是 assetPath。
        std::string placementId;
        uint32_t tileX = 0;
        uint32_t tileZ = 0;
    };

    struct PageState
    {
        PageKey key;
        bool resident = false;
        // 全局页表坐标（registration.pageOrigin + 本地 tile），注册后不再变化。
        uint32_t virtualPageX = 0;
        uint32_t virtualPageY = 0;
        // atlas 物理槽位；只有 resident 时有效，淘汰后可能被其他虚拟页复用。
        uint32_t physicalPageX = 0;
        uint32_t physicalPageY = 0;
        // frame 由调用方提供，只用于 LRU 排序；priority 越大越不容易被淘汰。
        uint64_t lastUsedFrame = 0;
        // streaming 专用：最近一次在 feedback/预取中被看到的 GPU epoch。
        // 与 lastUsedFrame 分开，避免把旧 LRU 的 frame 语义混进 GPU 安全窗。
        uint64_t lastSeenEpoch = 0;
        // streaming 专用：最近一次 feedback 命中/预取的 epoch，供 epoch-aware LRU
        // 判断“谁的可见性信息更旧”。lastUsedFrame 仍保留旧帧号 LRU 语义；
        // 无参 FindEvictionCandidateIndex() 不消费本字段。
        uint64_t lastTouchedEpoch = 0;
        // 页内容版本；每次页表项或 atlas 页内容改变递增。uploadedRevision 追上
        // contentRevision 才表示 GPU 侧与 CPU 内容一致。
        uint64_t contentRevision = 0;
        uint64_t uploadedRevision = 0;
        // 本帧请求/待上传的页必须 pin 住，防止同帧“刚驻留就被淘汰”。
        // 用计数而不是 bool：两个重叠上传批次各自持有一份 pin，任意一批
        // Commit/Abort 只能释放自己那一次，不能提前解掉另一批。
        uint32_t uploadPinCount = 0;
        uint32_t priority = 0;
        uint32_t referenceCount = 0;
        // 连续请求失败的次数；用于同优先级 miss 的轮转，避免高优先级页
        // 长期抢不到安全槽位时把其他页饿死。
        uint32_t deferredRequestCount = 0;
    };

    struct Stats
    {
        uint64_t requestCount = 0;
        uint64_t hitCount = 0;
        uint64_t missCount = 0;
        uint64_t evictionCount = 0;
        // preload 期间因容量/引用/pin 无法驻留的页数。它们保持非驻留，
        // 交给后续 RequestPage/feedback 重试，不会阻塞注册。
        uint64_t preloadDeferredCount = 0;
    };

    // C2c GPU 上传描述：只说明“哪些页需要写到 GPU”，不执行任何 Vulkan 调用。
    // virtualPageX/Y 是页表 texel 的 imageOffset；entry 才是 atlas 物理槽位坐标。
    // entry 与页表 RGBA8 texel 的 4 个通道一一对应，消费端可整 texel 拷贝。
    struct PageTableUpload
    {
        uint32_t virtualPageX = 0;
        uint32_t virtualPageY = 0;
        uint8_t entry[4] = {0, 0, 0, 0};
        // 收集时的页表槽位版本；Commit 用它拒绝“收集后又被改脏”的旧上传。
        uint64_t revision = 0;
    };

    // 单页 ID/权重数据，长度均为 PageResolution² * 4 字节；坐标是 atlas 物理槽位。
    struct AtlasPageUpload
    {
        uint32_t physicalPageX = 0;
        uint32_t physicalPageY = 0;
        // 收集时的物理槽位版本；语义同 PageTableUpload::revision。
        uint64_t revision = 0;
        std::vector<uint8_t> ids;
        std::vector<uint8_t> weights;
    };

    TerrainPageManager();

    // preload=true 保持 C2b 的“注册即全量常驻”；false 时页面由 RequestPage 按需生成。
    // 容量不足时 preload 会尽量驻留并在每次失败后继续，不会死循环；失败数记在
    // Stats::preloadDeferredCount，剩余页保持非驻留。
    void RegisterTerrain(const TerrainBuildPlan& plan, bool preload = true);

    // P3 流式加载的初始可见集合：只请求 placement 中心 tilesPerSide² 页，
    // 其余页交给 feedback。返回成功驻留的页数；这是“首帧不全灰”的最小集合，
    // 不是全量 preload。
    uint32_t RequestInitialTerrainPages(
        const Registration& registration,
        uint32_t tilesPerSide = 2,
        uint32_t priority = 0);

    // 未驻留时按 tile 生成 page 数据并写入 atlas；frame 参与 LRU，priority 只升不降。
    // 第一个参数是 placementId（见 Registration::placementId），不是 assetPath。
    bool RequestPage(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ,
        uint64_t frame = 0,
        uint32_t priority = 0);

    // streaming 入口：与 RequestPage 相同的命中/烘焙语义，但容量或物理槽位
    // 不足时只淘汰 lastSeenEpoch 已过安全窗的页；没有安全候选就返回 false，
    // 把请求留到下一帧重试。旧 RequestPage 保留给 preload/加载期，仍走无窗口 LRU。
    bool RequestPageAtEpoch(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ,
        uint64_t frame,
        uint32_t priority,
        uint64_t completedEpoch);

    // 只更新已驻留页的使用时间；未驻留页返回 false。
    bool MarkUsed(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ,
        uint64_t frame);
    bool SetPagePriority(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ,
        uint32_t priority);

    // streaming：记录页最近一次被 feedback/预取看到的 epoch，并可选提升 priority。
    bool MarkPageUsedAtEpoch(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ,
        uint64_t epoch,
        uint32_t priority = 0);

    // 只有满足安全窗、未被引用且未 pin 的常驻页才允许自动淘汰。
    bool CanEvictPageAt(
        const PageKey& key,
        uint64_t completedEpoch) const;
    bool CanEvictAt(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ,
        uint64_t completedEpoch) const;

    // 本帧请求的页 pin 到采样/上传完成；pin 期间显式与自动淘汰都会失败。
    // Pin/Unpin 成对计数；Unpin 只释放本调用方持有的一次 pin。
    bool PinPageForUpload(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ);
    bool UnpinPage(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ);
    bool IsPagePinned(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ) const;

    // 保存 GPU epoch 上下文。新驻留页以本帧已提交 epoch 作为初始 lastSeenEpoch
    // （没有提交过时退回已消费 feedback epoch），避免新页出生即过期；
    // 旧 RequestPage/容量/LRU 行为不变，安全窗只由 RequestPageAtEpoch 与
    // EvictLeastRecentlyUsed(completedEpoch) 消费。
    void SetGpuEpochContext(
        uint64_t lastSubmittedEpoch,
        uint64_t lastConsumedFeedbackEpoch,
        uint32_t framesInFlight);
    uint64_t LastSubmittedEpoch() const { return gpuLastSubmittedEpoch; }
    uint64_t LastConsumedFeedbackEpoch() const { return gpuLastConsumedFeedbackEpoch; }
    uint32_t FramesInFlight() const { return gpuFramesInFlight; }
    // 当前淘汰安全窗：max(EvictionHysteresisFrames, framesInFlight + 1)。
    // 所有 epoch 安全窗判断必须使用它，而不是固定 +2。
    uint64_t EvictionWindowFrames() const;

    // 被引用的页不会被自动淘汰；显式 EvictPage 也拒绝回收仍有引用的页。
    bool AddPageReference(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ);
    bool ReleasePageReference(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ);

    // 淘汰失败只发生在页未驻留、仍有引用，或没有可淘汰页时。
    bool EvictPage(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ);
    // 旧入口：保持既有 LRU/容量行为，不应用 GPU 安全窗。
    bool EvictLeastRecentlyUsed();
    // streaming 入口：只淘汰 lastSeenEpoch + EvictionWindowFrames() <= completedEpoch
    // 的安全候选；没有安全候选时返回 false，把 miss 留到下一帧重试。
    bool EvictLeastRecentlyUsed(uint64_t completedEpoch);

    // 常驻页预算；manager 自身初值等于可用物理槽位数，生产加载路径随后设为
    // DefaultResidentCapacity(512)。降低预算会立即按 LRU 淘汰多余常驻页。
    void SetResidentCapacity(uint32_t capacity);
    // streaming 入口：降预算只淘汰安全窗外的页；没有安全候选时允许驻留数暂时
    // 超过预算并停止，等后续帧再次调用，不会死循环。
    void SetResidentCapacityAtEpoch(uint32_t capacity, uint64_t completedEpoch);
    uint32_t ResidentCapacity() const { return residentCapacity; }
    // 当前可立即分配的空物理 atlas 槽位数：被常驻页占用或仍在 quarantine 的
    // 槽位都不算，fallback 槽位也不计入。过期的 quarantine 需先经过
    // ReclaimExpiredQuarantines（AllocatePhysicalSlot 会自动调用）才回到这里。
    uint32_t ImmediatelyAllocatablePhysicalSlotCount() const
    {
        return freePhysicalSlotCount;
    }
    // 旧名兼容别名；新代码与测试使用 ImmediatelyAllocatablePhysicalSlotCount。
    uint32_t AvailablePhysicalSlotCount() const
    {
        return ImmediatelyAllocatablePhysicalSlotCount();
    }
    // 已淘汰但仍在 GPU 安全隔离期的物理槽位数；这些槽位不能立刻分配。
    uint32_t QuarantinedPhysicalSlotCount() const { return quarantinedPhysicalSlotCount; }
    // 回收 releaseEpoch <= completedEpoch 的隔离槽位，返回本次回收数量。
    // 回收后槽位变为普通空闲槽位（occupancy=0、free++、quarantine--、
    // releaseEpoch=0）；fallback 物理槽位永不回收；重复调用不重复计数。
    uint32_t ReclaimExpiredQuarantines(uint64_t completedEpoch);
    // 槽位隔离到哪个 completedEpoch 之后才允许复用；未隔离返回 0。
    uint64_t PhysicalSlotReleaseEpoch(
        uint32_t physicalPageX,
        uint32_t physicalPageY) const;
    // 在给定 completedEpoch 下该物理槽位是否可分配。fallback 槽位恒为 false。
    bool IsPhysicalSlotAvailable(
        uint32_t physicalPageX,
        uint32_t physicalPageY,
        uint64_t completedEpoch) const;

    // P3 闭环的 CPU 决策：feedback 页 -> MarkPageUsedAtEpoch / RequestPageAtEpoch。
    // 只处理已注册虚拟页；未注册页（例如 M_pbr 地形）跳过并计数，不抛错。
    // 请求按 priority 降序、deferred 次数升序，每帧最多 maxRequestsPerFrame 个；
    // 失败项留在 pendingRequests，下一帧重新排序后重试。
    TerrainFeedbackApplyStats ApplyFeedback(
        const TerrainFeedbackFrameSummary& summary,
        uint64_t completedEpoch,
        uint32_t maxRequestsPerFrame,
        std::vector<TerrainPageRequest>& pendingRequests);

    // 按虚拟页表坐标查询：只看页表项 A 通道，与物理槽位分配无关。
    bool IsResident(uint32_t virtualPageX, uint32_t virtualPageY) const;
    bool IsPageResident(const PageKey& key) const;
    const PageState* FindPage(const PageKey& key) const;
    const PageState* FindPageByVirtual(uint32_t virtualPageX, uint32_t virtualPageY) const;
    const std::vector<PageState>& Pages() const { return pages; }

    // L4a：读取一个覆盖页的 top-4 层 ID/权重（RGBA8，PageResolution²）。
    // 常驻页直接读 CPU atlas 镜像；allowRebuild=true 且页未驻留时用与
    // RequestPage 完全相同的烘焙 kernel 临时重建，不写 atlas、不改变驻留
    // 状态。找不到注册/计划时返回 false。
    bool ReadPageLayers(
        const PageKey& key,
        bool allowRebuild,
        std::vector<uint8_t>& ids,
        std::vector<uint8_t>& weights) const;

    uint32_t ResidentCount() const { return residentCount; }
    uint64_t RequestCount() const { return stats.requestCount; }
    uint64_t PageHitCount() const { return stats.hitCount; }
    uint64_t PageMissCount() const { return stats.missCount; }
    uint64_t EvictionCount() const { return stats.evictionCount; }
    uint64_t PreloadDeferredCount() const { return stats.preloadDeferredCount; }
    const Stats& GetStats() const { return stats; }

    const std::vector<uint8_t>& PageTable() const { return pageTable; }
    const std::vector<uint8_t>& IdAtlas() const { return idAtlas; }
    const std::vector<uint8_t>& WeightAtlas() const { return weightAtlas; }
    const std::vector<Registration>& Registrations() const { return registrations; }

    // 取走自上次消费以来的所有变更页，按页表槽位升序输出；返回是否非空。
    // 输出 vector 会先被清空，但不会清 dirty；调用方在 GPU 上传成功后再 Commit。
    bool CollectDirtyUploads(
        std::vector<PageTableUpload>& pageTableUploads,
        std::vector<AtlasPageUpload>& atlasUploads);
    // 上传成功后清 dirty；未提交的页仍保持 dirty，便于重试。
    void CommitDirtyUploads(
        const std::vector<PageTableUpload>& pageTableUploads,
        const std::vector<AtlasPageUpload>& atlasUploads);
    // 兼容便捷入口：Collect + Commit，仅用于一次性同步消费者/测试。
    bool ConsumeDirtyUploads(
        std::vector<PageTableUpload>& pageTableUploads,
        std::vector<AtlasPageUpload>& atlasUploads);
    bool HasDirtyUploads() const;

    // P1：可回滚的上传批次。Build 只收集、给上传项打 revision 并 pin 涉及页，
    // 不直接清 dirty；GPU 提交成功后才 Commit，失败则 Abort 只解 pin、保留 dirty。
    // maxAtlasPages = 0 表示不限；非 0 时按物理槽位升序截断，并把“指向被截断
    // 物理槽位”的页表项一起留下，保证页表不会先于 atlas 内容生效。
    TerrainUploadBatch BuildUploadBatch(uint32_t maxAtlasPages = 0);
    void CommitUploadBatch(const TerrainUploadBatch& batch);
    void AbortUploadBatch(const TerrainUploadBatch& batch);

private:
    bool RequestPageInternal(
        const std::string& placementId,
        uint32_t tileX,
        uint32_t tileZ,
        uint64_t frame,
        uint32_t priority,
        bool useEpochWindow,
        uint64_t completedEpoch);
    void EnsureStorage();
    size_t FindRegistrationIndex(const std::string& placementId) const;
    PageState* FindPageMutable(const PageKey& key);
    bool MakeResident(
        size_t registrationIndex,
        PageState& page,
        uint64_t frame,
        uint32_t priority,
        uint64_t completedEpoch);
    bool EvictPageAt(PageState& page);
    bool AllocatePhysicalSlot(
        uint32_t& physicalPageX,
        uint32_t& physicalPageY,
        uint64_t completedEpoch);
    // 淘汰时只进入 quarantine，不立即清零/上传 atlas；旧 in-flight 帧仍可能
    // 采样旧内容，等 releaseEpoch 过后新页再用新内容整体覆盖。
    void QuarantinePhysicalSlot(
        uint32_t physicalPageX,
        uint32_t physicalPageY,
        uint64_t lastSeenEpoch);
    void ZeroPageTableEntry(uint32_t virtualPageX, uint32_t virtualPageY);
    size_t FindEvictionCandidateIndex() const;
    size_t FindEvictionCandidateIndex(uint64_t completedEpoch) const;
    void MarkPageTableDirty(uint32_t virtualPageX, uint32_t virtualPageY);
    void MarkAtlasPageDirty(uint32_t physicalPageX, uint32_t physicalPageY);
    PageState* FindPageByVirtualMutable(uint32_t virtualPageX, uint32_t virtualPageY);
    PageState* FindPageByPhysicalMutable(uint32_t physicalPageX, uint32_t physicalPageY);
    const TerrainBuildPlan* FindPlan(const std::string& placementId) const;
    void CopyAtlasPage(
        uint32_t physicalPageX,
        uint32_t physicalPageY,
        const std::vector<uint8_t>& atlas,
        std::vector<uint8_t>& destination) const;

    std::vector<uint8_t> pageTable;
    std::vector<uint8_t> idAtlas;
    std::vector<uint8_t> weightAtlas;
    // 页表脏标记按虚拟页槽位记录，atlas 脏标记按物理槽位记录；两者语义不同，
    // 但都只有 PageSlotCount 项。只描述 CPU 侧待上传状态，不持有 GPU 资源。
    std::vector<uint8_t> virtualPageTableDirty;
    std::vector<uint8_t> physicalAtlasDirty;
    // 每个页表/atlas 槽位的内容版本。上传描述携带收集时的版本，Commit 只在版本
    // 未变化时清 dirty，避免“收集后又被改脏”的旧数据把新内容标记为已上传。
    std::vector<uint64_t> virtualPageTableRevision;
    std::vector<uint64_t> physicalAtlasRevision;
    // physicalOccupancy 表示 atlas 物理槽位是否被常驻页占用；
    // virtualReserved 表示虚拟页坐标已被 registration 预留，两者必须分开，
    // 否则淘汰释放物理槽位时会误伤“虚拟坐标已被地形注册”的重叠检查。
    std::vector<uint8_t> physicalOccupancy;
    std::vector<uint8_t> virtualReserved;
    // physicalSlotQuarantined=1 表示槽位已淘汰但 GPU 可能仍在采样；
    // releaseEpoch 由淘汰时的 submitted epoch + framesInFlight 推导。
    std::vector<uint8_t> physicalSlotQuarantined;
    std::vector<uint64_t> physicalSlotReleaseEpoch;
    std::vector<Registration> registrations;
    std::vector<TerrainBuildPlan> plans;
    std::vector<PageState> pages;
    Stats stats;
    uint32_t residentCount = 0;
    uint32_t freePhysicalSlotCount = UsablePhysicalSlotCount;
    uint32_t quarantinedPhysicalSlotCount = 0;
    uint32_t residentCapacity = UsablePhysicalSlotCount;
    uint64_t gpuLastSubmittedEpoch = 0;
    uint64_t gpuLastConsumedFeedbackEpoch = 0;
    uint32_t gpuFramesInFlight = 0;
    uint64_t nextUploadBatchId = 1;
};

// 一次 GPU 上传事务的 CPU 描述。batch 只引用 page manager 内部状态的快照，
// 页内容必须在 Build 与 Commit 之间保持可读；pin 阻止这些页被淘汰/复用槽位。
struct TerrainUploadBatch
{
    uint64_t batchId = 0;
    std::vector<TerrainPageManager::PageTableUpload> pageTableUploads;
    std::vector<TerrainPageManager::AtlasPageUpload> atlasUploads;
    // Build 时本批次实际增加的 pin（每页去重一次）；Commit/Abort 只解除这些，
    // 不碰调用方或其他批次已经持有的 pin。
    std::vector<TerrainPageManager::PageKey> newlyPinnedPages;

    bool Empty() const noexcept
    {
        return pageTableUploads.empty() && atlasUploads.empty();
    }
};

} // namespace VL

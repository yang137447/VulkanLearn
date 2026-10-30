#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include "terrain/terrainAssetTypes.h"

namespace VL
{

struct TerrainLayerUploadBatch;

// 层纹理页的身份：全局层号 + mip + 该 mip 内的页坐标。
// 层源纹理是平铺资产，页内容与地形 tile 解耦；同一层页被所有地形共享。
struct TerrainLayerPageKey
{
    uint32_t layerId = 0;
    uint32_t mip = 0;
    uint32_t pageX = 0;
    uint32_t pageY = 0;

    bool operator==(const TerrainLayerPageKey& other) const noexcept
    {
        return layerId == other.layerId && mip == other.mip &&
            pageX == other.pageX && pageY == other.pageY;
    }
    // std::map 的稳定顺序：LRU 平局时按页身份排序，保证测试/结果可复现。
    bool operator<(const TerrainLayerPageKey& other) const noexcept
    {
        if (layerId != other.layerId) return layerId < other.layerId;
        if (mip != other.mip) return mip < other.mip;
        if (pageY != other.pageY) return pageY < other.pageY;
        return pageX < other.pageX;
    }
};

// L1：纯 CPU 的层页驻留/淘汰/上传批次决策。这里不持有任何像素：
// 页内容由 L2 从源纹理 GPU copy 到 atlas，本类只回答“哪个虚拟页现在
// 映射到哪个物理页、哪些页需要上传、什么时候可以复用物理页”。
//
// 两套编号必须区分：
//   virtual key  = (layerId, mip, pageX, pageY)；
//   物理页号     = 0..physicalPageCount-1，最后一个槽位固定留给 fallback。
// mip 页数（页 128²）：
//   512  -> mip0..2：4/2/1 页/边，21 页/层；
//   1024 -> mip0..3：8/4/2/1 页/边，85 页/层；
//   2048 -> mip0..4：16/8/4/2/1 页/边，341 页/层（mip4 是 128² 尾页）。
class TerrainLayerPageManager
{
public:
    static constexpr uint32_t MaxLayers = 256;
    static constexpr uint32_t MaxMipLevel = TerrainLayerMaxMipLevel;
    static constexpr uint32_t PageResolution = TerrainPageResolution;
    // 4096² atlas / 128² 页 = 32×32 页/ slice；物理页号按 slice 平铺。
    static constexpr uint32_t TilesPerAtlasSide = 32;
    static constexpr uint32_t PagesPerAtlasSlice =
        TilesPerAtlasSide * TilesPerAtlasSide;
    static constexpr uint32_t DefaultPhysicalPageCount = 1024;
    // 与覆盖页 P3 的实测预算一致：512 足以覆盖 32×32 宽视野的可见层页工作集。
    static constexpr uint32_t DefaultResidentCapacity = 512;
    static constexpr uint64_t EvictionHysteresisFrames = 2;
    static constexpr uint32_t InvalidPhysicalPage = 0xFFFFFFFFu;

    struct PageState
    {
        TerrainLayerPageKey key;
        bool resident = false;
        uint32_t physicalPage = InvalidPhysicalPage;
        // 旧 LRU 语义（调用方帧号）；epoch-aware 排序优先看 lastTouchedEpoch。
        uint64_t lastUsedFrame = 0;
        // 最近一次被 feedback/预取看到的 GPU epoch，决定能否安全淘汰。
        uint64_t lastSeenEpoch = 0;
        // 最近一次触碰（feedback 命中/新驻留），决定同优先级下先淘汰谁。
        uint64_t lastTouchedEpoch = 0;
        // 内容版本；uploadedRevision 追上 contentRevision 才表示 GPU 已同步。
        uint64_t contentRevision = 0;
        uint64_t uploadedRevision = 0;
        // 页表映射版本：驻留/淘汰都会推进。与 contentRevision 分开，
        // 因为“只换了物理页但像素已上传”的页仍需重写页表项。
        uint64_t pageTableRevision = 0;
        uint64_t pageTableUploadedRevision = 0;
        // 重叠上传批次各持一份 pin，Commit/Abort 只能释放自己那一份。
        uint32_t uploadPinCount = 0;
        uint32_t priority = 0;
        uint32_t referenceCount = 0;
        uint32_t deferredRequestCount = 0;
    };

    struct Stats
    {
        uint64_t requestCount = 0;
        uint64_t hitCount = 0;
        uint64_t missCount = 0;
        uint64_t evictionCount = 0;
        // 因容量/物理槽位不足且没有安全可淘汰页而推迟的请求数。
        uint64_t deferredCount = 0;
        // 已成功 Commit 的页上传次数（一页多次上传会累加）。
        uint64_t uploadedPageCount = 0;
        // 已成功 Commit 的页表项写入次数（含失效项）。
        uint64_t uploadedPageTableEntryCount = 0;
        // 以下是快照时的实时值，不是累计计数。
        uint32_t residentPageCount = 0;
        uint32_t residentCapacity = 0;
        uint32_t freePhysicalPageCount = 0;
        uint32_t quarantinedPhysicalPageCount = 0;
    };

    // L2 的 GPU 上传描述：只有页身份与物理页号，没有像素内容。
    struct PageUpload
    {
        TerrainLayerPageKey key;
        // 设计 §4.4 的 per-mip 虚拟页号；L2 调试/页表写入直接可用。
        uint64_t virtualPageIndex = 0;
        uint32_t physicalPage = InvalidPhysicalPage;
        // 收集时的 contentRevision；Commit 只在版本未变时标记已上传。
        uint64_t revision = 0;
    };

    // L2 的页表上传描述：valid=false 表示淘汰页必须把对应虚拟项写成零，
    // 否则 GPU 仍会通过旧映射采样已隔离/复用的物理页。
    struct PageTableUpdate
    {
        TerrainLayerPageKey key;
        uint64_t virtualPageIndex = 0;
        // RGBA32F texel：R=物理页、G=有效 mip、B=valid/flags、A=priority。
        std::array<float, 4> entry{0.0f, 0.0f, 0.0f, 0.0f};
        bool valid = false;
        uint64_t revision = 0;
        uint32_t physicalPage = InvalidPhysicalPage;
    };

    struct PageTableLayout
    {
        uint32_t width = 1;
        uint32_t height = 1;
    };

    // physicalPage -> (slice, tileX, tileY)，供 L2 计算 atlas 子区域。
    struct PhysicalPageAddress
    {
        uint32_t slice = 0;
        uint32_t tileX = 0;
        uint32_t tileY = 0;
    };

    explicit TerrainLayerPageManager(
        uint32_t physicalPageCount = DefaultPhysicalPageCount);

    // 配置每个 layerId 的源分辨率；未配置的层按 1024 处理。
    // 有页状态后重新配置会让虚拟页号失效，因此拒绝。
    void ConfigureLayers(const std::vector<TerrainLayerDesc>& layers);

    // 纯函数 helper：与 ConfigureLayers 解耦，测试和 L2 可以直接复用。
    static uint32_t PagesPerSideForSource(uint32_t sourceResolution, uint32_t mip);
    static uint32_t PagesPerMipForSource(uint32_t sourceResolution, uint32_t mip);
    static uint32_t TotalPagesForSource(uint32_t sourceResolution);
    static PhysicalPageAddress DecodePhysicalPage(uint32_t physicalPage);
    // 每 mip 一张页表；宽度上限 4096（规范保证的 2D 尺寸），高度按
    // ceil(entryCount / width) 展开。entryCount 只依赖混合分辨率的最大值，
    // 因此同一 World 内布局稳定，shader 可以用 `index % width` 定位。
    static PageTableLayout PageTableLayoutForEntries(uint32_t entryCount);
    PageTableLayout PageTableLayoutForMip(uint32_t mip) const;
    // 页表 texel 的统一编码/解码；无效项四个通道全零。
    static std::array<float, 4> EncodePageTableEntry(
        uint32_t physicalPage,
        uint32_t mip,
        bool valid,
        uint32_t priority);

    uint32_t LayerSourceResolution(uint32_t layerId) const;
    // catalog row2.z 的 CPU 对应值：该全局层从哪一组源数组上传。
    uint32_t LayerSourceSet(uint32_t layerId) const;
    uint32_t PagesPerSide(uint32_t layerId, uint32_t mip) const;
    uint32_t PagesPerMip(uint32_t layerId, uint32_t mip) const;
    uint32_t TotalPagesForLayer(uint32_t layerId) const;
    // catalog row2 的 pageTableBase：前面所有层的总页数之和。
    uint32_t PageTableBase(uint32_t layerId) const;
    // 设计 §4.4 的“按 mip 分表”编号：
    //   virtualPage(mip) = layerId * mipStride(mip) + pageY * side + pageX
    // mipStride 取该 mip 下所有已配置层的最大 pagesPerMip，因此 512/1K/2K
    // 混用时不同层的页号不会别名；全同分辨率时与设计公式完全一致。
    uint64_t VirtualPageIndex(const TerrainLayerPageKey& key) const;
    // 每层在该 mip 的页表槽位数（即 mipStride）与整张页表的槽位总数。
    uint32_t VirtualPagesPerLayer(uint32_t mip) const;
    uint32_t VirtualPageTableSize(uint32_t mip) const;
    // catalog pageTableBase 对应的扁平编号（每层一张表时使用），
    // 便于 L2 选择单表变体；当前主路径用 VirtualPageIndex。
    uint64_t FlatPageIndex(const TerrainLayerPageKey& key) const;

    // 越界 key 抛 std::out_of_range / std::invalid_argument。
    void ValidateKey(const TerrainLayerPageKey& key) const;

    // streaming 入口：容量/物理页不足时只淘汰安全窗外的页，没有安全候选
    // 就返回 false 并计入 deferred，把请求留到下一帧。
    // 调用方必须在每帧请求前调用 SetGpuEpochContext；否则新驻留页以 0 起算，
    // 安全窗保护会失效（与 TerrainPageManager 的 P3 前置条件相同）。
    bool RequestPageAtEpoch(
        const TerrainLayerPageKey& key,
        uint64_t frame,
        uint32_t priority,
        uint64_t completedEpoch);
    // 加载期/preload 入口：不做 GPU 安全窗检查，可立即回收隔离槽位。
    bool RequestPage(
        const TerrainLayerPageKey& key,
        uint64_t frame = 0,
        uint32_t priority = 0);

    // feedback 命中：推进 lastSeen/lastTouched，priority 只升不降。
    bool MarkPageUsedAtEpoch(
        const TerrainLayerPageKey& key,
        uint64_t epoch,
        uint32_t priority = 0);

    bool CanEvictPageAt(
        const TerrainLayerPageKey& key,
        uint64_t completedEpoch) const;
    // 显式淘汰不检查 GPU 安全窗（与 TerrainPageManager 一致），只拒绝
    // reference/pin。streaming 路径应使用 EvictLeastRecentlyUsed(epoch)。
    bool EvictPage(const TerrainLayerPageKey& key);
    bool EvictLeastRecentlyUsed(uint64_t completedEpoch);

    bool PinPageForUpload(const TerrainLayerPageKey& key);
    bool UnpinPage(const TerrainLayerPageKey& key);
    bool IsPagePinned(const TerrainLayerPageKey& key) const;
    bool AddPageReference(const TerrainLayerPageKey& key);
    bool ReleasePageReference(const TerrainLayerPageKey& key);

    // GPU epoch 上下文：新驻留页以 lastSubmittedEpoch 起算，避免出生即过期。
    // 两参重载等价于 lastConsumedFeedbackEpoch = 0；三参与 TerrainPageManager
    // 对齐，加载期没有提交过时退回已消费 feedback epoch。
    void SetGpuEpochContext(uint64_t lastSubmittedEpoch, uint32_t framesInFlight);
    void SetGpuEpochContext(
        uint64_t lastSubmittedEpoch,
        uint64_t lastConsumedFeedbackEpoch,
        uint32_t framesInFlight);
    uint64_t LastSubmittedEpoch() const { return gpuLastSubmittedEpoch; }
    uint32_t FramesInFlight() const { return gpuFramesInFlight; }
    // 淘汰安全窗 = max(2, framesInFlight + 1)，同时覆盖 feedback 回读延迟
    // 与提交队列中的 in-flight 帧。
    uint64_t EvictionWindowFrames() const;

    // 降预算：只淘汰安全窗外的页；没有安全候选时允许暂时超预算。
    void SetResidentCapacityAtEpoch(uint32_t capacity, uint64_t completedEpoch);
    void SetResidentCapacity(uint32_t capacity);
    uint32_t ResidentCapacity() const { return residentCapacity; }

    // fallback 物理页固定为最后一个槽位，永不参与普通分配/回收。
    uint32_t FallbackPhysicalPage() const { return physicalPageCount - 1; }
    uint32_t PhysicalPageCount() const { return physicalPageCount; }
    uint32_t UsablePhysicalPageCount() const { return physicalPageCount - 1; }
    uint32_t FreePhysicalPageCount() const { return freePhysicalPageCount; }
    uint32_t QuarantinedPhysicalPageCount() const
    {
        return quarantinedPhysicalPageCount;
    }
    // 未隔离返回 0；否则返回允许复用该物理页的 completedEpoch 门槛。
    uint64_t PhysicalPageReleaseEpoch(uint32_t physicalPage) const;
    bool IsPhysicalPageAvailable(
        uint32_t physicalPage,
        uint64_t completedEpoch) const;
    // 回收 releaseEpoch <= completedEpoch 的隔离槽位，返回本次回收数量。
    uint32_t ReclaimExpiredQuarantines(uint64_t completedEpoch);

    // 上传批次与 P1 语义一致：Build 只收集并 pin；Commit 只在 revision
    // 未变时清脏并解除本批 pin；Abort 只解除本批 pin，保留脏状态。
    // maxPages = 0 表示不限；非 0 时按物理页号升序截断，剩余页下一批继续。
    TerrainLayerUploadBatch BuildUploadBatch(uint32_t maxPages = 0);
    void CommitUploadBatch(const TerrainLayerUploadBatch& batch);
    void AbortUploadBatch(const TerrainLayerUploadBatch& batch);
    uint32_t DirtyPageCount() const;
    bool HasDirtyPages() const { return DirtyPageCount() != 0; }
    uint32_t DirtyPageTableEntryCount() const;

    bool IsResident(const TerrainLayerPageKey& key) const;
    const PageState* FindPage(const TerrainLayerPageKey& key) const;
    const std::map<TerrainLayerPageKey, PageState>& Pages() const { return pages; }
    uint32_t ResidentCount() const { return residentPageCount; }
    Stats SnapshotStats() const;

private:
    PageState& AcquirePageState(const TerrainLayerPageKey& key);
    void RebuildLayerLayout();
    bool RequestPageInternal(
        const TerrainLayerPageKey& key,
        uint64_t frame,
        uint32_t priority,
        bool useEpochWindow,
        uint64_t completedEpoch);
    bool MakeResident(
        PageState& page,
        uint64_t frame,
        uint32_t priority,
        uint64_t completedEpoch);
    bool AllocatePhysicalPage(
        uint32_t& physicalPage,
        uint64_t completedEpoch);
    void QuarantinePhysicalPage(
        uint32_t physicalPage,
        uint64_t lastSeenEpoch);
    void ReleasePhysicalPage(PageState& page);
    PageState* FindEvictionCandidate(uint64_t completedEpoch);
    bool EvictPageAt(PageState& page);

    uint32_t physicalPageCount = DefaultPhysicalPageCount;
    uint32_t residentCapacity = DefaultResidentCapacity;
    uint32_t residentPageCount = 0;
    uint32_t freePhysicalPageCount = 0;
    uint32_t quarantinedPhysicalPageCount = 0;
    uint64_t gpuLastSubmittedEpoch = 0;
    uint64_t gpuLastConsumedFeedbackEpoch = 0;
    uint32_t gpuFramesInFlight = 0;
    uint64_t nextUploadBatchId = 1;

    // 每个 layerId 的源分辨率；默认 1024。
    std::vector<uint32_t> layerSourceResolution;
    std::vector<uint32_t> layerSourceSet;
    // 每个 layerId 的总页数前缀和（catalog pageTableBase）。
    std::vector<uint32_t> layerPageTableBase;
    // 每个 mip 的统一页号 stride（所有已配置层该 mip 的最大 pagesPerMip）。
    std::vector<uint32_t> layerMipStride;
    // 所有被请求过的页（含非驻留），保留 revision/pin/deferred 状态。
    std::map<TerrainLayerPageKey, PageState> pages;
    // 物理槽位状态：occupancy 覆盖常驻 + 隔离 + fallback；
    // quarantined 表示淘汰后仍可能被 in-flight 帧采样，releaseEpoch 前不可复用。
    std::vector<uint8_t> physicalOccupancy;
    std::vector<uint8_t> physicalQuarantined;
    std::vector<uint64_t> physicalReleaseEpoch;
    Stats stats;
};

// 一次 CPU 侧上传事务。batch 只引用页身份；调用方在 Commit/Abort 之前必须
// 保证 page manager 未被并发修改（与 P1 相同的单 in-flight 约束）。
struct TerrainLayerUploadBatch
{
    uint64_t batchId = 0;
    std::vector<TerrainLayerPageManager::PageUpload> uploads;
    // 页表写入项（含淘汰失效项）；与 uploads 独立，因为“只改映射”的页
    // 不需要再上传 atlas 像素。
    std::vector<TerrainLayerPageManager::PageTableUpdate> pageTableUpdates;
    // Build 时本批实际增加的 pin（每页去重一次）。
    std::vector<TerrainLayerPageKey> newlyPinnedPages;

    bool Empty() const noexcept
    {
        return uploads.empty() && pageTableUpdates.empty();
    }
};

} // namespace VL

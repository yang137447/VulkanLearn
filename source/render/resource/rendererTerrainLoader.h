#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>
#include <vulkan/vulkan.hpp>

#include "scene/sceneAssetTypes.h"
#include "terrain/terrainFeedback.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainLayerVisibility.h"
#include "terrain/terrainPageManager.h"

class PipelineFactory;
class Texture;
struct MeshObjectBuildPlan;

namespace VL
{
class RendererBackendVulkan;
class TerrainFeedbackGpuRing;
struct RendererResourceLoadContext;

struct TerrainPageGpuResources
{
    std::shared_ptr<Texture> pageTable;
    std::shared_ptr<Texture> layerIds;
    std::shared_ptr<Texture> layerWeights;
};

struct TerrainLayerGpuResources
{
    std::shared_ptr<Texture> layerTable;
    // L2：每 mip 一张 RGBA32F 页表（nearest / clamp / 无 mip）。
    std::array<
        std::shared_ptr<Texture>,
        TerrainLayerPageManager::MaxMipLevel + 1> pageTables;
    // 三张 layer atlas 都是 4096² × slice 的 sampler2DArray，无 mip。
    std::shared_ptr<Texture> albedoAtlas;
    std::shared_ptr<Texture> normalAtlas;
    std::shared_ptr<Texture> pbrAtlas;
    uint32_t atlasSlices = 0;
    uint32_t physicalPageCount = 0;
    vk::Format albedoFormat = vk::Format::eUndefined;
    vk::Format normalFormat = vk::Format::eUndefined;
    vk::Format pbrFormat = vk::Format::eUndefined;
    // 单张 atlas（全部 slice）的估算字节数；统计按源格式计算。
    uint64_t atlasBytesPerMap = 0;
    uint64_t albedoBytes = 0;
    uint64_t normalBytes = 0;
    uint64_t pbrBytes = 0;
    uint64_t atlasBytesTotal = 0;
    bool atlasesCreated = false;
};

// 层源数组纹理：L2 要求一个 World 内所有 layer-table 地形共享同一套层纹理。
// 源纹理可能带完整 mip 链；页内 copy 只在源 mip 存在时执行。
struct TerrainLayerSourceTextures
{
    std::shared_ptr<Texture> albedo;
    std::shared_ptr<Texture> normal;
    std::shared_ptr<Texture> pbr;
    uint32_t arrayLayers = 0;
    uint32_t albedoMipLevels = 0;
    uint32_t normalMipLevels = 0;
    uint32_t pbrMipLevels = 0;
    // 实际源图尺寸（来自 backend 创建记录）；catalog 的 sourceResolution
    // 可能与之不一致，L2 按真实尺寸裁剪 copy extent。
    uint32_t albedoWidth = 0;
    uint32_t albedoHeight = 0;
    uint32_t normalWidth = 0;
    uint32_t normalHeight = 0;
    uint32_t pbrWidth = 0;
    uint32_t pbrHeight = 0;
    // L3：catalog 的 sourceResolution 必须等于真实源图边长，否则页表页号
    // 与实际像素区域不一致。校验失败只禁用 VT，不影响 mode 0。
    bool catalogResolutionMatches = true;
};

// 一个 staging buffer 内三段数据的 base offset；RecordTerrainPageUploads 用它们
// 构造批量 vk::BufferImageCopy，三段都由调用方按 16 字节对齐打包。
struct TerrainUploadStagingOffsets
{
    vk::DeviceSize pageTable = 0;
    vk::DeviceSize layerIds = 0;
    vk::DeviceSize layerWeights = 0;
};

// 每帧一条 host-visible/coherent staging buffer，槽数等于 frame fence 数。
// 同一槽位只有在 BeginFrame 等过对应 fence 后才会被 CPU 重写，避免覆盖
// GPU 尚未读取的上传内容；不创建 per-frame 临时 buffer，也不 waitIdle。
class TerrainUploadGpuRing
{
public:
    TerrainUploadGpuRing(
        RendererBackendVulkan& rendererBackend,
        uint32_t maxAtlasPages);
    ~TerrainUploadGpuRing();

    TerrainUploadGpuRing(const TerrainUploadGpuRing&) = delete;
    TerrainUploadGpuRing& operator=(const TerrainUploadGpuRing&) = delete;

    uint32_t SlotCount() const noexcept { return slotCount; }
    uint32_t MaxAtlasPages() const noexcept { return maxAtlasPages; }
    bool Fits(const TerrainUploadBatch& batch) const noexcept;

    // 把一整批页表项/ID/权重打包进该 frameIndex 的槽位，返回三段 base offset。
    // 调用方保证该槽位 fence 已完成（BeginFrame 已等待）。
    TerrainUploadStagingOffsets Write(
        uint32_t frameIndex,
        const TerrainUploadBatch& batch);
    vk::Buffer Buffer(uint32_t frameIndex) const;

private:
    RendererBackendVulkan* rendererBackend = nullptr;
    uint32_t slotCount = 0;
    uint32_t maxAtlasPages = 0;
    vk::DeviceSize slotBytes = 0;
    std::vector<vk::Buffer> buffers;
    std::vector<vk::DeviceMemory> memories;
    std::vector<void*> mapped;
};

// L4a：层页每帧上传 ring。层页上传只需要页表 texel 走 staging；atlas 像素
// 是 GPU→GPU copy，不占 staging。槽数等于 frame fence 数，常驻映射。
class TerrainLayerUploadGpuRing
{
public:
    TerrainLayerUploadGpuRing(
        RendererBackendVulkan& rendererBackend,
        const TerrainLayerPageManager& layerPageManager,
        uint32_t maxLayerPagesPerFrame);
    ~TerrainLayerUploadGpuRing();

    TerrainLayerUploadGpuRing(const TerrainLayerUploadGpuRing&) = delete;
    TerrainLayerUploadGpuRing& operator=(const TerrainLayerUploadGpuRing&) =
        delete;

    uint32_t SlotCount() const noexcept { return slotCount; }
    uint32_t MaxLayerPages() const noexcept { return maxLayerPages; }
    bool Fits(const TerrainLayerUploadBatch& batch) const noexcept;

    // 把本批全部页表项（16 B/项）打包进该 frameIndex 的槽位，返回 base offset。
    // 调用方保证该槽的 fence 已在 BeginFrame 等待过。
    vk::DeviceSize Write(
        uint32_t frameIndex,
        const TerrainLayerUploadBatch& batch);
    vk::Buffer Buffer(uint32_t frameIndex) const;

private:
    RendererBackendVulkan* rendererBackend = nullptr;
    uint32_t slotCount = 0;
    uint32_t maxLayerPages = 0;
    vk::DeviceSize slotBytes = 0;
    std::vector<vk::Buffer> buffers;
    std::vector<vk::DeviceMemory> memories;
    std::vector<void*> mapped;
};

struct TerrainStreamingStats
{
    uint32_t residentPages = 0;
    uint32_t residentCapacity = 0;
    uint32_t virtualPages = 0;
    uint32_t registrations = 0;
    uint32_t freePhysicalSlots = 0;
    uint32_t quarantinedPhysicalSlots = 0;
    uint64_t requestCount = 0;
    uint64_t hitCount = 0;
    uint64_t missCount = 0;
    uint64_t evictionCount = 0;
    uint64_t preloadDeferredCount = 0;
    // 累计成功写入 GPU 的 atlas 页数，以及本帧实际提交的页数。
    uint64_t uploadedPagesTotal = 0;
    uint32_t lastUploadedPages = 0;
    uint64_t lastFeedbackEpoch = 0;
    TerrainFeedbackApplyStats lastFeedbackApply;
    // L2 层页统计：atlas 已创建但 shader 尚未消费，用于确认布局/预算。
    uint32_t layerResidentPages = 0;
    uint32_t layerResidentCapacity = 0;
    uint32_t layerPhysicalPages = 0;
    uint32_t layerFreePhysicalPages = 0;
    uint32_t layerQuarantinedPhysicalPages = 0;
    uint64_t layerRequestCount = 0;
    uint64_t layerHitCount = 0;
    uint64_t layerMissCount = 0;
    uint64_t layerEvictionCount = 0;
    uint64_t layerUploadedPagesTotal = 0;
    uint64_t layerUploadedBytesTotal = 0;
    // L4a：可见层推导与逐帧预算的实时值。
    uint32_t layerVisibleLayers = 0;
    uint32_t layerLastRequestedPages = 0;
    uint32_t layerLastUploadedPages = 0;
    uint32_t layerPendingRequests = 0;
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> layerMipRequestCounts{};
    uint32_t layerDirtyPages = 0;
    uint32_t layerDirtyPageTableEntries = 0;
    uint32_t layerAtlasSlices = 0;
    uint64_t layerAtlasBytesPerMap = 0;
    uint64_t layerAtlasBytesTotal = 0;
    bool layerAtlasReady = false;
};

// World-local 地形流式状态：CPU 驻留、GPU 页资源和世界共享层表都随
// WorldLocalResourcePackage 一起发布/退休。
// 生命周期约束：
//   - 完整 World 候选必须创建新实例，不得继承 active 的 pageManager；
//   - Blueprint 扩展候选只读共享同一实例（它不改地形页）；
//   - MI 预览同样只读共享 active 实例：预览只替换材质/MI，若清空 state
//     会让 feedback ring、页表和驻留状态静默失效。
struct TerrainStreamingState
{
    TerrainPageManager pageManager;
    TerrainPageGpuResources pageResources;
    // L2：层页驻留与 GPU atlas/页表。与覆盖页完全独立；当前只做加载期
    // 小批上传，不接 feedback 驱动（L4）。
    TerrainLayerPageManager layerPageManager;
    TerrainLayerSourceTextures layerSources;
    TerrainLayerGpuResources layerResources;
    // 任一 layer-table MI 把 u_terrainLayerMode 设为 1 时置位；加载期据此
    // 建立反馈驱动的驻留策略（L4a）。mode 0 不会创建层页上传 ring。
    bool layerVTRequested = false;
    // L4a：覆盖页 feedback -> 可见层 -> mip -> 层页请求。pending 跨帧保留，
    // 失败项下一帧按 priority/deferred 重新排序后重试。
    TerrainLayerVisibilityResolver layerVisibilityResolver;
    std::vector<TerrainLayerPageRequest> pendingLayerPageRequests;
    std::shared_ptr<TerrainLayerUploadGpuRing> layerUploadRing;
    // 最近一帧的可见层推导统计，由渲染线程写、PublishStats 复制。
    TerrainLayerVisibilityStats latestLayerVisibility;
    uint32_t maxLayerPageRequestsPerFrame = 16;
    uint32_t maxLayerPageUploadsPerFrame = 8;
    uint64_t layerIdleEvictionEpochs = 180;
    // `terrainlayer preload` 的对照路径关闭 idle 淘汰；`terrainlayer capacity`
    // 重新打开，恢复 feedback 驱动的驻留语义。
    std::atomic<bool> layerIdleEvictionEnabled{true};
    // Console 排队、渲染线程帧首消费的层页预算。
    std::atomic<uint32_t> requestedLayerResidentCapacity{0};
    std::atomic<bool> requestedLayerPreload{false};
    // P2/P3 feedback 回读 ring：World-local，随 package 退休；P3 起其 summary
    // 经 ApplyFeedback 驱动命中/miss/RequestPageAtEpoch。
    std::shared_ptr<TerrainFeedbackGpuRing> feedbackRing;
    // 每帧上传 ring；BuildUploadBatch 的 CPU 快照写进当前 frameIndex 的槽位，
    // 在 scene pass 前随主帧提交。
    std::shared_ptr<TerrainUploadGpuRing> uploadRing;
    // World 级共享层表定义；GPU layerResources.layerTable 由它生成。
    std::vector<TerrainLayerDesc> layerDefinitions;
    // pendingUploadRevision 表示 CPU 内容最后变更的版本；
    // uploadedRevision 表示已经成功写入 GPU 的版本。两者相等才没有待上传内容。
    uint64_t pendingUploadRevision = 0;
    uint64_t uploadedRevision = 0;
    // feedback 消费到的最后 epoch；作为淘汰安全窗/统计输入。
    uint64_t lastConsumedFeedbackEpoch = 0;
    // 本帧 feedback 产生的 miss 请求跨帧保留，失败项不会丢。
    std::vector<TerrainPageRequest> pendingPageRequests;
    // 只在渲染线程写；PublishStats 时复制进线程安全快照。
    TerrainFeedbackApplyStats latestFeedbackApply;
    uint64_t uploadedPagesTotal = 0;
    uint32_t lastUploadedPages = 0;
    uint64_t layerUploadedPagesTotal = 0;
    uint64_t layerUploadedBytesTotal = 0;
    // 本帧成功 Commit 的层页数；PublishStats 后清零，只显示一帧。
    uint32_t lastLayerUploadedPages = 0;
    uint32_t maxPageRequestsPerFrame =
        TerrainPageManager::MaxPageRequestsPerFrame;
    uint32_t maxUploadPagesPerFrame =
        TerrainPageManager::MaxUploadPagesPerFrame;
    // Console 在 GT 写预算；渲染线程在帧首消费，避免跨线程直接改 page manager。
    std::atomic<uint32_t> requestedResidentCapacity{0};
    // 统计快照：渲染线程发布，Console 只读，不共享可变 page manager。
    mutable std::mutex statsMutex;
    TerrainStreamingStats statsSnapshot;

    void NotifyContentChanged() noexcept
    {
        ++pendingUploadRevision;
    }

    // 只前进不后退，重复确认同一版本是幂等的。
    void MarkUploaded(uint64_t revision) noexcept
    {
        uploadedRevision = std::max(uploadedRevision, revision);
    }

    bool HasPendingUpload() const noexcept
    {
        return uploadedRevision < pendingUploadRevision;
    }

    void RequestResidentCapacity(uint32_t capacity) noexcept
    {
        requestedResidentCapacity.store(capacity, std::memory_order_relaxed);
    }

    uint32_t ConsumeRequestedResidentCapacity() noexcept
    {
        return requestedResidentCapacity.exchange(0, std::memory_order_relaxed);
    }

    void PublishStats(const TerrainStreamingStats& stats)
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        statsSnapshot = stats;
    }

    TerrainStreamingStats SnapshotStats() const
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        return statsSnapshot;
    }
};

// 一次流式上传事务：Build 成功后 Begin；SubmitFrame 成功才 Commit。
// 析构时仍未 Commit（录制/提交流程抛异常）则 Abort，保留 dirty 与 pin 语义。
class TerrainUploadTransaction
{
public:
    void Begin(
        std::shared_ptr<TerrainStreamingState> streamingState,
        TerrainUploadBatch batch);
    void Commit();
    bool Active() const noexcept { return active; }
    ~TerrainUploadTransaction();

private:
    std::shared_ptr<TerrainStreamingState> streaming;
    TerrainUploadBatch batch;
    bool active = false;
};

// L4a：层页上传事务，语义与 TerrainUploadTransaction 相同，但提交到
// TerrainLayerPageManager，并累计层页上传统计。
class TerrainLayerUploadTransaction
{
public:
    void Begin(
        std::shared_ptr<TerrainStreamingState> streamingState,
        TerrainLayerUploadBatch batch);
    void Commit();
    bool Active() const noexcept { return active; }
    ~TerrainLayerUploadTransaction();

private:
    std::shared_ptr<TerrainStreamingState> streaming;
    TerrainLayerUploadBatch batch;
    uint32_t uploadedPageCount = 0;
    bool active = false;
};

// page table 始终创建（即使 World 没有地形），feedback pass 的
// worldTexture 输入需要一张稳定的 32×32 零页表。
std::shared_ptr<Texture> CreateTerrainPageTableGpuResource(
    RendererBackendVulkan& rendererBackend);

// page table 之外的 ID/weight atlas；只有真正注册了地形页时才创建。
void CreateTerrainPageAtlasGpuResources(
    RendererBackendVulkan& rendererBackend,
    TerrainPageGpuResources& resources);

TerrainPageGpuResources CreateTerrainPageGpuResources(
    RendererBackendVulkan& rendererBackend);

TerrainLayerGpuResources CreateTerrainLayerGpuResources(
    RendererBackendVulkan& rendererBackend,
    const std::vector<TerrainLayerDesc>& layers,
    const TerrainLayerPageManager& layerPageManager);

// L2：用源层数组的格式/层数创建三张空 atlas（4096² × slice，无 mip），
// 并用 record-only clear 把整张 atlas 置零；不做 CPU 零图上传。
void CreateTerrainLayerAtlasGpuResources(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState);

// 加载期初始测试集合：每层取源 mip 链的尾页（page 0,0），受物理页预算约束。
// 这是 L2 的映射验证集合，不是 L4 的 feedback 驻留策略。
void RequestInitialTerrainLayerPages(
    TerrainStreamingState& streamingState,
    uint64_t completedEpoch,
    uint32_t framesInFlight);

// L3 的 VT 验证路径：每层每个 mip 的全部页一次性请求。没有 feedback 驱动，
// 因此加载期必须建立完整映射。L4a 起只保留 debug console 的对照入口。
void RequestAllTerrainLayerPages(
    TerrainStreamingState& streamingState,
    uint64_t completedEpoch,
    uint32_t framesInFlight);

// L4a：debug console `terrainlayer preload` 的对照入口。请求不受每帧预算
// 限制；像素仍由逐帧上传 ring 分批写进 atlas，不回到加载期一次 submit。
void RequestTerrainLayerPreload(TerrainStreamingState& streamingState);

// 同步版：BuildUploadBatch -> 页表 staging -> 页表 copy + 源->atlas copy
// 全部录制进同一个 command buffer，一次 submit + wait 后 Commit。
// 返回本批成功上传的 atlas 页数；失败 Abort 保留 dirty。
size_t ApplyTerrainLayerPageUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceTextures& sources);

// 加载期入口：成功后推进 streaming state 的 uploadedRevision。
void ApplyTerrainLayerStreamingUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState);

// 消费 page manager 的 dirty 页，用 RHI 子区域 copy 增量写入 page table/atlas。
// 当前用于加载期首次上传；内部走 BuildUploadBatch -> 单 command buffer 录制 ->
// 一次 submit + wait -> Commit，失败 Abort 保留 dirty。
// 返回本批成功提交的 atlas 页数；加载期也计入 uploadedPagesTotal。
size_t ApplyTerrainPageUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainPageManager& pageManager,
    TerrainPageGpuResources& pageResources);

// 只录制，不 submit：把本批页表/ID/权重 copy 写进调用方的主帧 command buffer。
// 调用方负责 image 当前 layout 是 ShaderReadOnly、staging 内容已填充且存活到提交后。
// 三张 image 各自做一次 ShaderReadOnly -> TransferDst -> ShaderReadOnly 往返。
void RecordTerrainPageUploads(
    vk::CommandBuffer commandBuffer,
    TerrainPageGpuResources& pageResources,
    const TerrainUploadBatch& batch,
    vk::Buffer stagingBuffer,
    const TerrainUploadStagingOffsets& stagingOffsets);

// L4a：只录制层页上传（页表 + 三张 atlas 的 GPU→GPU copy），不 submit。
// 调用方负责 staging 内容已填充并存活到提交后；本函数会在页表写入前后
// 各做一次 layout 往返（ShaderReadOnly → TransferDst → ShaderReadOnly）。
void RecordTerrainLayerPageUploads(
    vk::CommandBuffer commandBuffer,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceTextures& sources,
    const TerrainLayerUploadBatch& batch,
    vk::Buffer stagingBuffer,
    vk::DeviceSize pageTableStagingOffset);

// 加载期同步上传入口：上传成功后把 streaming state 的 uploadedRevision
// 推进到本次内容版本；失败时保持 pending，便于下次整批重试。
void ApplyTerrainStreamingUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState);

struct RendererTerrainLoadResult
{
    std::vector<MeshObjectBuildPlan> objectPlans;
};

// 把已预检的 TerrainBuildPlan 展开成 chunk RenderableObject 与纯数据
// MeshObjectBuildPlan。地形没有独立 World 类型，仍复用 mesh draw/材质路径。
class RendererTerrainLoader
{
public:
    RendererTerrainLoader(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        RendererResourceLoadContext& loadContext,
        TerrainStreamingState& streamingState);

    RendererTerrainLoadResult LoadTerrainObject(
        const nlohmann::json& node,
        const TerrainBuildPlan& plan) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
    TerrainStreamingState& streamingState;
};

} // namespace VL

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
#include "terrain/terrainChunkResidency.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainLayerSources.h"
#include "terrain/terrainLayerVisibility.h"
#include "terrain/terrainPageManager.h"
#include "render/resource/terrain/terrainAtlasGpu.h"
#include "render/resource/terrain/terrainLayerPageUpload.h"
#include "render/resource/terrain/terrainPageUpload.h"
#include "render/resource/terrain/terrainSourceSets.h"
#include "render/resource/terrain/terrainUploadRings.h"

class PipelineFactory;
class Texture;
class MaterialInstance;
struct MeshObjectBuildPlan;

namespace VL
{
class RendererBackendVulkan;
class TerrainFeedbackGpuRing;
class TerrainLayerFeedbackGpuRing;
class TerrainPageBakeGpu;
struct RendererResourceLoadContext;

struct TerrainStreamingStats
{
    // 渲染线程第一次 PublishStats 之前为 false：此时全部计数都是构造默认值，
    // 控制台必须报告“尚未发布”而不是打印一整行 0。
    bool statsPublished = false;
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
    // GPU compute 烘焙页（与 CPU preload/atlas upload 分开统计）。
    uint64_t gpuBakedPagesTotal = 0;
    uint32_t lastGpuBakedPages = 0;
    uint32_t pendingGpuBakeJobs = 0;
    uint64_t lastFeedbackEpoch = 0;
    TerrainFeedbackApplyStats lastFeedbackApply;
    // L2 层页统计：atlas 已创建但 shader 尚未消费，用于确认布局/预算。
    // S1/S2：全局层数与 sourceSet 数；dedupedLayers 是跨 TR 复用的层数。
    uint32_t layerGlobalLayers = 0;
    uint32_t layerSourceSetCount = 0;
    uint32_t layerDedupedLayers = 0;
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
    // feedback 命中（已驻留页被本帧看到）与请求尝试/成功必须分开统计：
    // 旧字段名 requestedLast 实际是 accepted，容易把“请求未成功”读成 0 命中。
    uint32_t layerFeedbackHits = 0;
    uint32_t layerRequestedPages = 0;
    uint32_t layerAcceptedPages = 0;
    uint32_t layerLastUploadedPages = 0;
    uint32_t layerPendingRequests = 0;
    // L4b：精确层页 feedback 的 slot/页计数（0 表示本帧走了覆盖页回退路径）。
    uint32_t layerExactFeedbackSlots = 0;
    uint32_t layerExactFeedbackPages = 0;
    // 饥饿保护：连续没有有效精确 feedback 的帧数、最近一次有效 feedback 的
    // epoch、本帧可见性是否来自覆盖页回退，以及当前 idle 窗口补偿 epoch。
    uint32_t layerStarvedFrames = 0;
    uint64_t layerLastValidFeedbackEpoch = 0;
    bool layerUsedCoverFallback = false;
    uint64_t layerIdleGraceEpochs = 0;
    // L4e：强制目标 mip 与相机前向预取/回退混合的实时值。
    float layerForceMip = -1.0f;
    bool layerPrefetchMoving = false;
    uint32_t layerFallbackBlendPages = 0;
    uint32_t layerAvgColorPages = 0;
    uint32_t layerPrefetchPages = 0;
    uint32_t layerPrefetchRequests = 0;
    uint32_t layerPrefetchAccepted = 0;
    // L4c：目标 mip 缺页、shader 向更粗 mip 回退并混合的 texel/页数。
    // fallbackPixels 由控制台按当前渲染分辨率换算，避免每帧写统计。
    uint32_t layerFallbackSlots = 0;
    uint32_t layerFallbackTargetPages = 0;
    std::array<uint32_t, 3> layerFallbackLevelPages{};
    // 有效 feedback texel 的目标 mip 分布（回退级分布的基准）。
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1> layerTargetMipSlots{};
    // 全局 mip 预算：容量上限、去重页集总页数、页集数、被预算夹住的
    // 细化次数，以及可见需求/去重页集各自的 mip 分布。
    uint32_t layerPageBudget = 0;
    uint32_t layerBudgetPages = 0;
    uint32_t layerBudgetSets = 0;
    uint32_t layerBudgetClampedRefinements = 0;
    bool layerBudgetExceeded = false;
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1>
        layerBudgetEntriesByMip{};
    std::array<uint32_t, TerrainLayerMaxMipLevel + 1>
        layerBudgetSetsByMip{};
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
    // S2：多个 sourceSet 各自绑定一组 albedo/normal/pbr 数组，共享同一组
    // 层 atlas。同一 set 内必须指向同一组 shared_ptr；不同 set 允许不同。
    TerrainLayerSourceSets layerSources;
    TerrainLayerGpuResources layerResources;
    // 任一 layer-table MI 把 u_terrainLayerMode 设为 1 时置位；加载期据此
    // 建立反馈驱动的驻留策略（L4a）。mode 0 不会创建层页上传 ring。
    bool layerVTRequested = false;
    // L4e：mode 1 的 M_terrain 实例。console `terrainlayer forcemip` 只写
    // layerForceMip 原子，渲染线程帧首把值写进这些实例；weak_ptr 不会让
    // MI 的生命周期越过 World 资源包。
    std::vector<std::weak_ptr<MaterialInstance>> layerVTMaterials;
    // -1 = 自动 mip；0..4 = 强制目标 mip。Console 写、渲染线程读。
    std::atomic<int32_t> layerForceMip{-1};
    // 渲染线程最后写入 M_terrain 的值，避免每帧重复 SetParameter。
    float lastAppliedLayerForceMip = -1.0f;
    // 新登记的 MI 需要补写一次当前值（World 加载后 console 已设置过的情况）。
    size_t lastAppliedLayerMaterialCount = 0;
    // L4e：相机前向预取。渲染线程按相机位移设置；每帧名额独立于
    // feedback 请求预算，但仍走 RequestPageAtEpoch 的容量/安全窗。
    bool layerPrefetchMoving = false;
    uint32_t maxLayerPrefetchPagesPerFrame = 4;
    float layerPrefetchMinForwardCm = 5.0f;
    // L4a：覆盖页 feedback -> 可见层 -> mip -> 层页请求。pending 跨帧保留，
    // 失败项下一帧按 priority/deferred 重新排序后重试。
    TerrainLayerVisibilityResolver layerVisibilityResolver;
    std::vector<TerrainLayerPageRequest> pendingLayerPageRequests;
    std::shared_ptr<TerrainLayerUploadGpuRing> layerUploadRing;
    // 最近一帧的可见层推导统计，由渲染线程写、PublishStats 复制。
    TerrainLayerVisibilityStats latestLayerVisibility;
    // L4b：最近一帧的精确层页 feedback 统计；latestLayerFeedbackExact
    // 标记本帧使用的是精确路径还是覆盖页回退路径。
    TerrainLayerExactFeedbackStats latestLayerExactFeedback;
    bool latestLayerFeedbackExact = false;
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
    // L4b：4 张层页 feedback attachment 的回读 ring；只有 layerVTRequested
    // 时才记录/消费，mode 0 完全不参与。
    std::shared_ptr<TerrainLayerFeedbackGpuRing> layerFeedbackRing;
    // M_terrain 的 u_uvScale；层页 feedback 必须用同一个值才能复现层 UV。
    // 同一 World 的所有 layer-table 地形必须一致（加载期校验）。
    float layerUvScale = 64.0f;
    bool layerUvScaleInitialized = false;
    // 每帧上传 ring；BuildUploadBatch 的 CPU 快照写进当前 frameIndex 的槽位，
    // 在 scene pass 前随主帧提交。
    std::shared_ptr<TerrainUploadGpuRing> uploadRing;
    // 覆盖页 GPU 烘焙：输入 SSBO + 每帧输出 buffer + compute pipeline。
    // 为 null 时 pageManager 保持 CPU 烘焙路径。
    std::shared_ptr<TerrainPageBakeGpu> pageBake;
    // World 级共享层表定义；GPU layerResources.layerTable 由它生成。
    // S1 起这是 registry 的全局层列表（globalId == 下标），不再是首个 TR 的
    // 局部层列表。
    std::vector<TerrainLayerDesc> layerDefinitions;
    // 跨 TR 复用掉的局部层数（registry 统计；0 表示没有任何共享 key）。
    uint32_t layerDedupedLayerCount = 0;
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
    uint64_t gpuBakedPagesTotal = 0;
    uint32_t lastGpuBakedPages = 0;
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
    // 几何 chunk 驻留基线：加载期写入一次、之后只读。与页统计分开存放，
    // 避免每帧把 per-level 明细复制进 PublishStats。
    mutable std::mutex chunkStatsMutex;
    TerrainChunkResidencyStats chunkStats;

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

    // 加载期一次性发布（多次地形加载时在 loader 内累加后整体发布）。
    void PublishChunkStats(TerrainChunkResidencyStats stats)
    {
        std::lock_guard<std::mutex> lock(chunkStatsMutex);
        chunkStats = std::move(stats);
    }

    TerrainChunkResidencyStats SnapshotChunkStats() const
    {
        std::lock_guard<std::mutex> lock(chunkStatsMutex);
        return chunkStats;
    }
};

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
        TerrainStreamingState& streamingState,
        const TerrainChunkResidencyDesc& chunkResidency =
            TerrainChunkResidencyDesc());

    RendererTerrainLoadResult LoadTerrainObject(
        const nlohmann::json& node,
        const TerrainBuildPlan& plan) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
    TerrainStreamingState& streamingState;
    // 第一版只在加载期使用：窗口外的细 LOD 不创建，运行时不做异步重传。
    TerrainChunkResidencyDesc chunkResidency;
};

} // namespace VL

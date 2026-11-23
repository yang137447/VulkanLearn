#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

#include <nlohmann/json.hpp>
#include <vulkan/vulkan.hpp>

#include "scene/sceneAssetTypes.h"
#include "terrain/terrainFeedback.h"
#include "terrain/terrainChunkResidency.h"
#include "terrain/terrainLayerSources.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainWeightMapBinding.h"
#include "terrain/terrainRvtBudget.h"
#include "render/resource/terrain/terrainAtlasGpu.h"
#include "render/resource/terrain/terrainPageUpload.h"
#include "render/resource/terrain/terrainSourceSets.h"
#include "render/resource/terrain/terrainUploadRings.h"

class PipelineFactory;
class Texture;
class MaterialInstance;
struct MeshObjectBuildPlan;

namespace VL
{
class TerrainCompositedService;
class TerrainVertexUpdateQueue;
class RendererBackendVulkan;
class TerrainFeedbackGpuRing;
class TerrainCompositedFeedbackGpuRing;
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
    std::function<void(TerrainRvtResourcePhase)> setResourcePhase;
    std::shared_ptr<TerrainCompositedService> composited;
    std::shared_ptr<TerrainVertexUpdateQueue> terrainVertices;
    TerrainPageManager pageManager;
    TerrainPageGpuResources pageResources;
    // sourceSet 保存实际源数组及 global ID 到源 slice 的映射，
    // 供合成构建与实时 detail 消费；不再创建层源 VT atlas。
    TerrainLayerSourceSets layerSources;
    TerrainLayerGpuResources layerResources;
    // 多地形 World 守卫：u_terrainWeightMapMode=1 采样的是 MI 自带的单块
    // 地形权重数组。按 **layerWeightMaps 纹理**分组（复制 MI 但指向同一份
    // 权重数组仍会逐 tile 重复），记录使用它的 placement；超过一个直接失败。
    TerrainWeightMapBindingTracker weightMapBindings;
    // P2/P3 feedback 回读 ring：World-local，随 package 退休；P3 起其 summary
    // 经 ApplyFeedback 驱动命中/miss/RequestPageAtEpoch。
    std::shared_ptr<TerrainFeedbackGpuRing> feedbackRing;
    // 合成材质页四附件回读，与覆盖页共享 epoch/fence 生命周期。
    std::shared_ptr<TerrainCompositedFeedbackGpuRing> compositedFeedbackRing;
    // 每帧上传 ring；BuildUploadBatch 的 CPU 快照写进当前 frameIndex 的槽位，
    // 在 scene pass 前随主帧提交。
    std::shared_ptr<TerrainUploadGpuRing> uploadRing;
    // 覆盖页 GPU 烘焙：输入 SSBO + 每帧输出 buffer + compute pipeline。
    // 为 null 时 pageManager 保持 CPU 烘焙路径。
    std::shared_ptr<TerrainPageBakeGpu> pageBake;
    // World 级共享材质 catalog；GPU layerResources.layerTable 由它生成。
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

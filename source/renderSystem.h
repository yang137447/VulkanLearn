#pragma once
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>
#include <array>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "baseStructs.h"
#include "render/backend/rendererDescriptorContext.h"
#include "render/backend/gpuPassTimer.h"
#include "render/backend/rendererFrameResources.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/environment/environmentIblBaker.h"
#include "render/environment/environmentComputeReloadParticipants.h"
#include "shader/reload/computeShaderArtifact.h"
#include "render/camera/autoExposureCompute.h"
#include "render/environment/environmentGpuTimer.h"
#include "render/environment/environmentUpdateScheduler.h"
#include "render/environment/environmentUpdateState.h"
#include "render/environment/environmentUpdateDiagnostics.h"
#include "render/environment/proceduralSkyCubeGenerator.h"
#include "render/environment/environmentRuntime.h"
#include "render/frontend/renderScene.h"
#include "render/frame/renderWorldSnapshot.h"
#include "render/frame/renderFrameRecorder.h"
#include "render/frontend/rendererFrontend.h"
#include "renderableObject.h"
#include "render/resource/placementBindingResolver.h"
#include "render/offline/offlineCameraMotionPolicy.h"
#include "render/offline/offlineDisplayVulkan.h"
#include "render/offline/offlineRenderCoordinator.h"
#include "render/frontend/scenePicking.h"
#include "render/hair/hairRuntime.h"
#include "render/pass/passRuntime.h"
#include "render/picking/renderPicking.h"
#include "render/overlay/editorViewportOverlay.h"
#include "render/foliage/speedTreeWindSystem.h"
#include "world/worldWind.h"
#include "render/shadow/csmSettings.h"
#include "render/shadow/csmRuntime.h"
#include "render/postprocess/postProcessControls.h"
#include "render/terrain/terrainFeedbackRuntime.h"
#include "terrain/terrainFeedback.h"
#include "terrain/terrainLayerFeedback.h"
#include "render/shadow/shadowCasterCull.h"
#include "ui/uiOverlayRendererVulkan.h"
#include "ui/uiRenderSnapshotQueue.h"
#include "world/worldSnapshotBuilder.h"
#include "render/editorBridge/materialInstancePreviewAdapter.h"
#include "hair/hairTraceFormat.h"
#include "hair/tressfxLiveParams.h"
#include "hair/tressfxGroupBinding.h"

class Material;
class MaterialInstance;
struct Renderpass;
class PipelineFactory;

namespace VL
{
class World;
struct OfflineWorldAssets;
struct ScriptMaterialCommand;
struct TerrainStreamingState;
class RendererBackendVulkan;
class ShaderReloadCoordinator;
class MaterialInstancePreviewSession;
class OfflineMaterialInstancePreviewSession;
}

namespace VL
{

struct PreparedRuntimeBinding
{
    std::shared_ptr<const World> world;
    std::shared_ptr<const OfflineWorldAssets> offlineWorldAssets;
    WorldSnapshotBuilder snapshotBuilder;
    RenderScene renderScene;
    ResolvedRenderScene resolvedRenderScene;
    RendererFrameResources::PreparedLightCapacity
        lightCapacity;
    WindResponseProfileSet windResponseProfiles;
    CsmSettings csmSettings;
    uint64_t nextSnapshotFrameIndex = 0;
};

} // namespace VL

// Owns the renderer-facing frame path: it converts the active World into an
// immutable WorldSnapshot, resolves that snapshot into GPU resource handles,
// records render graph passes, and hands the completed frame back to the Vulkan
// backend. It does not mutate gameplay World data; RT mode only enters through
// the snapshot mailbox.
class RenderSystem
    : private VL::PassRuntimeServices
    , private VL::IRendererMaterialInstancePreviewOwner
{
public:
    static RenderSystem& GetInstance()
    {
        static RenderSystem instance;
        return instance;
    }
    ~RenderSystem();
    // Startup frame/environment resources must exist before the initial World
    // transaction prepares graph descriptors and candidate light capacity.
    void InitializeWorldTransactionResources();
    // The World transaction already commits the active scene, graph, and
    // object resources. Startup only has to attach the optional UI participant
    // after that commit succeeds.
    void FinalizeInitialRenderObjectInitialization();
    void ShutdownRenderObject();
    // GT-only snapshot production. In workerThreadCount=1 Render() consumes it
    // immediately; in workerThreadCount=2 RenderThread consumes it later.
    void PublishSnapshotFromActiveWorld();
    // Dynamic mesh producers (softbody/glass) never write host-visible buffers
    // on GT. They publish the latest content per mesh key; the render thread
    // applies it to the swapchain frame slot before recording passes.
    void QueueDynamicVertices(
        std::string meshKey,
        std::vector<struct Vertex> vertices);
    void QueueDynamicMesh(
        std::string meshKey,
        std::vector<struct Vertex> vertices,
        std::vector<uint32_t> indices);
    size_t GetPendingDynamicMeshUpdateCountForTest() const
    {
        return dynamicMeshUpdateQueue.PendingCount();
    }
    // GT 在动画/毛发更新边界写入，发布后随 WorldSnapshot 冻结；RT 不读
    // ScriptRuntime，也不再用自动曝光的墙钟驱动物理。
    // frameDelta/gameDelta/hairDelta 是各自已夹取的模拟 delta；windDelta 是
    // EngineLoop RuntimeClock 的原始真实 delta，专门给固定步长风时钟消费。
    void SetSimulationFrameDeltas(float frameDelta, float gameDelta, float hairDelta,
        float windDelta)
    {
        simulationDeltaSeconds = frameDelta;
        gameSimulationDeltaSeconds = gameDelta;
        hairSimulationDeltaSeconds = hairDelta;
        // EngineLoop 的 RuntimeClock 是真实时间；RenderSystem 在 RT 上不能直接读
        // GT 的 clock，因此把原始 delta 累加到这里，由风时钟在 RT 上消费。
        // 累加而不是覆盖：RT 可能比 GT 慢一帧，丢掉任一 GT delta 都会让风变慢。
        // C++17 的 atomic<float> 没有 fetch_add，用 CAS 累加。
        float expected = windPendingDeltaSeconds.load(std::memory_order_relaxed);
        while (!windPendingDeltaSeconds.compare_exchange_weak(
            expected, expected + windDelta, std::memory_order_relaxed))
        {
        }
    }
    // Consumes the latest snapshot if present, otherwise reuses the previous
    // RenderScene after one has been initialized.
    void RenderLatestSnapshotOrLastGood();
    void Render();
    void RequestScreenshot(std::string path);
    // 控制台只读诊断：打印最近一次已完成 feedback 回读的页统计。
    // 不触发 RequestPage/Evict，也不改变 preload=true 的驻留行为。
    void ReportTerrainFeedbackStatistics() const;
    // P3 控制台：打印驻留/预算/淘汰/上传统计（渲染线程发布的快照）。
    void ReportTerrainStatistics() const;
    void ReportTerrainLayerStatistics() const;
    // 运行时几何 LOD：读取最近一帧 WorldSnapshotBuilder 的组状态。
    void ReportTerrainLodStatistics() const;
    void ReportTerrainChunkStatistics() const;
    // 预算命令由 GT 写 atomic，渲染线程帧首消费；不直接跨线程改 page manager。
    void RequestTerrainResidentCapacity(uint32_t capacity);
    // L4a 层页预算与对照预取命令：同样只排队，渲染线程帧首消费。
    void RequestTerrainLayerResidentCapacity(uint32_t capacity);
    void RequestTerrainLayerPreload();
    // L4e：调试用强制目标 mip（-1 = 自动，0..4 = 覆盖）。只影响 mode 1；
    // 写入 streaming state 的 atomic，渲染线程帧首应用到 M_terrain/feedback。
    void RequestTerrainLayerForceMip(int forceMip);
    // 控制台 `terrain parity [tileX tileZ]`：渲染线程空闲时对同一页分别做
    // CPU 与 GPU 烘焙并逐 texel 比较。调试入口，会同步等待一次 GPU。
    void RequestTerrainPageBakeParity(int tileX, int tileZ);
    // 正常 Console `hair trace` 的 GT 侧入口：只记录值语义请求，由渲染线程在
    // RecordHairSimulation 中转发给 HairSimulationPass。GT 与渲染线程在
    // workerThreadCount=2 时并发，因此用互斥量保护这唯一一个待消费槽。
    void RequestHairTrace(std::string path, uint32_t frames, uint32_t guideStride);
    // 正常 Console `hair reset` / 毛发视口复位按钮的 GT 侧入口：只记录目标对象名
    // （空 = 所有 GPU guide 对象），由渲染线程在 RecordHairSimulation 中转发一次。
    void RequestHairSimulationReset(std::string objectName);
    // `hair set` 的 GT 侧入口：字段/值域已在执行端校验，这里只把请求交给渲染线程；
    // 由 HairSimulationPass 在 Record 中按对象名应用到该渲染对象的 config 副本。
    void RequestHairSimulationParameter(const VL::Hair::HairLiveParameterRequest& request);
    // 分组笔刷 ghost：GT 只登记值语义；渲染线程在下一帧按对象名整表更新 buffer。
    void RequestTressfxBrushPreview(
        const VL::Hair::TressfxBrushPreviewRequest& request);
    // Groups 草稿 binding：只在结构/预览 generation 变化时入队；RT 在
    // BeginFrame 后原地更新 group id/颜色/参数表，不重建 renderable。
    void RequestTressfxGroupBindingPreview(
        const VL::Hair::TressfxGroupBindingPreviewRequest& request);
    // --gpu-stats：开启后逐 pass 采集并按 120 帧打印一次；关闭时不创建 query pool。
    void SetGpuPassStatsLoggingEnabled(bool enabled)
    {
        gpuPassStatsLoggingEnabled = enabled;
        gpuPassTimer.SetLoggingEnabled(enabled);
    }
    // 控制台 gpustats：立即打印当前表，并在未开启时启动采集。
    void PrintGpuPassStats();
    // 控制台 perf json：打印与 Performance 页同源的单行 JSON，并启动采集。
    void PrintPerformanceJson();
    // GT 每帧复制一份冻结快照给 UI；读锁只覆盖一次值拷贝。
    VL::PerformanceSnapshot CapturePerformanceSnapshot() const
    {
        return gpuPassTimer.Snapshot();
    }
    // GT 把 CpuFrameProfiler 冻结的 CPU 阶段并入同一份快照；只在 GT 调用。
    void PublishCpuStageSamples(
        const std::vector<VL::PerformancePassSample>& stages,
        size_t windowFrameCount)
    {
        gpuPassTimer.PublishCpuStageSamples(stages, windowFrameCount);
    }
    // --perf-jsonl 需要时间序列，即使没有 --gpu-stats 也开启采集。
    void EnableGpuPassCollection() { gpuPassTimer.RequestCollection(); }
    void SetRendererBackend(VL::RendererBackendVulkan* backend) { rendererBackend = backend; }
    void SetUiRenderSnapshotQueue(VL::UiRenderSnapshotQueue* queue) { uiRenderSnapshotQueue = queue; }
    void SetSceneViewport(const VL::UiViewportRect& rect, bool enabled);
    vk::Extent2D GetSceneRenderExtent() const;
    void SetUiOverlayShaderPaths(std::string vertexPath, std::string fragmentPath);
    void SetCsmSettings(const VL::CsmSettings& settings);
    VL::CsmSettings GetCsmSettings() const;
    bool SetCsmCastShadows(bool enabled, std::string& outMessage);
    bool SetCsmDynamicShadowDistance(float distance, std::string& outMessage);
    bool SetCsmCascadeCount(uint32_t cascadeCount, std::string& outMessage);
    bool SetCsmCascadeDistributionExponent(float exponent, std::string& outMessage);
    bool SetCsmCascadeTransitionFraction(float fraction, std::string& outMessage);
    bool SetCsmShadowDistanceFadeoutFraction(float fraction, std::string& outMessage);
    bool SetCsmShadowBias(float value, std::string& outMessage);
    bool SetCsmShadowSlopeBias(float value, std::string& outMessage);
    bool SetCsmShadowCascadeBiasDistribution(
        float value,
        std::string& outMessage);
    void ReleaseSwapchainDependentResources();
    void RebuildSwapchainDependentResources();
    void RebuildRenderGraphDependentResources();
    void RefreshResolvedSceneAfterShaderReload();
    std::string GetResolvedShaderGenerationFingerprint() const;
    uint64_t GetActiveWorldGeneration() const noexcept;
    // 编辑器拾取/模型上下文只在开发者 UI 可见时需要；运行时 HUD 或
    // --no-dev-ui 下跳过整份 picking RenderScene 的逐帧重建。
    void SetPickingSceneEnabled(bool enabled) noexcept;
    std::optional<VL::MaterialInstanceSelection>
    PickMaterialInstanceAt(
        float mouseX,
        float mouseY,
        uint32_t viewportWidth,
        uint32_t viewportHeight) const;
    std::vector<VL::MaterialInstanceModelContext>
    GetMaterialInstanceModelContexts() const;
    void SetSelectedMaterialInstanceModel(
        const VL::MaterialInstanceModelContext& model);
    void SetSelectedMaterialInstance(
        const VL::MaterialInstanceSelection& selection);
    void ClearSelectedMaterialInstance() noexcept;
    VL::IMaterialInstancePreviewAdapter&
    GetMaterialInstancePreviewAdapter() noexcept
    {
        return materialInstancePreviewAdapter;
    }
    std::unique_ptr<VL::IMaterialInstancePreviewAdapter>
    CreateMaterialInstancePreviewAdapter();
    // 编辑器注入的视口覆盖层；runtime 只持有裸指针，editor 负责生命周期。
    void SetEditorViewportOverlay(VL::IEditorViewportOverlay* overlay) noexcept
    {
        editorViewportOverlay = overlay;
    }
    VL::PreparedRuntimeBinding PrepareRuntimeBinding(
        std::shared_ptr<const VL::World> world,
        VL::RendererResourceCache& candidateCache,
        RenderGraph& candidateGraph,
        std::shared_ptr<const VL::OfflineWorldAssets> offlineWorldAssets = {},
        bool prepareOffline = false);
    std::shared_ptr<const VL::OfflineWorldAssets>
    CaptureOfflineWorldAssets() const noexcept { return activeOfflineWorldAssets; }
    bool IsOfflineMode() const noexcept { return offlineMode.load(std::memory_order_acquire); }
    void SetInitialOfflineMode(bool enabled) noexcept
    {
        offlineMode.store(enabled, std::memory_order_release);
    }
    // --prerender-skin：启动时固定，必须在加载初始 World 之前设置，因为材质
    // shader variant 与对象 GPU 资源都按它选择。
    void SetPrerenderSkinEnabled(bool enabled) noexcept { prerenderSkinEnabled = enabled; }
    bool IsPrerenderSkinEnabled() const noexcept { return prerenderSkinEnabled; }
    bool SetOfflineMode(bool enabled, std::string& message);
    bool SetOfflineSetting(const std::string& name, int value, std::string& message);
    void SetOfflineAov(VL::OfflineAov aov) noexcept
    {
        offlineAov.store(aov, std::memory_order_release);
    }
    bool SetOfflineExposure(float exposure, std::string& message);
    void ControlOfflineSession(int command);
    const VL::OfflineRenderSettings& GetOfflineSettings() const noexcept { return offlineSettings; }
    VL::OfflineAov GetOfflineAov() const noexcept
    {
        return offlineAov.load(std::memory_order_acquire);
    }
    float GetOfflineExposure() const noexcept
    {
        return offlineExposure.load(std::memory_order_acquire);
    }
    // Structural renderer mutations still require RT to be idle. EngineLoop
    // installs a callback that implements WaitForRenderThreadIdle(); without a
    // render thread the call is a no-op.
    void SetRendererMutationSafePoint(std::function<void()> safePoint);
    void WaitForRendererMutationSafePoint();
    VL::Offline::SessionState GetOfflineState() const;
    std::string GetOfflineError() const;
    std::shared_ptr<const VL::OfflinePublishedResult> CaptureOfflineResult() const;
    bool ExportOffline(const VL::OfflinePublishedResult& result,
        const std::filesystem::path& directory, std::string& message) const;
    std::shared_ptr<void> CommitPreparedRuntimeBinding(
        VL::PreparedRuntimeBinding prepared) noexcept;
    void ClearPendingWorldSnapshots() noexcept;
    bool HasPendingWorldSnapshotForTest() const
    {
        return worldSnapshotQueue.HasPendingSnapshot();
    }
    VL::WorldSnapshotPtr PeekPendingWorldSnapshotForTest() const
    {
        const auto pending = worldSnapshotQueue.PeekLatest();
        return pending ? *pending : nullptr;
    }
    size_t GetLightCapacityForTest() const noexcept
    {
        return frameResources.GetLightCapacity();
    }
    const std::vector<VL::RHIBufferHandle>&
        GetLightBufferHandlesForTest() const noexcept
    {
        return frameResources.GetLightBufferHandlesForTest();
    }
    void PrepareRenderGraphReload(
        RenderGraph& candidateGraph);
    
    const std::vector<vk::DescriptorBufferInfo>& GetUBOGlobalBufferInfo() const
    {
        return frameResources.GetGlobalUniformBufferInfos();
    }
    const std::vector<vk::DescriptorBufferInfo>& GetLightBufferInfo() const
    {
        return frameResources.GetLightBufferInfos();
    }

    void SetDebugViewMode(int mode) { debugViewMode = mode; }
    int GetDebugViewMode() const { return debugViewMode; }
    // GT 只入队；MaterialInstance 由渲染线程拥有，命令在 Render() 内应用。
    void ApplyScriptMaterialCommands(
        const std::vector<VL::ScriptMaterialCommand>& commands);
    // 取回渲染线程发布的权威材质快照；无新快照返回 false。
    bool TakeScriptMaterialState(nlohmann::json& state);
    // 取回渲染线程产生的命令错误，由 GT 写入诊断。
    void TakeScriptMaterialErrors(std::vector<std::string>& errors);
    void SetEditorGridVisible(bool visible) noexcept
    {
        showEditorGrid.store(visible, std::memory_order_relaxed);
    }
    bool IsEditorGridVisible() const noexcept
    {
        return showEditorGrid.load(std::memory_order_relaxed);
    }
    // 编辑器辅助几何（参考网格等）是否允许录制。它只由脚本状态/开发者 UI
    // 驱动的帧级策略控制，与用户自己的 Grid 开关分开保存。
    void SetEditorOverlaysVisible(bool visible) noexcept
    {
        editorOverlaysVisible.store(visible, std::memory_order_relaxed);
    }
    bool AreEditorOverlaysVisible() const noexcept
    {
        return editorOverlaysVisible.load(std::memory_order_relaxed);
    }
    void SetEnvironmentIntensity(float intensity) { environmentIntensity = intensity; }
    float GetEnvironmentIntensity() const { return environmentIntensity; }
    VL::EnvironmentUpdateDiagnostics GetEnvironmentUpdateDiagnostics() const;
    ComputeShaderArtifact GetActiveComputeShaderArtifact(
        const std::string& shaderName) const;
    // 本帧解析后的场景风（已缓动、已含阵风）。风时间以固定 1/60 s 步长推进，
    // 步进量来自 EngineLoop 的 RuntimeClock 真实 delta（经 SetSimulationFrameDeltas
    // 传入 RT），与 master 的 SpeedTree 墙钟速度一致；活动 World 换代时清零并重新
    // 从作者目标起步。GetActiveWind 返回场级解析结果（供头发/调试），植被的每物种
    // 状态在 windResponseProfiles 内。
    const VL::ResolvedSceneWind& GetActiveWind() const { return currentRenderScene.resolvedWind; }
    double GetWindTimeSeconds() const { return currentRenderScene.windTimeSeconds; }
    // Renderer-owned debug/post-process controls. Runtime commands call these
    // APIs instead of reaching through RenderGraph to pass material instances.
    bool SetToneMappingMode(int mode, std::string& outMessage);
    int GetToneMappingMode() const { return postProcessControls.GetToneMappingMode(); }
    bool SetBloomStrength(float value, std::string& outMessage);
    float GetBloomStrength() const { return postProcessControls.GetBloomStrength(); }
    bool SetBloomThreshold(float value, std::string& outMessage);
    float GetBloomThreshold() const { return postProcessControls.GetBloomThreshold(); }
    bool SetBloomKnee(float value, std::string& outMessage);
    float GetBloomKnee() const { return postProcessControls.GetBloomKnee(); }
    bool SetBloomClamp(float value, std::string& outMessage);
    float GetBloomClamp() const { return postProcessControls.GetBloomClamp(); }
    // Rebinding the active World invalidates all CPU-side render views derived
    // from the previous World. The next Render() builds a fresh snapshot and
    // resolved scene from the new generation.
    void SetActiveWorld(std::shared_ptr<const VL::World> world);
    void SetPipelineFactory(PipelineFactory* factory) { pipelineFactory = factory; }
    void SetShaderReloadCoordinator(
        VL::ShaderReloadCoordinator* coordinator)
    {
        shaderReloadCoordinator = coordinator;
    }
private:
    friend class VL::MaterialInstancePreviewSession;
    friend class VL::OfflineMaterialInstancePreviewSession;

    RenderSystem()
        : materialInstancePreviewAdapter(*this)
    {
    }
    std::optional<VL::MaterialInstancePreviewWorldIdentity>
    GetActiveWorldIdentity() const override;
    VL::MaterialInstancePreviewAdapterResult
    CaptureMaterialInstancePreviewSession(
        const VL::MaterialInstancePreviewAdapterCommand& command,
        std::shared_ptr<
            VL::IRendererMaterialInstancePreviewSession>&
            outSession) override;
    VL::MaterialInstancePreviewAdapterResult
    CommitMaterialInstancePreviewDraft(
        const VL::MaterialInstancePreviewWorldIdentity& world,
        const VL::NormalizedMaterialInstancePath& materialInstancePath,
        const VL::MaterialInstancePreviewDraft& draft) override;
    std::shared_ptr<const VL::OfflineWorldAssets>
    PrepareOfflineMaterialPreview(
        const std::string& materialInstancePath,
        const nlohmann::json& materialInstanceJson) const;
    void ApplyMaterialInstancePreviewMaterialOverride(
        VL::RenderScene& renderScene) const;
    void RebuildRenderSceneMaterialGroups(
        VL::RenderScene& renderScene) const;
    void UpdateUBOGlobal(vk::CommandBuffer& commandBuffer);
    void UpdateUBOGlobalWithProjectionTransform(
        vk::CommandBuffer& commandBuffer,
        float projectionScaleX,
        float projectionScaleY,
        float projectionOffsetX,
        float projectionOffsetY);
    void UpdateUBOGlobalForShadow(
        vk::CommandBuffer& commandBuffer,
        uint32_t passSizeWidth,
        uint32_t passSizeHeight,
        uint32_t cascadeIndex);
    void RefreshRenderSceneFromActiveWorld();
    void PublishSnapshotFromActiveWorld(bool advanceSimulation);
    bool ConsumeLatestSnapshotIntoRenderScene();
    void BuildResolvedRenderScene();
    // 贴图参数替换后，每个 swapchain image 的描述符集要在它被 acquire 的那一帧重写。
    struct PendingMaterialTextureRefresh
    {
        std::string materialInstanceKey;
        // 每个 swapchain image 一位；该 image 被 acquire 时重建并清位，全部清空后移除。
        uint32_t remainingImagesMask = 0;
    };
    std::vector<PendingMaterialTextureRefresh> pendingMaterialTextureRefresh;
    void RefreshPendingMaterialTextureDescriptors();
    std::mutex scriptMaterialMutex;
    std::vector<VL::ScriptMaterialCommand> pendingScriptMaterialCommands;
    std::vector<std::string> pendingScriptMaterialErrors;
    nlohmann::json scriptMaterialState;
    bool scriptMaterialStateDirty = false;
    void ApplyPendingScriptMaterialCommands();
    void PublishScriptMaterialState();
    // 运行时克隆的蓝图对象在缺失 per-actor 实例时，从模板实例派生并绑进活动缓存。
    void EnsureBlueprintMaterialInstances(VL::RenderScene& renderScene);
    // RT frame-start snapshot of the GT-owned CSM settings. Invalidates the
    // RT shadow-cascade cache when the GT revision changed.
    void SnapshotFrameCsmSettings();
    // 放置级（per-placement）网格：resolve 之前确保每个实例键都有独立
    // RenderableObject（从模板克隆、共享导入几何），并按 placementBinding 的 Kind
    // 交给通用 resolver 应用绑定。当前唯一 provider 是 GPU 毛发，但流程不含
    // 类型分支；新增类型只需加 PlacementBinding::Kind 与 resolver 的一个 case。
    void EnsurePlacementRenderables(const VL::RenderScene& renderScene,
        VL::RendererResourceCache& resourceCache);
    // actor 销毁后（快照里连续两帧不再出现）把放置级 renderable 从活动 World 包
    // 摘除并交给 ResourceRetireQueue；与其它资源共用同一套 cache/epoch 回收。
    void PrunePlacementRenderables(const VL::RenderScene& renderScene);
    void InitializeCurrentRenderSceneResources();
    void RecordAndSubmitCurrentRenderScene();
    void AdvanceSceneWind();
    void AdvanceSceneWindTick();
    void UpdateExposureFrameTiming();
    void RenderInitialize();
    VL::RendererDescriptorContext BuildRendererDescriptorContext() const;
    VL::RendererDescriptorContext BuildRendererDescriptorContext(
        const VL::RendererResourceCache& resourceCache,
        RenderGraph& renderGraph,
        const std::vector<vk::DescriptorBufferInfo>&
            lightBufferInfos) const;
    std::shared_ptr<Texture> GetEnvironmentCube(
        const VL::RenderScene& renderScene,
        const VL::RendererResourceCache& resourceCache) const;
    void PrepareEnvironmentResources(
        const VL::RenderScene& renderScene,
        VL::RendererResourceCache& resourceCache) const;
    void UpdateGlobalUBOForPass(vk::CommandBuffer& commandBuffer) override;
    void UpdateShadowGlobalUBOForPass(
        vk::CommandBuffer& commandBuffer,
        uint32_t passWidth,
        uint32_t passHeight,
        uint32_t cascadeIndex) override;
    void UpdateMaterialInstanceUBOForPass(
        const std::shared_ptr<MaterialInstance>& materialInstance) override;
    void UpdateObjectUBOForPass(
        VL::RendererObjectGpuResources& objectResources,
        const VL::RenderDrawPacket& drawPacket) override;
    bool ShouldRecordEditorGridPass() const override;
    void RefreshMaterialDescriptorsForImage(uint32_t imageIndex) override;
    void RecordOutlineNormalSmoothing(
        vk::CommandBuffer& commandBuffer,
        uint32_t imageIndex) override;
    void UploadLightsForPass(
        uint32_t swapChainImageIndex,
        const std::vector<VL::LightSnapshot>& lights) override;
    bool IsCsmEnabled() const override;
    bool IsShadowCascadeActive(uint32_t cascadeIndex) const override;
    bool GetShadowCascadeLightViewProjection(
        uint32_t cascadeIndex,
        Eigen::Matrix4f& outLightViewProjection) const override;
    bool GetShadowCascadeCasterReceiverBounds(
        uint32_t cascadeIndex,
        VL::ShadowCasterReceiverBounds& outBounds) const override;
    void DrawEditorGridGeometry(vk::CommandBuffer& commandBuffer) override;

    void InitializeFrameResources();
    void ShutdownFrameResources();
    void ValidateFrameResourceDescriptors();
    // --gpu-stats 周期输出每级联 caster draw 的 before/after 计数。
    void LogShadowCasterCullStats() const;

    void BuildShadowCascadeFrameData(uint32_t passSizeWidth, uint32_t passSizeHeight);
    
    // 环境IBL相关
    std::shared_ptr<Texture> GetActiveEnvironmentCube();
    void PrepareEnvironmentResources();
    void RecordEnvironmentIbl(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex);
    // 在 renderPassOrdered 之前推进所有 GPU backend 的 TressFX guide 位置。
    void RecordHairSimulation(
        vk::CommandBuffer commandBuffer,
        uint32_t frameSlot);
    void ApplyTressfxBrushPreviewRequests(uint32_t swapchainImageIndex);
    void ApplyTressfxGroupBindingPreviewRequests();
    // 在 renderPassOrdered 之前：上传本帧 palette，再录制 GPU 预蒙皮 dispatch。
    // 关闭 --prerender-skin 时 renderGraph 没有该 pass，本函数直接返回。
    void RecordSkinDeform(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex);
    void RefreshEnvironmentUpdateDiagnostics();
    void ConsumeTerrainFeedback(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming,
        uint64_t completedEpoch,
        uint32_t frameIndex);
    // L4e：把 console 的强制 mip 写进已登记的 M_terrain 实例；只在值或
    // 实例集合变化时写，避免每帧改材质 UBO。
    void ApplyTerrainLayerForceMip(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming);
    // L4e：用相机本帧位移更新“前向移动”标志，驱动细一级 mip 预取。
    void UpdateTerrainCameraPrefetch(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming);
    void PrepareTerrainFeedbackPass(uint8_t frameTag);
    void PublishTerrainStreamingStats(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming,
        uint64_t completedEpoch);
    void RunTerrainPageBakeParityIfRequested();

    uint32_t currentFrame = 0;
    uint32_t swapChainImageIndex = 0;
    uint64_t nextSnapshotFrameIndex = 0;
    float simulationDeltaSeconds = 0.0f;
    float gameSimulationDeltaSeconds = 0.0f;
    float hairSimulationDeltaSeconds = 0.0f;
    int debugViewMode = 0;
    std::atomic_bool showEditorGrid{true};
    std::atomic_bool editorOverlaysVisible{true};
    float environmentIntensity = 1.0f;
    VL::PostProcessControls postProcessControls;
    bool hasRenderScene = false;
    // 风模拟时间。严格按固定 1/60 s 步长递增；不要把它当成帧时间。
    double currentWindTimeSeconds = 0.0;
    // true 表示已经执行过首帧 time=0 的采样；World 换代时清除，让新场景从目标值起步。
    bool windClockInitialized = false;
    // GT RuntimeClock 原始 delta 的跨线程累加器；RT 每帧 exchange(0) 取走。
    // 用 atomic 是因为 GT/RT 可能并行运行；CAS 累加见 SetSimulationFrameDeltas。
    std::atomic<float> windPendingDeltaSeconds{0.0f};
    // 不足一个 1/60 s 步长的余量；超过 0.25 s 的长卡顿只追 15 步，避免 gust 一次性补发。
    double windTickAccumulator = 0.0;
    VL::SceneWindRuntime sceneWindRuntime;
    WindResponseProfileSet windResponseProfiles;
    uint64_t initializedRenderWorldGeneration = 0;
    
    VL::CsmRuntime csmRuntime;
    VL::RendererFrameResources frameResources;
    VL::WorldSnapshotBuilder worldSnapshotBuilder;
    VL::WorldSnapshotQueue worldSnapshotQueue;
    VL::DynamicMeshUpdateQueue dynamicMeshUpdateQueue;
    VL::RendererFrontend rendererFrontend;
    VL::ResolvedRenderSceneBuilder resolvedRenderSceneBuilder;
    VL::PassRuntime passRuntime;
    // 编辑器参考网格几何：只由 editorGrid pass 消费；runtime 不拥有实现。
    VL::IEditorViewportOverlay* editorViewportOverlay = nullptr;
    VL::RenderScene currentRenderScene;
    VL::PlacementRenderableRuntime placementRuntime;
    VL::RenderFrameRecorder frameRecorder;
    uint64_t submittedCameraGeneration = 0;
    std::optional<Eigen::Matrix4f> submittedViewProjection;
    uint64_t submittedFrameSerial = 0;
    VL::ResolvedRenderScene currentResolvedRenderScene;
    VL::RenderPickingRuntime pickingRuntime;
    std::shared_ptr<const VL::World> activeWorld;
    std::shared_ptr<const VL::OfflineWorldAssets> activeOfflineWorldAssets;
    std::atomic<bool> offlineMode{false};
    bool prerenderSkinEnabled = false;
    VL::OfflineRenderSettings offlineSettings{0, 0, 32, 2, 1, 0};
    std::atomic<VL::OfflineAov> offlineAov{VL::OfflineAov::Combined};
    std::atomic<float> offlineExposure{1.0f};
    std::unique_ptr<VL::OfflineRenderCoordinator> offlineCoordinator;
    VL::OfflineDisplayVulkan offlineDisplay;
    VL::WorldSnapshotPtr currentOfflineSnapshot;
    VL::OfflineCameraMotionPolicy offlineCameraMotionPolicy;
    VL::RendererOwnedMaterialInstancePreviewAdapter
        materialInstancePreviewAdapter;
    VL::RendererBackendVulkan* rendererBackend = nullptr;
    PipelineFactory* pipelineFactory = nullptr;
    VL::AutoExposureReloadParticipant exposureHistogramReloadParticipant{0};
    VL::AutoExposureReloadParticipant exposureAdaptReloadParticipant{1};
    VL::ShaderReloadCoordinator* shaderReloadCoordinator = nullptr;
    VL::ProceduralSkyCubeGenerator proceduralSkyCubeGenerator;
    VL::EnvironmentIblBaker environmentIblBaker;
    VL::SkyShGenerateReloadParticipant skyShReloadParticipant{
        environmentIblBaker};
    VL::PrefilterEnvMapReloadParticipant prefilterReloadParticipant{
        environmentIblBaker};
    // 逐 pass GPU 计时：query pool 随帧资源创建/销毁，采集关闭时不分配。
    VL::GpuPassTimer gpuPassTimer;
    // 逐级联 caster 剔除计数只在渲染线程读写；日志与 --gpu-stats 同周期。
    std::array<VL::ShadowCasterCullStats, VL::CsmSettings::MaxCascadeCount>
        frameShadowCasterCullStats{};
    std::atomic<bool> gpuPassStatsLoggingEnabled{false};
    uint32_t shadowCasterCullStatsFramesSinceLog = 0;
    VL::EnvironmentRuntime environmentRuntime;
    VL::TerrainFeedbackRuntime terrainFeedbackRuntime;
    VL::HairRuntime hairRuntime;
    VL::UiOverlayRendererVulkan uiOverlayRenderer;
    VL::UiViewportRect requestedSceneViewport;
    bool sceneViewportRequested = false;
    mutable std::mutex sceneViewportMutex;
    VL::UiRenderSnapshotQueue* uiRenderSnapshotQueue = nullptr;
    std::shared_ptr<const VL::UiRenderSnapshot> currentUiRenderSnapshot;
    std::string uiVertexShaderPath;
    std::string uiFragmentShaderPath;
    // 截图请求只在 GT 命令执行端写入，在当前帧提交后由 RenderSystem 消费，
    // 确保回读不会与仍在录制的 command buffer 交叉。
    std::optional<std::string> pendingScreenshotPath;
    mutable std::mutex pendingScreenshotPathMutex;
    bool hasMaterialInstancePreviewMaterialOverride = false;
    std::string materialInstancePreviewOverridePath;
    std::string materialInstancePreviewOverrideMaterialKey;
    std::mutex rendererMutationSafePointMutex;
    std::function<void()> rendererMutationSafePoint;
};

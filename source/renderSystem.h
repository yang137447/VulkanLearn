#pragma once
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <unordered_map>
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
#include "render/frontend/renderScene.h"
#include "render/frontend/rendererFrontend.h"
#include "render/resource/placementBindingResolver.h"
#include "render/offline/offlineCameraMotionPolicy.h"
#include "render/offline/offlineDisplayVulkan.h"
#include "render/offline/offlineRenderCoordinator.h"
#include "editor/selection/materialInstanceSelection.h"
#include "render/pass/passRuntime.h"
#include "render/editor/editorViewportGrid.h"
#include "render/foliage/speedTreeWindSystem.h"
#include "render/shadow/csmSettings.h"
#include "render/shadow/shadowCasterCull.h"
#include "ui/uiOverlayRendererVulkan.h"
#include "ui/uiRenderSnapshotQueue.h"
#include "world/worldSnapshotBuilder.h"
#include "editor/preview/materialInstancePreviewAdapter.h"
#include "hair/hairTraceFormat.h"
#include "hair/tressfxLiveParams.h"

class Material;
class MaterialInstance;
struct Renderpass;
class PipelineFactory;

namespace VL
{
class World;
struct OfflineWorldAssets;
class RendererBackendVulkan;
class ShaderReloadCoordinator;
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
    SpeedTreeWindProfileSet speedTreeWindProfiles;
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
    , private VL::Editor::Preview::IRendererMaterialInstancePreviewOwner
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
    // GT 在动画/毛发更新边界写入，发布后随 WorldSnapshot 冻结；RT 不读
    // ScriptRuntime，也不再用自动曝光的墙钟驱动物理。
    void SetSimulationFrameDeltas(float frameDelta, float gameDelta, float hairDelta)
    {
        simulationDeltaSeconds = frameDelta;
        gameSimulationDeltaSeconds = gameDelta;
        hairSimulationDeltaSeconds = hairDelta;
    }
    // Consumes the latest snapshot if present, otherwise reuses the previous
    // RenderScene after one has been initialized.
    void RenderLatestSnapshotOrLastGood();
    void Render();
    void RequestScreenshot(std::string path);
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
    const VL::CsmSettings& GetCsmSettings() const noexcept { return csmSettings; }
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
    std::optional<VL::Editor::Selection::MaterialInstanceSelection>
    PickMaterialInstanceAt(
        float mouseX,
        float mouseY,
        uint32_t viewportWidth,
        uint32_t viewportHeight) const;
    std::vector<VL::Editor::Selection::MaterialInstanceModelContext>
    GetMaterialInstanceModelContexts() const;
    void SetSelectedMaterialInstanceModel(
        const VL::Editor::Selection::MaterialInstanceModelContext& model);
    void SetSelectedMaterialInstance(
        const VL::Editor::Selection::MaterialInstanceSelection& selection);
    void ClearSelectedMaterialInstance() noexcept;
    VL::Editor::Preview::IMaterialInstancePreviewAdapter&
    GetMaterialInstancePreviewAdapter() noexcept
    {
        return materialInstancePreviewAdapter;
    }
    std::unique_ptr<VL::Editor::Preview::IMaterialInstancePreviewAdapter>
    CreateMaterialInstancePreviewAdapter();
    VL::PreparedRuntimeBinding PrepareRuntimeBinding(
        std::shared_ptr<const VL::World> world,
        VL::RendererResourceCache& candidateCache,
        RenderGraph& candidateGraph,
        std::shared_ptr<const VL::OfflineWorldAssets> offlineWorldAssets = {},
        bool prepareOffline = false);
    std::shared_ptr<const VL::OfflineWorldAssets>
    CaptureOfflineWorldAssets() const noexcept { return activeOfflineWorldAssets; }
    bool IsOfflineMode() const noexcept { return offlineMode; }
    void SetInitialOfflineMode(bool enabled) noexcept { offlineMode = enabled; }
    // --prerender-skin：启动时固定，必须在加载初始 World 之前设置，因为材质
    // shader variant 与对象 GPU 资源都按它选择。
    void SetPrerenderSkinEnabled(bool enabled) noexcept { prerenderSkinEnabled = enabled; }
    bool IsPrerenderSkinEnabled() const noexcept { return prerenderSkinEnabled; }
    bool SetOfflineMode(bool enabled, std::string& message);
    bool SetOfflineSetting(const std::string& name, int value, std::string& message);
    void SetOfflineAov(VL::OfflineAov aov) noexcept { offlineAov = aov; }
    bool SetOfflineExposure(float exposure, std::string& message);
    void ControlOfflineSession(int command);
    const VL::OfflineRenderSettings& GetOfflineSettings() const noexcept { return offlineSettings; }
    VL::OfflineAov GetOfflineAov() const noexcept { return offlineAov; }
    float GetOfflineExposure() const noexcept { return offlineExposure; }
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
    void SetEnvironmentIntensity(float intensity) { environmentIntensity = intensity; }
    float GetEnvironmentIntensity() const { return environmentIntensity; }
    VL::EnvironmentUpdateDiagnostics GetEnvironmentUpdateDiagnostics() const;
    ComputeShaderArtifact GetActiveComputeShaderArtifact(
        const std::string& shaderName) const;
    bool SetSpeedTreeStrength(float strength);
    float GetSpeedTreeStrength() const { return speedTreeStrength; }
    bool SetSpeedTreeGustingEnabled(bool enabled);
    bool GetSpeedTreeGustingEnabled() const { return speedTreeGustingEnabled; }
    uint32_t GetSpeedTreeWindProfileCount() const
    {
        return static_cast<uint32_t>(speedTreeWindProfiles.GetProfileCount());
    }
    bool ForceSpeedTreeGust();
    // Renderer-owned debug/post-process controls. Runtime commands call these
    // APIs instead of reaching through RenderGraph to pass material instances.
    bool SetToneMappingMode(int mode, std::string& outMessage);
    int GetToneMappingMode() const { return toneMappingMode; }
    bool SetBloomStrength(float value, std::string& outMessage);
    float GetBloomStrength() const { return bloomStrength; }
    bool SetBloomThreshold(float value, std::string& outMessage);
    float GetBloomThreshold() const { return bloomThreshold; }
    bool SetBloomKnee(float value, std::string& outMessage);
    float GetBloomKnee() const { return bloomKnee; }
    bool SetBloomClamp(float value, std::string& outMessage);
    float GetBloomClamp() const { return bloomClamp; }
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
    class MaterialInstancePreviewSession;
    class OfflineMaterialInstancePreviewSession;

    RenderSystem()
        : materialInstancePreviewAdapter(*this)
    {
    }
    std::optional<VL::Editor::Preview::MaterialInstancePreviewWorldIdentity>
    GetActiveWorldIdentity() const override;
    VL::Editor::Preview::MaterialInstancePreviewAdapterResult
    CaptureMaterialInstancePreviewSession(
        const VL::Editor::Preview::MaterialInstancePreviewAdapterCommand& command,
        std::shared_ptr<
            VL::Editor::Preview::IRendererMaterialInstancePreviewSession>&
            outSession) override;
    VL::Editor::Preview::MaterialInstancePreviewAdapterResult
    CommitMaterialInstancePreviewDraft(
        const VL::Editor::Preview::MaterialInstancePreviewWorldIdentity& world,
        const VL::Editor::Preview::NormalizedMaterialInstancePath& materialInstancePath,
        const VL::Editor::Preview::MaterialInstancePreviewDraft& draft) override;
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
    void AdvanceSpeedTreeWindProfiles();
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
    double GetSpeedTreeWindTimeSeconds();
    // --gpu-stats 周期输出每级联 caster draw 的 before/after 计数。
    void LogShadowCasterCullStats() const;

        // 用于使用boundingbox加速
    std::pair<float, float> ComputeMinMaxAlongAxis(const Eigen::Vector3f& aabbMin, const Eigen::Vector3f& aabbMax, const Eigen::Vector3f& axis) const;
        // 遍历场景物体，在lightspace确定 minZ 和 maxZ（每帧只投影一次，见下）
    std::optional<std::pair<float, float>> ComputeCascadeLightSpaceZBounds(
        const Eigen::Matrix3f& worldToShadowMatrix,
        float minX,
        float maxX,
        float minY,
        float maxY);
    // caster 在光源空间的包围盒。投影只取决于光源朝向，同一帧的四个级联完全相同；
    // 因此每帧投影一次缓存，逐级联只做 XY 过滤与 Z 合并，避免 N 级 × 全部 draw 的重复计算。
    struct ShadowCasterLightSpaceBounds
    {
        float minX = 0.0f;
        float maxX = 0.0f;
        float minY = 0.0f;
        float maxY = 0.0f;
        float minZ = 0.0f;
        float maxZ = 0.0f;
    };
    std::vector<ShadowCasterLightSpaceBounds> casterLightSpaceBounds;
    bool casterLightSpaceBoundsValid = false;
        // 将局部空间的aabb变换到世界空间
    std::array<Eigen::Vector3f, 8> BuildWorldCorners(const Eigen::Vector3f& localMin, const Eigen::Vector3f& localMax, const Eigen::Matrix4f& modelMatrix) const;
        // 计算某个空间下的aabb
    void ComputeAabbFromCorners(const std::array<Eigen::Vector3f, 8>& corners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax);

    // 阴影计算策略
    enum class ShadowStrategy {
        DynamicTightBox,    // 原始方案：最小面积矩形 (利用率高，闪烁)
        StableBoundingSphere, // 基础稳定方案：外接球 (利用率低，极其稳定)
        StableRectangular // 改良方案：稳定长方形 (利用率中，稳定)
    };

    struct ShadowProjectionParams {
        Eigen::Matrix4f viewMatrix;
        Eigen::Matrix4f projectionMatrix;
        // 该级联接收面（相机 split）的光源空间 XY 范围，供 caster 预筛。
        VL::ShadowCasterReceiverBounds casterReceiverBounds;
    };

    struct ShadowCascadeFrameData {
        std::array<Eigen::Matrix4f, 4> view{};
        std::array<Eigen::Matrix4f, 4> projection{};
        std::array<Eigen::Matrix4f, 4> lightViewProj{};
        // 逐级联接收面 XY 范围；与 lightViewProj 同帧同源。
        std::array<VL::ShadowCasterReceiverBounds, 4> casterReceiverBounds{};
        Eigen::Vector4f cascadeSplits = Eigen::Vector4f::Zero();
        std::array<Eigen::Vector4f, 4> bias{};
        Eigen::Vector4f csmParameters = Eigen::Vector4f::Zero();
        uint64_t frameIndex = std::numeric_limits<uint64_t>::max();
        uint64_t worldGeneration = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        bool valid = false;
    };

    void BuildShadowCascadeFrameData(uint32_t passSizeWidth, uint32_t passSizeHeight);
    ShadowProjectionParams CalculateShadowMatrixForCameraRange(
        float splitNear,
        float splitFar,
        uint32_t passSizeWidth,
        uint32_t passSizeHeight);
    ShadowProjectionParams CalculateShadowMatrix_DynamicTight(const std::vector<Eigen::Vector3f>& pointsInShadowSys, const Eigen::Matrix3f& worldToShadowRotation, float sceneMaxZ, float sceneZRange);
    ShadowProjectionParams CalculateShadowMatrix_StableSphere(const std::vector<Eigen::Vector3f>& pointsInShadowSys, const Eigen::Matrix3f& worldToShadowRotation, float shadowMapResolution, float sceneMaxZ, float sceneZRange);
    ShadowProjectionParams CalculateShadowMatrix_StableRectangular(const std::vector<Eigen::Vector3f>& pointsInShadowSys, const Eigen::Matrix3f& worldToShadowRotation, float shadowMapResolution, float sceneMaxZ, float sceneZRange);
    
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
    // 在 renderPassOrdered 之前：上传本帧 palette，再录制 GPU 预蒙皮 dispatch。
    // 关闭 --prerender-skin 时 renderGraph 没有该 pass，本函数直接返回。
    void RecordSkinDeform(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex);
    void RefreshEnvironmentUpdateDiagnostics();
    uint32_t currentFrame = 0;
    uint32_t swapChainImageIndex = 0;
    uint64_t nextSnapshotFrameIndex = 0;
    float simulationDeltaSeconds = 0.0f;
    float gameSimulationDeltaSeconds = 0.0f;
    float hairSimulationDeltaSeconds = 0.0f;
    bool hasSelectedDraw = false;
    bool selectedAllMaterialSlots = false;
    VL::RuntimeId selectedObjectId = 0;
    uint32_t selectedMaterialSlotIndex = 0;
    uint64_t selectedWorldGeneration = 0;
    int debugViewMode = 0;
    float environmentIntensity = 1.0f;
    int toneMappingMode = 3;
    float bloomStrength = 0.08f;
    float bloomThreshold = 1.0f;
    float bloomKnee = 0.5f;
    float bloomClamp = 12.0f;
    float exposureDeltaSeconds = 1.0f / 60.0f;
    bool exposureClockInitialized = false;
    std::chrono::steady_clock::time_point exposureLastFrameTime;
    float speedTreeStrength = 0.35f;
    bool speedTreeGustingEnabled = true;
    bool hasRenderScene = false;
    bool windClockInitialized = false;
    std::chrono::steady_clock::time_point windStartTime;
    double currentWindTimeSeconds = 0.0;
    SpeedTreeWindProfileSet speedTreeWindProfiles;
    uint64_t initializedRenderWorldGeneration = 0;
    
    VL::CsmSettings csmSettings;
    ShadowCascadeFrameData shadowCascadeFrameData;
    VL::RendererFrameResources frameResources;
    VL::WorldSnapshotBuilder worldSnapshotBuilder;
    VL::WorldSnapshotQueue worldSnapshotQueue;
    VL::RendererFrontend rendererFrontend;
    VL::ResolvedRenderSceneBuilder resolvedRenderSceneBuilder;
    VL::PassRuntime passRuntime;
    // 编辑器参考网格几何：只由 editorGrid pass 消费，随帧资源一起创建/释放。
    VL::EditorViewportGrid editorViewportGrid;
    VL::RenderScene currentRenderScene;
    // 放置级 renderable 键 → 连续缺失帧数。只在 RT 访问；World 换代时清空
    // （整包由 transaction 退休）。
    std::unordered_map<std::string, int> placementAbsentFrames;
    // 放置级 renderable 键 → debugName；退休日志需要对象身份，不能只报键。
    std::unordered_map<std::string, std::string> placementObjectNames;
    // 上一帧放置级 draw 数；只在变化时打一条日志，兼作 spawn/destroy 的存活计数。
    size_t placementLiveCount = 0;
    // 活动包里 placement-scoped renderable 数；变化时打一条，作为无泄漏核对。
    size_t placementCacheCount = 0;
    // Ensure 创建、尚未退休的运行时放置 renderable 数；destroy 后应回到 0。
    size_t placementRuntimeCount = 0;
    // 放置绑定解析结果的跨实例缓存，键见 MakePlacementBindingCacheKey。
    VL::PlacementBindingCache placementBindingCache;
    uint64_t placementWorldGeneration = 0;
    uint64_t submittedCameraGeneration = 0;
    std::optional<Eigen::Matrix4f> submittedViewProjection;
    uint64_t submittedFrameSerial = 0;
    VL::ResolvedRenderScene currentResolvedRenderScene;
    // 拾取只读 GT 发布快照时生成的副本，避免 worker render thread 更新
    // currentRenderScene 时与鼠标事件并发读写同一对象。
    VL::RenderScene pickingRenderScene;
    bool hasPickingRenderScene = false;
    std::shared_ptr<const VL::World> activeWorld;
    std::shared_ptr<const VL::OfflineWorldAssets> activeOfflineWorldAssets;
    bool offlineMode = false;
    bool prerenderSkinEnabled = false;
    VL::OfflineRenderSettings offlineSettings{0, 0, 32, 2, 1, 0};
    VL::OfflineAov offlineAov = VL::OfflineAov::Combined;
    float offlineExposure = 1.0f;
    std::unique_ptr<VL::OfflineRenderCoordinator> offlineCoordinator;
    VL::OfflineDisplayVulkan offlineDisplay;
    VL::WorldSnapshotPtr currentOfflineSnapshot;
    VL::OfflineCameraMotionPolicy offlineCameraMotionPolicy;
    VL::Editor::Preview::RendererOwnedMaterialInstancePreviewAdapter
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
    VL::EnvironmentGpuTimer environmentGpuTimer;
    // 逐 pass GPU 计时：query pool 随帧资源创建/销毁，采集关闭时不分配。
    VL::GpuPassTimer gpuPassTimer;
    // 逐级联 caster 剔除计数只在渲染线程读写；日志与 --gpu-stats 同周期。
    std::array<VL::ShadowCasterCullStats, VL::CsmSettings::MaxCascadeCount>
        frameShadowCasterCullStats{};
    bool gpuPassStatsLoggingEnabled = false;
    uint32_t shadowCasterCullStatsFramesSinceLog = 0;
    VL::EnvironmentUpdateScheduler environmentUpdateScheduler;
    VL::EnvironmentUpdateState environmentUpdateState;
    std::shared_ptr<Texture> environmentUpdateSourceCube;
    mutable std::mutex environmentDiagnosticsMutex;
    VL::EnvironmentUpdateDiagnostics environmentDiagnostics;
    VL::UiOverlayRendererVulkan uiOverlayRenderer;
    VL::UiViewportRect requestedSceneViewport;
    bool sceneViewportRequested = false;
    VL::UiRenderSnapshotQueue* uiRenderSnapshotQueue = nullptr;
    std::shared_ptr<const VL::UiRenderSnapshot> currentUiRenderSnapshot;
    std::string uiVertexShaderPath;
    std::string uiFragmentShaderPath;
    // 截图请求只在 GT 命令执行端写入，在当前帧提交后由 RenderSystem 消费，
    // 确保回读不会与仍在录制的 command buffer 交叉。
    std::optional<std::string> pendingScreenshotPath;
    // hair trace 请求跨 GT/渲染线程；渲染线程每帧只取一次，取走后由
    // HairSimulationPass 在 Record 中消费。未请求时只有一次加锁读取。
    std::mutex hairTraceRequestMutex;
    std::optional<VL::Hair::HairTraceRequest> pendingHairTraceRequest;
    // 复位请求与 trace 同构：GT 只写这里，渲染线程每帧取走一次；未请求时
    // 只有一次加锁读取。空字符串表示所有 GPU guide 对象。
    std::mutex hairResetRequestMutex;
    std::optional<std::string> pendingHairResetRequest;
    // 实时参数覆盖：一帧可能有多条（滑杆拖动），用小队列而不是单槽；队列有界，
    // 过期意图先丢弃，避免无渲染帧时无限累积。
    std::mutex hairLiveParameterMutex;
    std::vector<VL::Hair::HairLiveParameterRequest> pendingHairLiveParameters;
    bool hasMaterialInstancePreviewMaterialOverride = false;
    std::string materialInstancePreviewOverridePath;
    std::string materialInstancePreviewOverrideMaterialKey;
};

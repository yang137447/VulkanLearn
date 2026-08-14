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
#include "baseStructs.h"
#include "engine/runtimeConfig.h"
#include "render/backend/rendererDescriptorContext.h"
#include "render/backend/rendererFrameResources.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/environment/environmentIblBaker.h"
#include "render/environment/environmentComputeReloadParticipants.h"
#include "shader/reload/computeShaderArtifact.h"
#include "render/environment/environmentGpuTimer.h"
#include "render/environment/environmentUpdateScheduler.h"
#include "render/environment/environmentUpdateState.h"
#include "render/environment/environmentUpdateDiagnostics.h"
#include "render/environment/proceduralSkyCubeGenerator.h"
#include "render/frontend/renderScene.h"
#include "render/frontend/rendererFrontend.h"
#include "render/pass/passRuntime.h"
#include "render/foliage/speedTreeWindSystem.h"
#include "ui/uiOverlayRendererVulkan.h"
#include "ui/uiRenderSnapshotQueue.h"
#include "world/worldSnapshotBuilder.h"

class Material;
class MaterialInstance;
struct Renderpass;
class PipelineFactory;

namespace VL
{
class World;
class RendererBackendVulkan;
class ShaderReloadCoordinator;
}

namespace VL
{

struct PreparedRuntimeBinding
{
    std::shared_ptr<const World> world;
    WorldSnapshotBuilder snapshotBuilder;
    RenderScene renderScene;
    ResolvedRenderScene resolvedRenderScene;
    RendererFrameResources::PreparedLightCapacity
        lightCapacity;
    SpeedTreeWindProfileSet speedTreeWindProfiles;
    uint64_t nextSnapshotFrameIndex = 0;
};

} // namespace VL

// Owns the renderer-facing frame path: it converts the active World into an
// immutable WorldSnapshot, resolves that snapshot into GPU resource handles,
// records render graph passes, and hands the completed frame back to the Vulkan
// backend. It does not mutate gameplay World data; RT mode only enters through
// the snapshot mailbox.
class RenderSystem : private VL::PassRuntimeServices
{
public:
    static RenderSystem& GetInstance()
    {
        static RenderSystem instance;
        return instance;
    }
    ~RenderSystem();
    void InitRenderObject();
    void ShutdownRenderObject();
    // GT-only snapshot production. In workerThreadCount=1 Render() consumes it
    // immediately; in workerThreadCount=2 RenderThread consumes it later.
    void PublishSnapshotFromActiveWorld();
    // Consumes the latest snapshot if present, otherwise reuses the previous
    // RenderScene after one has been initialized.
    void RenderLatestSnapshotOrLastGood();
    void Render();
    void SetRendererBackend(VL::RendererBackendVulkan* backend) { rendererBackend = backend; }
    void SetUiRenderSnapshotQueue(VL::UiRenderSnapshotQueue* queue) { uiRenderSnapshotQueue = queue; }
    void SetUiOverlayShaderPaths(std::string vertexPath, std::string fragmentPath);
    void SetCsmSettings(const VL::CsmSettings& settings);
    void ReleaseSwapchainDependentResources();
    void RebuildSwapchainDependentResources();
    void RebuildRenderGraphDependentResources();
    void RefreshResolvedSceneAfterShaderReload();
    std::string GetResolvedShaderGenerationFingerprint() const;
    uint64_t GetActiveWorldGeneration() const noexcept;
    VL::PreparedRuntimeBinding PrepareRuntimeBinding(
        std::shared_ptr<const VL::World> world,
        VL::RendererResourceCache& candidateCache,
        RenderGraph& candidateGraph);
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
    RenderSystem() = default;
    void UpdateUBOGlobal(vk::CommandBuffer& commandBuffer);
    void UpdateUBOGlobalForShadow(
        vk::CommandBuffer& commandBuffer,
        uint32_t passSizeWidth,
        uint32_t passSizeHeight,
        uint32_t cascadeIndex);
    void RefreshRenderSceneFromActiveWorld();
    bool ConsumeLatestSnapshotIntoRenderScene();
    void BuildResolvedRenderScene();
    void InitializeCurrentRenderSceneResources();
    void RecordAndSubmitCurrentRenderScene();
    void AdvanceSpeedTreeWindProfiles();
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

    void InitializeFrameResources();
    void ShutdownFrameResources();
    void ValidateFrameResourceDescriptors();
    double GetSpeedTreeWindTimeSeconds();

        // 用于使用boundingbox加速
    std::pair<float, float> ComputeMinMaxAlongAxis(const Eigen::Vector3f& aabbMin, const Eigen::Vector3f& aabbMax, const Eigen::Vector3f& axis) const;
        // 遍历场景物体，在lightspace确定 minZ 和 maxZ
    std::optional<std::pair<float, float>> ComputeCascadeLightSpaceZBounds(
        const Eigen::Matrix3f& worldToShadowMatrix,
        float minX,
        float maxX,
        float minY,
        float maxY) const;
        // 将局部空间的aabb变换到世界空间
    std::array<Eigen::Vector3f, 8> BuildWorldCorners(const Eigen::Vector3f& localMin, const Eigen::Vector3f& localMax, const Eigen::Matrix4f& modelMatrix) const;
        // 计算某个空间下的aabb
    void ComputeAabbFromCorners(const std::array<Eigen::Vector3f, 8>& corners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax);
        // 计算view空间下的aabb
    void ComputeViewAabbFromWorldCorners(const Eigen::Matrix4f& viewMatrix, const std::array<Eigen::Vector3f, 8>& worldCorners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax);
        // 用于筛选出实际落入当前级联范围内的 drawPacket
    bool IntersectsSplitFrustumFast(const Eigen::Vector3f& viewMin, const Eigen::Vector3f& viewMax, float splitNear, float splitFar, float fovRad, float aspect, float padding);

    // 阴影计算策略
    enum class ShadowStrategy {
        DynamicTightBox,    // 原始方案：最小面积矩形 (利用率高，闪烁)
        StableBoundingSphere, // 基础稳定方案：外接球 (利用率低，极其稳定)
        StableRectangular // 改良方案：稳定长方形 (利用率中，稳定)
    };

    struct ShadowProjectionParams {
        Eigen::Matrix4f viewMatrix;
        Eigen::Matrix4f projectionMatrix;
    };

    struct ShadowCascadeFrameData {
        std::array<Eigen::Matrix4f, 4> view{};
        std::array<Eigen::Matrix4f, 4> projection{};
        std::array<Eigen::Matrix4f, 4> lightViewProj{};
        Eigen::Vector4f cascadeSplits = Eigen::Vector4f::Zero();
        std::array<Eigen::Vector4f, 4> bias{};
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
    void RefreshEnvironmentUpdateDiagnostics();

    uint32_t currentFrame = 0;
    uint32_t swapChainImageIndex = 0;
    uint64_t nextSnapshotFrameIndex = 0;
    int debugViewMode = 0;
    float environmentIntensity = 1.0f;
    int toneMappingMode = 3;
    float bloomStrength = 0.08f;
    float bloomThreshold = 1.0f;
    float bloomKnee = 0.5f;
    float bloomClamp = 12.0f;
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
    VL::RenderScene currentRenderScene;
    VL::ResolvedRenderScene currentResolvedRenderScene;
    std::shared_ptr<const VL::World> activeWorld;
    VL::RendererBackendVulkan* rendererBackend = nullptr;
    PipelineFactory* pipelineFactory = nullptr;
    VL::ShaderReloadCoordinator* shaderReloadCoordinator = nullptr;
    VL::ProceduralSkyCubeGenerator proceduralSkyCubeGenerator;
    VL::EnvironmentIblBaker environmentIblBaker;
    VL::SkyShGenerateReloadParticipant skyShReloadParticipant{
        environmentIblBaker};
    VL::PrefilterEnvMapReloadParticipant prefilterReloadParticipant{
        environmentIblBaker};
    VL::EnvironmentGpuTimer environmentGpuTimer;
    VL::EnvironmentUpdateScheduler environmentUpdateScheduler;
    VL::EnvironmentUpdateState environmentUpdateState;
    std::shared_ptr<Texture> environmentUpdateSourceCube;
    mutable std::mutex environmentDiagnosticsMutex;
    VL::EnvironmentUpdateDiagnostics environmentDiagnostics;
    VL::UiOverlayRendererVulkan uiOverlayRenderer;
    VL::UiRenderSnapshotQueue* uiRenderSnapshotQueue = nullptr;
    std::shared_ptr<const VL::UiRenderSnapshot> currentUiRenderSnapshot;
    std::string uiVertexShaderPath;
    std::string uiFragmentShaderPath;
};

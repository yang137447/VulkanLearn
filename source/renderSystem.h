#pragma once
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include <string>
#include <utility>
#include "baseStructs.h"
#include "render/backend/rendererDescriptorContext.h"
#include "render/backend/rendererFrameResources.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/frontend/renderScene.h"
#include "render/frontend/rendererFrontend.h"
#include "render/pass/passRuntime.h"
#include "world/worldSnapshotBuilder.h"

class Material;
class MaterialInstance;
struct Renderpass;

namespace VL
{
class World;
class RendererBackendVulkan;
}

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
    void ReleaseSwapchainDependentResources();
    void RebuildSwapchainDependentResources();
    void RebuildRenderGraphDependentResources();
    
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
    // Renderer-owned debug/post-process controls. Runtime commands call these
    // APIs instead of reaching through RenderGraph to pass material instances.
    bool SetToneMappingMode(int mode, std::string& outMessage);
    bool SetBloomStrength(float value, std::string& outMessage);
    bool SetBloomThreshold(float value, std::string& outMessage);
    bool SetBloomKnee(float value, std::string& outMessage);
    bool SetBloomClamp(float value, std::string& outMessage);
    // Rebinding the active World invalidates all CPU-side render views derived
    // from the previous World. The next Render() builds a fresh snapshot and
    // resolved scene from the new generation.
    void SetActiveWorld(std::shared_ptr<const VL::World> world);
private:
    RenderSystem();
    void UpdateUBOGlobal(vk::CommandBuffer& commandBuffer);
    void UpdateUBOGlobalForShadow(vk::CommandBuffer& commandBuffer, uint32_t PassSizeWidth, uint32_t PassSizeHeight);
    void RefreshRenderSceneFromActiveWorld();
    bool ConsumeLatestSnapshotIntoRenderScene();
    void BuildResolvedRenderScene();
    void InitializeCurrentRenderSceneResources();
    void RecordAndSubmitCurrentRenderScene();
    void RenderInitialize();
    VL::RendererDescriptorContext BuildRendererDescriptorContext() const;
    void UpdateGlobalUBOForPass(vk::CommandBuffer& commandBuffer) override;
    void UpdateShadowGlobalUBOForPass(
        vk::CommandBuffer& commandBuffer,
        uint32_t passWidth,
        uint32_t passHeight) override;
    void UpdateMaterialInstanceUBOForPass(
        const std::shared_ptr<MaterialInstance>& materialInstance) override;
    void UpdateObjectUBOForPass(
        VL::RendererObjectGpuResources& objectResources,
        const VL::RenderDrawPacket& drawPacket) override;
    void UploadLightsForPass(
        uint32_t swapChainImageIndex,
        const std::vector<VL::LightSnapshot>& lights) override;

    void InitializeFrameResources();
    void ShutdownFrameResources();
    void ValidateFrameResourceDescriptors();

        // 用于使用boundingbox加速
    std::pair<float, float> ComputeMinMaxAlongAxis(const Eigen::Vector3f& aabbMin, const Eigen::Vector3f& aabbMax, const Eigen::Vector3f& axis) const;
    std::array<Eigen::Vector3f, 8> BuildWorldCorners(const Eigen::Vector3f& localMin, const Eigen::Vector3f& localMax, const Eigen::Matrix4f& modelMatrix);
    void ComputeAabbFromCorners(const std::array<Eigen::Vector3f, 8>& corners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax);
    void ComputeViewAabbFromWorldCorners(const Eigen::Matrix4f& viewMatrix, const std::array<Eigen::Vector3f, 8>& worldCorners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax);
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

    ShadowProjectionParams CalculateShadowMatrix_DynamicTight(const std::vector<Eigen::Vector3f>& pointsInShadowSys, const Eigen::Matrix3f& worldToShadowRotation, float sceneMaxZ, float sceneZRange);
    ShadowProjectionParams CalculateShadowMatrix_StableSphere(const std::vector<Eigen::Vector3f>& pointsInShadowSys, const Eigen::Matrix3f& worldToShadowRotation, float shadowMapResolution, float sceneMaxZ, float sceneZRange);
    ShadowProjectionParams CalculateShadowMatrix_StableRectangular(const std::vector<Eigen::Vector3f>& pointsInShadowSys, const Eigen::Matrix3f& worldToShadowRotation, float shadowMapResolution, float sceneMaxZ, float sceneZRange);

    uint32_t currentFrame = 0;
    uint32_t swapChainImageIndex = 0;
    uint64_t nextSnapshotFrameIndex = 0;
    int debugViewMode = 0;
    float environmentIntensity = 1.0f;
    bool hasRenderScene = false;
    uint64_t initializedRenderWorldGeneration = 0;
    
    Eigen::Matrix4f lightViewProj = Eigen::Matrix4f::Identity();
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
};

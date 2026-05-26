#pragma once
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>
#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include <string>
#include <utility>
#include "baseStructs.h"

class Material;
class SceneObject;
class MaterialInstance;
//用于按材质分类渲染
class RenderSystem
{
public:
    static RenderSystem& GetInstance()
    {
        static RenderSystem instance;
        return instance;
    }
    ~RenderSystem();
    void InitRenderObject();
    void Render();
    
    std::vector<vk::DescriptorBufferInfo>& GetUBOGlobalBufferInfo(){ return uboGlobal.bufferInfos; }

    void SetDebugViewMode(int mode) { debugViewMode = mode; }
    int GetDebugViewMode() const { return debugViewMode; }
    void SetEnvironmentIntensity(float intensity) { environmentIntensity = intensity; }
    float GetEnvironmentIntensity() const { return environmentIntensity; }
private:
    RenderSystem();
    void UpdateUBOGlobal(vk::CommandBuffer& commandBuffer);
    void UpdateUBOGlobalForShadow(vk::CommandBuffer& commandBuffer, uint32_t PassSizeWidth, uint32_t PassSizeHeight);
    void UpdateUBOMaterialInstance(const std::shared_ptr<MaterialInstance>& materialInstance);
    void UpdateUBOModel(const std::shared_ptr<SceneObject>& object);
    void CapturePreviousFrameTransforms();
    void RenderInitialize();

    void CreateUniformBuffers();
    void DestroyUniformBuffers();
    void SetupDescriptors();

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
    int debugViewMode = 0;
    float environmentIntensity = 1.0f;
    
    Eigen::Matrix4f lightViewProj = Eigen::Matrix4f::Identity();
    std::optional<Eigen::Matrix4f> previousViewProjection;
    Buffer uboGlobal;
    // 按基础材质对象分组： {materialKey, {materialInstance, [sceneObject]}}
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::weak_ptr<SceneObject>>>> hierarchyObjects;
};

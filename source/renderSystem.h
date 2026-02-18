#pragma once
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>
#include <array>
#include <memory>
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
private:
    RenderSystem();
    void UpdateUBOGlobal(vk::CommandBuffer& commandBuffer);
    void UpdateUBOGlobalForShadow(vk::CommandBuffer& commandBuffer, uint32_t PassSizeWidth, uint32_t PassSizeHeight);
    void UpdateUBOMaterialInstance(const std::shared_ptr<MaterialInstance>& materialInstance);
    void UpdateUBOModel(const std::shared_ptr<SceneObject>& object);
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

    uint32_t currentFrame = 0;
    uint32_t cpuSyncIndex = 0;
    uint32_t gpuSyncIndex = 0;
    std::vector<int32_t> onWorkFenceForSwapChainImage;
    uint32_t swapChainImageIndex = 0;
    
    Eigen::Matrix4f lightViewProj = Eigen::Matrix4f::Identity();
    Buffer uboGlobal;
    // 按基础材质分组： {shader, {materialInstance, [sceneObject]}}
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::weak_ptr<SceneObject>>>> hierarchyObjects;
};

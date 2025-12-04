#pragma once
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
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
    void UpdateUBOGlobal();            
    void UpdateUBOMaterialInstance(const std::shared_ptr<MaterialInstance>& materialInstance);
    void UpdateUBOModel(const std::shared_ptr<SceneObject>& object);
    void RenderInitialize();

    void CreateUniformBuffers();
    void DestroyUniformBuffers();
    void SetupDescriptors();

    uint32_t currentFrame = 0;
    uint32_t cpuSyncIndex = 0;
    uint32_t gpuSyncIndex = 0;
    std::vector<int32_t> onWorkFenceForSwapChainImage;
    uint32_t swapChainImageIndex = 0;
    
    Buffer uboGlobal;
    // 按基础材质分组： {shader, {materialInstance, [sceneObject]}}
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::weak_ptr<SceneObject>>>> hierarchyObjects;
};
#pragma once

#include <vector>
#include <vulkan/vulkan.hpp>
#include "baseStructs.h"

class DirectinalLight;
class PointLight;
class SpotLight;

class LightManager
{
public:
    static LightManager& GetInstance()
    {
        static LightManager instance;
        return instance;
    }
    
    ~LightManager();

    void RenderInitialize();

    void CreateLightBuffer();
    void DestroyLightBuffer();
    void SetupDescriptors();
    void UpdateLightBuffer(uint32_t swapChainImageIndex);
    std::vector<vk::DescriptorBufferInfo>& GetLightBufferInfo(){ return lightBuffer.bufferInfos; }
private:
    LightManager();

    Buffer lightBuffer;
};
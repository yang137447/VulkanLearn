#include "lightManager.h"

#include "commonFunction.h"
#include "vulkanManager.h"
#include "sceneLoader.h"
#include "sceneObject.h"
#include "baseStructs.h"

LightManager::LightManager()
{
}

LightManager::~LightManager()
{
    DestroyLightBuffer();
}

void LightManager::RenderInitialize()
{
    CreateLightBuffer();
    SetupDescriptors();
}

void LightManager::CreateLightBuffer()
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    const SceneLoader& sceneLoader = SceneLoader::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();
    uint32_t swapChainImageCount = vulkanManager.GetSwapChainImageCount();

    vk::DeviceSize lightSSBOSize = 
        sizeof(LightSSBOHeader) + sizeof(LightGPU) * (sceneLoader.GetDirectinalLight().size() +
                                                        sceneLoader.GetPointLight().size() +
                                                        sceneLoader.GetSpotLight().size());
    lightBuffer.buffers.resize(swapChainImageCount);
    lightBuffer.bufferMemories.resize(swapChainImageCount);
    lightBuffer.buffersMapped.resize(swapChainImageCount);
    vk::BufferUsageFlags Usage = vk::BufferUsageFlagBits::eStorageBuffer;
    vk::MemoryPropertyFlags MemoryProperty = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;

    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        std::tie(lightBuffer.buffers[i], lightBuffer.bufferMemories[i]) = CommonFunction::CreateBuffer(
            device,
            lightSSBOSize,
            Usage,
            VulkanManager::GetInstance().GetGpuMemoryProperties(),
            MemoryProperty
        );
        lightBuffer.buffersMapped[i] = device.mapMemory(lightBuffer.bufferMemories[i], 0, lightSSBOSize);
    }
}
void LightManager::DestroyLightBuffer()
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    uint32_t swapChainImageCount = vulkanManager.GetSwapChainImageCount();
    vk::Device& device = vulkanManager.GetDevice();
    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        device.freeMemory(lightBuffer.bufferMemories[i]);
        device.destroyBuffer(lightBuffer.buffers[i]);
    }
}

void LightManager::SetupDescriptors()
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    uint32_t swapChainImageCount = vulkanManager.GetSwapChainImageCount();
    vk::Device& device = vulkanManager.GetDevice();
    lightBuffer.bufferInfos.resize(swapChainImageCount);
    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        lightBuffer.bufferInfos[i] = vk::DescriptorBufferInfo()
            .setBuffer(lightBuffer.buffers[i])
            .setOffset(0)
            .setRange(VK_WHOLE_SIZE);       //整个SSBO
    }
}

void LightManager::UpdateLightBuffer(uint32_t swapChainImageIndex)
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    const SceneLoader& sceneLoader = SceneLoader::GetInstance();
    vk::Device& device = vulkanManager.GetDevice();
    uint32_t swapChainImageCount = vulkanManager.GetSwapChainImageCount();

    const std::unordered_map<std::string, std::shared_ptr<DirectinalLight>>& directionalLights = sceneLoader.GetDirectinalLight();
    const std::unordered_map<std::string, std::shared_ptr<PointLight>>& pointLights = sceneLoader.GetPointLight();
    const std::unordered_map<std::string, std::shared_ptr<SpotLight>>& spotLights = sceneLoader.GetSpotLight();

    uint32_t directionalLightCount = directionalLights.size();
    uint32_t pointLightCount = pointLights.size();
    uint32_t spotLightCount = spotLights.size();

    // 准备数据
    LightSSBOHeader lightSSBOHeader = {
        .directionalLightOffset = 0,
        .directionalLightCount = static_cast<int>(directionalLightCount),
        .pointLightOffset = static_cast<int>(directionalLightCount),
        .pointLightCount = static_cast<int>(pointLightCount),
        .spotLightOffset = static_cast<int>(directionalLightCount + pointLightCount),
        .spotLightCount = static_cast<int>(spotLightCount), 
    };
    std::vector<LightGPU> directionalLightGPUs(directionalLightCount);
    for (size_t i = 0; i < directionalLightCount; i++)
    {
        auto it = directionalLights.begin();
        std::advance(it, i);
        directionalLightGPUs[i].colorIntensity = Eigen::Vector4f(
            it->second->GetColor().x(), 
            it->second->GetColor().y(), 
            it->second->GetColor().z(), 
            it->second->GetIntensity());
        directionalLightGPUs[i].directionPad = Eigen::Vector4f(
            it->second->GetForwordVector().x(), 
            it->second->GetForwordVector().y(), 
            it->second->GetForwordVector().z(), 
            0.0f);
    }
    std::vector<LightGPU> pointLightGPUs(pointLightCount);
    for (size_t i = 0; i < pointLightCount; i++)
    {
        auto it = pointLights.begin();
        std::advance(it, i);
        pointLightGPUs[i].colorIntensity = Eigen::Vector4f(
            it->second->GetColor().x(), 
            it->second->GetColor().y(), 
            it->second->GetColor().z(), 
            it->second->GetIntensity());
        pointLightGPUs[i].positionRadius = Eigen::Vector4f(
            it->second->GetPosition().x(), 
            it->second->GetPosition().y(), 
            it->second->GetPosition().z(), 
            it->second->GetRadius());
    }
    std::vector<LightGPU> spotLightGPUs(spotLightCount);
    for (size_t i = 0; i < spotLightCount; i++)
    {
        auto it = spotLights.begin();
        std::advance(it, i);
        spotLightGPUs[i].colorIntensity = Eigen::Vector4f(
            it->second->GetColor().x(), 
            it->second->GetColor().y(), 
            it->second->GetColor().z(), 
            it->second->GetIntensity());
        spotLightGPUs[i].positionRadius = Eigen::Vector4f(
            it->second->GetPosition().x(), 
            it->second->GetPosition().y(), 
            it->second->GetPosition().z(), 
            it->second->GetRadius());
        spotLightGPUs[i].directionPad = Eigen::Vector4f(
            it->second->GetForwordVector().x(), 
            it->second->GetForwordVector().y(), 
            it->second->GetForwordVector().z(), 
            0.0f);
        spotLightGPUs[i].coneAngleOuterInnerPadPad = Eigen::Vector4f(
            it->second->GetConeAngleOuter(), 
            it->second->GetConeAngleInner(), 
            0.0f, 
            0.0f);
    }

    // 写入SSBO
    uint32_t offset = 0;
    const auto& value = lightSSBOHeader;
    uint8_t* data = static_cast<uint8_t*>(lightBuffer.buffersMapped[swapChainImageIndex]) + offset;
    std::memcpy(data, &value, sizeof(LightSSBOHeader));

    offset += sizeof(LightSSBOHeader);
    data = static_cast<uint8_t*>(lightBuffer.buffersMapped[swapChainImageIndex]) + offset;
    std::memcpy(data, directionalLightGPUs.data(), sizeof(LightGPU) * directionalLightCount);

    offset += sizeof(LightGPU) * directionalLightCount;
    data = static_cast<uint8_t*>(lightBuffer.buffersMapped[swapChainImageIndex]) + offset;
    std::memcpy(data, pointLightGPUs.data(), sizeof(LightGPU) * pointLightCount);

    offset += sizeof(LightGPU) * pointLightCount;
    data = static_cast<uint8_t*>(lightBuffer.buffersMapped[swapChainImageIndex]) + offset;
    std::memcpy(data, spotLightGPUs.data(), sizeof(LightGPU) * spotLightCount);
}

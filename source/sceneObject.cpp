#include "sceneObject.h"
#include "renderableObject.h"
#include <memory>
#include "commonFunction.h"
#include "vulkanManager.h"
#include "materialInstance.h"
#include "material.h"
#include "texture.h"
#include "renderPipline.h"
#include "shaderReflect.h"
#include "renderSystem.h"

void SceneNode::SetPosition(Eigen::Vector3f& pos)
{
    this->position = pos;
}

void SceneNode::SetRotation(Eigen::Vector3f& rot)
{
    this->rotation = rot;
}

void SceneNode::SetScale(Eigen::Vector3f& scale)
{
    this->scale = scale;
}

void SunLight::SetColor(Eigen::Vector3f& color)
{
    this->color = color;
}
void SunLight::SetIntensity(float intensity)
{
    this->intensity = intensity;
}

void Camera::SetHFOV(float fov)
{
    this->hFov = fov;
}

void Camera::SetClip(float near, float far)
{
    this->clipNear = near;
    this->clipFar = far;
}

SceneObject::SceneObject()
{
}

SceneObject::SceneObject(std::shared_ptr<RenderableObject> renderableObject, std::shared_ptr<MaterialInstance> materialInstance)
{
    SetRenderableObject(renderableObject);
    SetMaterialInstance(materialInstance);
}

SceneObject::~SceneObject()
{
    DestroyUniformBuffers();
    DestroyDescriptorSets();
}

void SceneObject::RenderInitialize()
{
    CreateUniformBuffers();
    SetupDescriptors();
    CreateDescriptorSets();
    UpdateDescriptorSet();
}

void SceneObject::SetRenderableObject(std::shared_ptr<RenderableObject> renderableObject)
{
    this->renderableObject = renderableObject;
}

void SceneObject::SetMaterialInstance(std::shared_ptr<MaterialInstance> materialInstance)
{
    this->materialInstance = materialInstance;
}

void SceneObject::CreateUniformBuffers()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    for(auto& ubo: {&uboModel})
    {
        vk::DeviceSize uniformBufferSize = sizeof(UBOModel);
        ubo->uniformBuffers.resize(swapChainImageCount);
        ubo->uniformBufferMemories.resize(swapChainImageCount);
        ubo->uniformBuffersMapped.resize(swapChainImageCount);
        ubo->uniformBufferSize = uniformBufferSize;
        vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer;
        vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        for(int i = 0; i < swapChainImageCount; i++)
        {
            std::tie(ubo->uniformBuffers[i], ubo->uniformBufferMemories[i]) = CommonFunction::CreateBuffer(
                device,
                uniformBufferSize, 
                usage, 
                VulkanManager::GetInstance().GetGpuMemoryProperties(), 
                memoryPropertyFlags
            );
            ubo->uniformBuffersMapped[i] = device.mapMemory(ubo->uniformBufferMemories[i], 0, uniformBufferSize);
        }
    }
}
void SceneObject::DestroyUniformBuffers()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    for(auto& ubo: {&uboModel})
    {
        for(int i = 0; i < swapChainImageCount; i++)
        {
            device.unmapMemory(ubo->uniformBufferMemories[i]);
            device.destroyBuffer(ubo->uniformBuffers[i]);
            device.freeMemory(ubo->uniformBufferMemories[i]);
        }
    }
}

void SceneObject::CreateDescriptorSets()
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    auto baseMaterial = materialInstance->GetBaseMaterial().lock();
    const std::vector<ShaderBinding>& shaderBindings = baseMaterial->GetRenderPipline()->GetShaderBindings();
    std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
    // descriptorPoolSizes[0]
    //     .setType(vk::DescriptorType::eUniformBuffer)
    //     .setDescriptorCount(MAX_FRAMES_IN_FLIGHT);
    // descriptorPoolSizes[1]
    //     .setType(vk::DescriptorType::eCombinedImageSampler)
    //     .setDescriptorCount(MAX_FRAMES_IN_FLIGHT);
    for(const auto& binding : shaderBindings)
    {
        vk::DescriptorPoolSize poolSize;
        poolSize
            .setType(binding.type)
            .setDescriptorCount(swapChainImageCount);
        descriptorPoolSizes.push_back(poolSize);
    }
    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(swapChainImageCount)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    vk::Result result = vulkanManager.GetDevice().createDescriptorPool(&descriptorPoolCreateInfo, nullptr, &descriptorPool);
    assert(result == vk::Result::eSuccess);

    std::vector<vk::DescriptorSetLayout> setLayouts(swapChainImageCount, baseMaterial->GetRenderPipline()->GetDescriptorSetLayout());
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayouts);
    
    descriptorSets.resize(swapChainImageCount);
    result = vulkanManager.GetDevice().allocateDescriptorSets(&descriptorSetAllocateInfo, descriptorSets.data());
    assert(result == vk::Result::eSuccess);
}

void SceneObject::DestroyDescriptorSets()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    device.freeDescriptorSets(descriptorPool, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data());
    device.destroyDescriptorPool(descriptorPool, nullptr);
}

void SceneObject::SetupDescriptors()
{
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    // 设置uniform缓冲区信息
    for(auto& ubo: {&uboModel})
    {
        ubo->uniformBufferInfos.resize(swapChainImageCount);
        for(int i = 0; i < swapChainImageCount; i++)
        {
            ubo->uniformBufferInfos[i]
                .setBuffer(ubo->uniformBuffers[i])
                .setOffset(0)
                .setRange(ubo->uniformBufferSize);
        }
    }
}

void SceneObject::UpdateDescriptorSet()
{
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    // 设置descriptor set信息
    writeDescriptorSets.resize(swapChainImageCount);
    auto baseMaterial = materialInstance->GetBaseMaterial().lock();
    auto& renderPipline = baseMaterial->GetRenderPipline();
    auto& renderSystem = RenderSystem::GetInstance();
    for(int i = 0; i < swapChainImageCount; i++)
    {
        writeDescriptorSets[i].push_back(
            vk::WriteDescriptorSet()
                .setDstSet(descriptorSets[i])
                .setDstBinding(0)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(renderSystem.GetUBOGlobalBufferInfo()[i]));
        writeDescriptorSets[i].push_back(
            vk::WriteDescriptorSet()
                .setDstSet(descriptorSets[i])
                .setDstBinding(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(materialInstance->GetUboMaterialInstanceInfo()[i]));
        writeDescriptorSets[i].push_back(
            vk::WriteDescriptorSet()
                .setDstSet(descriptorSets[i])
                .setDstBinding(2)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uboModel.uniformBufferInfos[i]));
        writeDescriptorSets[i].push_back(
            vk::WriteDescriptorSet()
                .setDstSet(descriptorSets[i])
                .setDstBinding(3)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setImageInfo(materialInstance->GetUboMaterialInstanceImageInfo()));
        
        VulkanManager::GetInstance().GetDevice().updateDescriptorSets(writeDescriptorSets[i], nullptr);
    }
}
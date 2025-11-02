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
    for(auto& ubo: {&uboModel})
    {
        vk::DeviceSize uniformBufferSize = sizeof(UBOModel);
        ubo->uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        ubo->uniformBufferMemories.resize(MAX_FRAMES_IN_FLIGHT);
        ubo->uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);
        ubo->uniformBufferSize = uniformBufferSize;
        vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer;
        vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
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
    for(auto& ubo: {&uboModel})
    {
        for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
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
    const std::vector<ShaderBinding>& shaderBindings = GetMaterialInstance()->GetBaseMaterial()->GetRenderPipline()->GetShaderBindings();
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
            .setDescriptorCount(MAX_FRAMES_IN_FLIGHT);
        descriptorPoolSizes.push_back(poolSize);
    }
    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(MAX_FRAMES_IN_FLIGHT)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    vk::Result result = vulkanManager.GetDevice().createDescriptorPool(&descriptorPoolCreateInfo, nullptr, &descriptorPool);
    assert(result == vk::Result::eSuccess);

    std::vector<vk::DescriptorSetLayout> setLayouts(MAX_FRAMES_IN_FLIGHT, GetMaterialInstance()->GetBaseMaterial()->GetRenderPipline()->GetDescriptorSetLayout());
    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(setLayouts);
    
    descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
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
    // 设置uniform缓冲区信息
    for(auto& ubo: {&uboModel})
    {
        ubo->uniformBufferInfos.resize(MAX_FRAMES_IN_FLIGHT);
        for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
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
    // 设置descriptor set信息
    writeDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);

    auto& renderPipline = GetMaterialInstance()->GetBaseMaterial()->GetRenderPipline();
    auto& renderSystem = RenderSystem::GetInstance();
    for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
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
#include "sceneObject.h"
#include <memory>
#include <iostream>

#include "renderableObject.h"
#include "commonFunction.h"
#include "texture.h"
#include "vulkanManager.h"
#include "materialInstance.h"
#include "material.h"
#include "pipeline/graphicsPipeline.h"
#include "shaderReflect.h"
#include "renderSystem.h"
#include "lightManager.h"
#include "sceneLoader.h"
#include "vulkanDebug.h"

void SceneNode::SetPosition(Eigen::Vector3f& position)
{
    this->position = position;
}

void SceneNode::SetRotation(Eigen::Vector3f& rotation)
{
    quaternion = CommonFunction::RotationToQuat(rotation);
    this->rotation = CommonFunction::QuatToRotation(quaternion);
}

void SceneNode::SetRotation(Eigen::Quaternionf& quaternion)
{
    this->quaternion = quaternion;
    this->rotation = CommonFunction::QuatToRotation(quaternion);
}

void SceneNode::SetRotation(Eigen::Quaternionf quaternion)
{
    this->quaternion = quaternion;
    this->rotation = CommonFunction::QuatToRotation(quaternion);
    //std::cout << "rotation: " << rotation.x() << " " << rotation.y() << " " << rotation.z() << std::endl;
}

void SceneNode::SetDeltaRotation(Eigen::Vector3f& deltaRotation)
{
    Eigen::Quaternionf deltaRotationQuaternion = CommonFunction::RotationToQuat(deltaRotation);
    quaternion = quaternion * deltaRotationQuaternion;
    quaternion = CommonFunction::RemoveRoll(quaternion);
    quaternion.normalize();
    this->rotation = CommonFunction::QuatToRotation(quaternion);
}

void SceneNode::SetScale(Eigen::Vector3f& scale)
{
    this->scale = scale;
}

Eigen::Vector3f SceneNode::GetForwardVector() const
{
    return quaternion * Eigen::Vector3f(0.0f, 0.0f, -1.0f);
}

Eigen::Vector3f SceneNode::GetRightVector() const
{
    return quaternion * Eigen::Vector3f(1.0f, 0.0f, 0.0f);
}

Eigen::Vector3f SceneNode::GetUpVector() const
{
    return quaternion * Eigen::Vector3f(0.0f, 1.0f, 0.0f);
}

void SceneNode::SetTransform(Eigen::Vector3f& position, Eigen::Vector3f& rotation, Eigen::Vector3f& scale)
{
    SetPosition(position);
    SetRotation(rotation);
    SetScale(scale);

    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationMatrix = CommonFunction::QuatToMatrix(quaternion);
    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    modelMatrix = translationMatrix * rotationMatrix * scaleMatrix;
    if(!previousModelMatrix.has_value())
    {
        // Scene load 时第一次 SetTransform 需要把 previous 初始化到当前矩阵，
        // 避免第一帧把“从单位矩阵移动到摆放位置”误写成 motion vector。
        // 后续每帧的 previous 由 RenderSystem::CapturePreviousFrameTransforms 维护。
        previousModelMatrix = modelMatrix;
    }
}

void SceneNode::UpdateModelMatrix()
{
    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationMatrix = CommonFunction::QuatToMatrix(quaternion);
    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    modelMatrix = translationMatrix * rotationMatrix * scaleMatrix;
}

Eigen::Matrix4f SceneNode::GetPreviousModelMatrix() const
{
    // UBOModel.previousModel 必须始终是一个确定矩阵。
    // optional 只表达 CPU 侧“还没有历史帧”的状态；首次上传时退回当前 model，避免假 motion vector。
    return previousModelMatrix.value_or(modelMatrix);
}

void SceneNode::SnapshotPreviousModelMatrix()
{
    previousModelMatrix = modelMatrix;
}

void DirectionalLight::SetColor(Eigen::Vector3f& color)
{
    this->color = color;
}
void DirectionalLight::SetIntensity(float intensity)
{
    this->intensity = intensity;
}

// PointLight 的 SetColor 和 SetIntensity 方法已内联在头文件中
Camera::Camera()
{
    isOrthographic = false;

    SetCamera(Eigen::Vector3f(0.0f, 2.0f, 2.0f), Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Vector3f(0.0f, 1.0f, 0.0f));

    SetProjection(
        90.0f, 
        static_cast<float>(CommonFunction::GetWindowSize().x()) / static_cast<float>(CommonFunction::GetWindowSize().y()), 
        0.1f, 10.0f);

    //Vulkan设备空间XYZ三个轴范围分别是 -1.0～+1.0、+1.0～-1.0、0.0～+1.0
    ndcMatrix << 
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -0.5f, 0.5f,
        0.0f, 0.0f, 0.0f, 1.0f;
}
void Camera::SetSize(float size)
{
    this->size = size;
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
void Camera::EnableOrthographic(bool enable)
{
    isOrthographic = enable;
}
void Camera::SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f lookAtPosition, Eigen::Vector3f up)
{
    const Eigen::Vector3f& f = -1.0 * (lookAtPosition - cameraPosition).normalized();
    const Eigen::Vector3f& r = up.cross(f).normalized();
    const Eigen::Vector3f& u = f.cross(r).normalized();
    const Eigen::Vector3f& p = cameraPosition;

    static Eigen::Matrix4f matrix01;
    matrix01 <<
        r.x(), r.y(), r.z(), 0.0f,
        u.x(), u.y(), u.z(), 0.0f,
        f.x(), f.y(), f.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;
    static Eigen::Matrix4f matrix02;
    matrix02 <<
        1.0f, 0.0f, 0.0f, -p.x(),
        0.0f, 1.0f, 0.0f, -p.y(),
        0.0f, 0.0f, 1.0f, -p.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    viewMatrix = matrix01 * matrix02;

    SetPosition(cameraPosition);
    SetRotation(Eigen::Quaternionf(matrix01.block<3,3>(0,0).transpose()));
}

void Camera::SetCamera(Eigen::Vector3f cameraPosition, Eigen::Vector3f cameraRotation)
{
    Eigen::Vector3f position = cameraPosition;

    //对于旋转处理，遵循 Yaw Pitch Roll 的顺序,即 先绕Y轴旋转，再绕X轴旋转，最后绕Z轴旋转
    Eigen::Matrix4f rotationMatrix = CommonFunction::RotationToMatrix(cameraRotation);

    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, -position.x(),
        0.0f, 1.0f, 0.0f, -position.y(),
        0.0f, 0.0f, 1.0f, -position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    viewMatrix =  rotationMatrix.transpose() * translationMatrix;

    SetPosition(cameraPosition);
    SetRotation(Eigen::Quaternionf(rotationMatrix.block<3,3>(0,0)));
}

void Camera::SetProjection(float fov, float aspect, float near, float far)
{
    float n = -1.0f * near;
    float f = -1.0f * far;
    float fovRad = fov * M_PI / 180.0f; 
    float k = -1.0f / std::tan(fovRad / 2.0f);
    projectionMatrix <<
        -k, 0.0f, 0.0f, 0.0f,
        0.0f, -aspect * k , 0.0f, 0.0f,
        0.0f, 0.0f, -(n + f)/(n-f), 2.0f * n * f / (n - f),
        0.0f, 0.0f, -1.0f, 0.0f;

    SetHFOV(fov);
    SetClip(near, far);
}

void Camera::SetOrthographic(float size, float aspect, float near, float far)
{
    float n = -1.0f * near;
    float f = -1.0f * far;
    projectionMatrix <<
        2.0f/size, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f*aspect/size , 0.0f, 0.0f,
        0.0f, 0.0f, 2.0f/(n-f), -1.0f*(n + f)/(n - f),
        0.0f, 0.0f, 0.0f, 1.0f;

    SetSize(size);
    SetClip(near, far);
}

Eigen::Matrix4f& Camera::GetViewMatrix()
{
    updateViewMatrix();
    return viewMatrix;
}
Eigen::Matrix4f& Camera::GetProjectionMatrix()
{
    static Eigen::Matrix4f matrix;
    matrix = ndcMatrix * projectionMatrix;
    return matrix;
}

void Camera::updateViewMatrix()
{
    //对于旋转处理，遵循 Yaw Pitch Roll 的顺序,即 先绕Y轴旋转，再绕X轴旋转，最后绕Z轴旋转
    Eigen::Matrix4f rotationMatrix = CommonFunction::QuatToMatrix(quaternion);

    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, -position.x(),
        0.0f, 1.0f, 0.0f, -position.y(),
        0.0f, 0.0f, 1.0f, -position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    viewMatrix =  rotationMatrix.transpose() * translationMatrix;
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
    DestroyDescriptorSetsForShadow();
}

void SceneObject::RenderInitialize()
{
    CreateUniformBuffers();
    SetupDescriptors();
    CreateDescriptorSets();
    UpdateDescriptorSet();

    SetupDescriptorsForShadow();
    CreateDescriptorSetsForShadow();
    UpdateDescriptorSetForShadow();
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
        vk::DeviceSize bufferSize = sizeof(UBOModel);
        ubo->buffers.resize(swapChainImageCount);
        ubo->bufferMemories.resize(swapChainImageCount);
        ubo->buffersMapped.resize(swapChainImageCount);
        ubo->bufferSize = bufferSize;
        vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer;
        vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        for(int i = 0; i < swapChainImageCount; i++)
        {
            std::tie(ubo->buffers[i], ubo->bufferMemories[i]) = CommonFunction::CreateBuffer(
                device,
                bufferSize, 
                usage, 
                VulkanManager::GetInstance().GetGpuMemoryProperties(), 
                memoryPropertyFlags,
                "UBO_Model: " + name + " (SwapchainIndex " + std::to_string(i) + ")"
            );
            ubo->buffersMapped[i] = device.mapMemory(ubo->bufferMemories[i], 0, bufferSize);
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
            device.unmapMemory(ubo->bufferMemories[i]);
            device.destroyBuffer(ubo->buffers[i]);
            device.freeMemory(ubo->bufferMemories[i]);
        }
    }
}

void SceneObject::CreateDescriptorSets()
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    uint32_t swapChainImageCount = vulkanManager.GetSwapChainImageCount();
    auto baseMaterial = materialInstance->GetBaseMaterial().lock();
    const std::vector<ShaderBinding>& shaderBindings = baseMaterial->GetRenderPipeline()->GetShaderBindings();
    std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
    // descriptorPoolSizes[0]
    //     .setType(vk::DescriptorType::eUniformBuffer)
    //     .setDescriptorCount(MAX_FRAMES_IN_FLIGHT);
    // descriptorPoolSizes[1]
    //     .setType(vk::DescriptorType::eCombinedImageSampler)
    //     .setDescriptorCount(MAX_FRAMES_IN_FLIGHT);
    for(const auto& binding : shaderBindings)
    {
        if(binding.set == PassSetIndex)
        {
            continue;
        }
        vk::DescriptorPoolSize poolSize;
        poolSize
            .setType(binding.type)
            .setDescriptorCount(swapChainImageCount);
        descriptorPoolSizes.push_back(poolSize);
    }
    const auto& pipelineSetLayouts = baseMaterial->GetRenderPipeline()->GetDescriptorSetLayouts();
    
    // 只分配前3个Set (0:Global, 1:Material, 2:Object)，Set 3 (Pass) 由 RenderPass 管理
    std::vector<vk::DescriptorSetLayout> allocateLayouts;
    for(size_t i = 0; i < pipelineSetLayouts.size(); ++i)
    {
        if(i != PassSetIndex)
        {
            allocateLayouts.push_back(pipelineSetLayouts[i]);
        }
    }
    uint32_t SetLayoutCount = allocateLayouts.size();

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(swapChainImageCount * SetLayoutCount)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    vk::Result result = vulkanManager.GetDevice().createDescriptorPool(&descriptorPoolCreateInfo, nullptr, &descriptorPool);
    assert(result == vk::Result::eSuccess);
    VulkanDebug::SetObjectName(vulkanManager.GetDevice(), descriptorPool, vk::ObjectType::eDescriptorPool, "DescriptorPool: " + name);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(allocateLayouts);
    
    descriptorSets.resize(swapChainImageCount);
    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        descriptorSets[i].resize(SetLayoutCount);
        result = vulkanManager.GetDevice().allocateDescriptorSets(&descriptorSetAllocateInfo, descriptorSets[i].data());
        assert(result == vk::Result::eSuccess);
        for(uint32_t j = 0; j < SetLayoutCount; j++)
        {
            VulkanDebug::SetObjectName(vulkanManager.GetDevice(), descriptorSets[i][j], vk::ObjectType::eDescriptorSet, "DescriptorSet: " + name + " (SwapchainIndex " + std::to_string(i) + ", Set " + std::to_string(j) + ")");
        }
    }
}

void SceneObject::DestroyDescriptorSets()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    for(auto& set: descriptorSets)
    {
        for(auto& descriptorSet: set)
        {
            device.freeDescriptorSets(descriptorPool, 1, &descriptorSet);
        }
    }
    device.destroyDescriptorPool(descriptorPool, nullptr);
}

void SceneObject::SetupDescriptors()
{
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    // 设置uniform缓冲区信息
    for(auto& ubo: {&uboModel})
    {
        ubo->bufferInfos.resize(swapChainImageCount);
        for(int i = 0; i < swapChainImageCount; i++)
        {
            ubo->bufferInfos[i]
                .setBuffer(ubo->buffers[i])
                .setOffset(0)
                .setRange(ubo->bufferSize);
        }
    }
}

void SceneObject::UpdateDescriptorSet()
{
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    // 设置descriptor set信息
    writeDescriptorSets.resize(swapChainImageCount);
    auto baseMaterial = materialInstance->GetBaseMaterial().lock();
    const auto& shaderBindings = baseMaterial->GetRenderPipeline()->GetShaderBindings();
    auto& renderSystem = RenderSystem::GetInstance();
    auto& lightManager = LightManager::GetInstance();
    auto& sceneLoader = SceneLoader::GetInstance();
    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        for(const auto& binding : shaderBindings)
        {
            if(binding.set == PassSetIndex)
            {
                continue;
            }

            vk::WriteDescriptorSet write;
            write
                .setDstSet(descriptorSets[i][binding.set])
                .setDstBinding(binding.binding)
                .setDescriptorCount(1)
                .setDescriptorType(binding.type);

            if (binding.type == vk::DescriptorType::eUniformBuffer)
            {
                if (binding.set == GlobalSetIndex)
                {
                    write.setBufferInfo(renderSystem.GetUBOGlobalBufferInfo()[i]);
                }
                else if (binding.set == MaterialSetIndex)
                {
                    write.setBufferInfo(materialInstance->GetUboMaterialInstanceInfo()[i]);
                }
                else if (binding.set == ObjectSetIndex)
                {
                    write.setBufferInfo(uboModel.bufferInfos[i]);
                }
            }
            else if (binding.type == vk::DescriptorType::eStorageBuffer)
            {
                write.setBufferInfo(lightManager.GetLightBufferInfo()[i]);
            }
            else if (binding.type == vk::DescriptorType::eCombinedImageSampler)
            {
                if (binding.set == GlobalSetIndex)
                {
                    const std::shared_ptr<Texture>* texture = sceneLoader.GetGlobalTextureByBindingName(binding.name);
                    if (texture == nullptr || *texture == nullptr)
                    {
                        continue;
                    }
                    write.setImageInfo((*texture)->GetDescriptorInfo());
                }
                else
                {
                    write.setImageInfo(materialInstance->GetTextureDescriptorInfo(binding.name));
                }
            }

            writeDescriptorSets[i].push_back(write);
        }
        
        VulkanManager::GetInstance().GetDevice().updateDescriptorSets(writeDescriptorSets[i], nullptr);
    }
}

void SceneObject::CreateDescriptorSetsForShadow()
{
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    uint32_t swapChainImageCount = vulkanManager.GetSwapChainImageCount();
    auto& sceneLoader = SceneLoader::GetInstance();

    // check if shadow material exists
    if (sceneLoader.GetMaterials().find("shadow") == sceneLoader.GetMaterials().end())
    {
        return;
    }
    auto shadowMaterial = sceneLoader.GetMaterials().at("shadow");
    const std::vector<ShaderBinding>& shaderBindings = shadowMaterial->GetRenderPipeline()->GetShaderBindings();
    
    std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
    for(const auto& binding : shaderBindings)
    {
        if(binding.set != ObjectSetIndex)
        {
            continue;
        }
        vk::DescriptorPoolSize poolSize;
        poolSize
            .setType(binding.type)
            .setDescriptorCount(swapChainImageCount);
        descriptorPoolSizes.push_back(poolSize);
    }

    if (descriptorPoolSizes.empty())
    {
        return;
    }

    const auto& pipelineSetLayouts = shadowMaterial->GetRenderPipeline()->GetDescriptorSetLayouts();
    
    // check if pipelineSetLayouts has enough sets
    if (pipelineSetLayouts.size() <= ObjectSetIndex)
    {
        return;
    }

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(swapChainImageCount)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    vk::Result result = vulkanManager.GetDevice().createDescriptorPool(&descriptorPoolCreateInfo, nullptr, &descriptorPoolShadow);
    assert(result == vk::Result::eSuccess);
    VulkanDebug::SetObjectName(vulkanManager.GetDevice(), descriptorPoolShadow, vk::ObjectType::eDescriptorPool, "DescriptorPool: Shadow: " + name);

    vk::DescriptorSetLayout objectSetLayout = pipelineSetLayouts[ObjectSetIndex];
    std::vector<vk::DescriptorSetLayout> allocateLayouts(swapChainImageCount, objectSetLayout);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPoolShadow)
        .setSetLayouts(allocateLayouts);
    
    std::vector<vk::DescriptorSet> allocatedSets(swapChainImageCount);
    result = vulkanManager.GetDevice().allocateDescriptorSets(&descriptorSetAllocateInfo, allocatedSets.data());
    assert(result == vk::Result::eSuccess);

    descriptorSetsShadow.resize(swapChainImageCount);
    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        descriptorSetsShadow[i].resize(ObjectSetIndex + 1);
        descriptorSetsShadow[i][ObjectSetIndex] = allocatedSets[i];
        VulkanDebug::SetObjectName(vulkanManager.GetDevice(), allocatedSets[i], vk::ObjectType::eDescriptorSet, "DescriptorSet: Shadow: " + name + " (SwapchainIndex " + std::to_string(i) + ")");
    }
}

void SceneObject::DestroyDescriptorSetsForShadow()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    if (descriptorPoolShadow)
    {
        device.destroyDescriptorPool(descriptorPoolShadow, nullptr);
        descriptorPoolShadow = nullptr;
    }
    descriptorSetsShadow.clear();
}

void SceneObject::SetupDescriptorsForShadow()
{
    // currently using same uboModel, already setup in SetupDescriptors
}

void SceneObject::UpdateDescriptorSetForShadow()
{
    if (descriptorSetsShadow.empty()) return;

    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    auto& sceneLoader = SceneLoader::GetInstance();
    
    if (sceneLoader.GetMaterials().find("shadow") == sceneLoader.GetMaterials().end())
    {
        return;
    }
    auto shadowMaterial = sceneLoader.GetMaterials().at("shadow");
    const auto& shaderBindings = shadowMaterial->GetRenderPipeline()->GetShaderBindings();

    writeDescriptorSetsShadow.resize(swapChainImageCount);

    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        writeDescriptorSetsShadow[i].clear();

        for(const auto& binding : shaderBindings)
        {
            if(binding.set != ObjectSetIndex)
            {
                continue;
            }

            vk::WriteDescriptorSet write;
            write
                .setDstSet(descriptorSetsShadow[i][binding.set])
                .setDstBinding(binding.binding)
                .setDescriptorCount(1)
                .setDescriptorType(binding.type);

            if (binding.type == vk::DescriptorType::eUniformBuffer)
            {
                if (binding.set == ObjectSetIndex)
                {
                    write.setBufferInfo(uboModel.bufferInfos[i]);
                }
            }
            
            writeDescriptorSetsShadow[i].push_back(write);
        }
        
        if (!writeDescriptorSetsShadow[i].empty())
        {
            VulkanManager::GetInstance().GetDevice().updateDescriptorSets(writeDescriptorSetsShadow[i], nullptr);
        }
    }
}

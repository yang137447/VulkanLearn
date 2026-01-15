#include "sceneObject.h"
#include <memory>
#include <iostream>

#include "renderableObject.h"
#include "commonFunction.h"
#include "vulkanManager.h"
#include "materialInstance.h"
#include "material.h"
#include "renderPipline.h"
#include "shaderReflect.h"
#include "renderSystem.h"
#include "lightManager.h"

void SceneNode::SetPosition(Eigen::Vector3f& position)
{
    this->position = position;
}

void SceneNode::SetRotation(Eigen::Vector3f& rotation)
{
    quaternion = CommonFunction::rotationToQuat(rotation);
    this->rotation = CommonFunction::quatToRotation(quaternion);
}

void SceneNode::SetRotation(Eigen::Quaternionf& quaternion)
{
    this->quaternion = quaternion;
    this->rotation = CommonFunction::quatToRotation(quaternion);
}

void SceneNode::SetRotation(Eigen::Quaternionf quaternion)
{
    this->quaternion = quaternion;
    this->rotation = CommonFunction::quatToRotation(quaternion);
    std::cout << "rotation: " << rotation.x() << " " << rotation.y() << " " << rotation.z() << std::endl;
}

void SceneNode::SetDeltaRotation(Eigen::Vector3f& deltaRotation)
{
    Eigen::Quaternionf deltaRotationQuaternion = CommonFunction::rotationToQuat(deltaRotation);
    quaternion = quaternion * deltaRotationQuaternion;
    quaternion = CommonFunction::RemoveRoll(quaternion);
    quaternion.normalize();
    this->rotation = CommonFunction::quatToRotation(quaternion);
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

    Eigen::Matrix4f rotationMatrix = CommonFunction::quatToMatrix(quaternion);
    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    modelMatrix = translationMatrix * rotationMatrix * scaleMatrix;
}

void SceneNode::UpdateModelMatrix()
{
    Eigen::Matrix4f scaleMatrix;
    scaleMatrix <<
        scale.x(), 0.0f, 0.0f, 0.0f,
        0.0f, scale.y(), 0.0f, 0.0f,
        0.0f, 0.0f, scale.z(), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f;

    Eigen::Matrix4f rotationMatrix = CommonFunction::quatToMatrix(quaternion);
    Eigen::Matrix4f translationMatrix;
    translationMatrix <<
        1.0f, 0.0f, 0.0f, position.x(),
        0.0f, 1.0f, 0.0f, position.y(),
        0.0f, 0.0f, 1.0f, position.z(),
        0.0f, 0.0f, 0.0f, 1.0f;

    modelMatrix = translationMatrix * rotationMatrix * scaleMatrix;
}

void DirectinalLight::SetColor(Eigen::Vector3f& color)
{
    this->color = color;
}
void DirectinalLight::SetIntensity(float intensity)
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
    Eigen::Matrix4f rotationMatrix = CommonFunction::rotationToMatrix(cameraRotation);

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
    Eigen::Matrix4f rotationMatrix = CommonFunction::quatToMatrix(quaternion);

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
                memoryPropertyFlags
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
    auto& shaderBindings = baseMaterial->GetRenderPipline()->GetShaderBindings();
    //TODO: 这里应有shaderbinding,来决定各个descriptor是否存在，并进行更新
    bool hasLightBuffer = false;
    bool hasTexture = false;
    for(const auto& binding : shaderBindings)
    {
        if(binding.binding == 1)
        {
            hasLightBuffer = true;
        }
        if(binding.binding == 4)
        {
            hasTexture = true;
        }
    }
    auto& renderSystem = RenderSystem::GetInstance();
    auto& lightManager = LightManager::GetInstance();
    for(int i = 0; i < swapChainImageCount; i++)
    {
        writeDescriptorSets[i].push_back(
            vk::WriteDescriptorSet()
                .setDstSet(descriptorSets[i])
                .setDstBinding(0)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(renderSystem.GetUBOGlobalBufferInfo()[i]));
        if(hasLightBuffer)
        {
            writeDescriptorSets[i].push_back(vk::WriteDescriptorSet()
                    .setDstSet(descriptorSets[i])
                    .setDstBinding(1)
                    .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                    .setBufferInfo(lightManager.GetLightBufferInfo()[i]));
        }
        writeDescriptorSets[i].push_back(
            vk::WriteDescriptorSet()
                .setDstSet(descriptorSets[i])
                .setDstBinding(2)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(materialInstance->GetUboMaterialInstanceInfo()[i]));
        writeDescriptorSets[i].push_back(
            vk::WriteDescriptorSet()
                .setDstSet(descriptorSets[i])
                .setDstBinding(3)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(uboModel.bufferInfos[i]));
        if(hasTexture)
        {
            writeDescriptorSets[i].push_back(
                vk::WriteDescriptorSet()
                    .setDstSet(descriptorSets[i])
                    .setDstBinding(4)
                    .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                    .setImageInfo(materialInstance->GetUboMaterialInstanceImageInfo()));
        }
        
        VulkanManager::GetInstance().GetDevice().updateDescriptorSets(writeDescriptorSets[i], nullptr);
    }
}

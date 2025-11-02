#include "renderSystem.h"
#include <iostream>
#include "sceneObject.h"
#include "materialInstance.h"
#include "material.h"
#include "vulkanManager.h"
#include "renderPipline.h"
#include "sceneLoader.h"
#include "matrix.h"
#include "settings.h"
#include "commonFunction.h"
#include "texture.h"
#include "renderableObject.h"
#include "sceneLoader.h"

RenderSystem::RenderSystem()
{
    CreateUniformBuffers();
    //SetupDescriptors();
}
RenderSystem::~RenderSystem()
{
    DestroyUniformBuffers();
}

void RenderSystem::InitRenderObject()
{
    auto& scene = SceneLoader::GetInstance();
    // 设置相机
    auto& matrix = Matrix::GetInstance();
    matrix.SetCamera(scene.GetCamera()->GetPosition(), Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0));
    matrix.SetProjection(scene.GetCamera()->GetHFOV(), (float)width/(float)height, scene.GetCamera()->GetClipNear(), scene.GetCamera()->GetClipFar());
    
    // 将场景物体按shader，materialInstance分组，填充进hierarchyObjects
    for(const auto& [objectName, sceneObject] : scene.GetSceneObjects())
    {
        auto& shaderName = sceneObject->GetMaterialInstance()->GetBaseMaterial()->GetShaderName();
        auto& materialInstanceName = sceneObject->GetMaterialInstance()->GetName();
        
        auto& shaderMap = hierarchyObjects.emplace(shaderName, std::unordered_map<std::string, std::vector<std::weak_ptr<SceneObject>>>()).first->second;
        auto& objects = shaderMap.emplace(materialInstanceName, std::vector<std::weak_ptr<SceneObject>>()).first->second;
        objects.push_back(sceneObject);
    }

    //初始化渲染需要的资源
    this->RenderInitialize();
    for (const auto& [shaderName, materialInstanceObjects] : hierarchyObjects)
    {
        for (const auto& [materialInstanceName, objects] : materialInstanceObjects)
        {
            const auto& materialInstance = scene.GetMaterialInstances().at(materialInstanceName);
            materialInstance->RenderInitialize();
            for(const auto& object : objects)
            {
                if(!object.expired())
                {
                    auto renderableObject = object.lock();
                    renderableObject->RenderInitialize();
                }
            }
        }
    }
}

void RenderSystem::Render()
{
    VulkanManager& instance = VulkanManager::GetInstance();
    vk::Device& device = instance.GetDevice();
    vk::CommandBuffer& commandBuffer = instance.GetCommandBuffers()[currentFrame];
    vk::Fence& taskFinishedFence = instance.GetTaskFinishedFences()[currentFrame];
    vk::SwapchainKHR& swapChain = instance.GetSwapChain();
    vk::Semaphore& imageAcquiredSemaphore = instance.GetImageAcquiredSemaphores()[currentFrame];
    vk::Semaphore& renderFinishedSemaphore = instance.GetRenderFinishedSemaphores()[currentFrame];
    vk::RenderPassBeginInfo& renderPassBeginInfo = instance.GetRenderPassBeginInfo();
    std::vector<vk::Framebuffer>& framebuffers = instance.GetFrameBuffers();
    vk::Queue& graphicQueue = instance.GetGraphicQueue();

    // 等待前一帧完成
    vk::Result result = device.waitForFences(taskFinishedFence, true, UINT64_MAX);
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to wait for fence");
    }
    device.resetFences(taskFinishedFence);

    // 获取下一帧
    result = device.acquireNextImageKHR(swapChain, UINT64_MAX, imageAcquiredSemaphore, nullptr, &swapchainImageIndex);
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to acquire next image");
    }

    // 重置并开始记录Command Buffer
    commandBuffer.reset();
    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    commandBuffer.begin(beginInfo);

    // 开始渲染通道
    renderPassBeginInfo.setFramebuffer(framebuffers[swapchainImageIndex]);
    commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);

    this->UpdateUBOGlobal();
    // 渲染每种材质
    for (const auto& [shaderName, shaderObjects] : hierarchyObjects) {
        // 绑定Pipeline
        const RenderPipline& renderPipline = *SceneLoader::GetInstance().GetMaterials().at(shaderName)->GetRenderPipline();
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, renderPipline.GetGraphicsPipeline());
        
        // 渲染每个材质实例
        for (auto& [materialInstanceName, objects] : shaderObjects) {
            const std::weak_ptr<MaterialInstance> materialInstance = SceneLoader::GetInstance().GetMaterialInstances().at(materialInstanceName);
            if(materialInstance.expired())
            {
                std::cout << "materialInstanceName: " << materialInstanceName << " is expired" << std::endl;
                continue;
            }
            else
            {
                this->UpdateUBOMaterialInstance(materialInstance.lock());
            }

            // 渲染使用该材质实例的所有对象
            for (auto& object : objects) {
                if(object.expired())
                {
                   std::cout << "object is expired" << std::endl;
                   continue;
                }
                auto objectPtr = object.lock();
                // 绑定DescriptorSet
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    renderPipline.GetPipelineLayout(),
                    0,
                    object.lock()->GetDescriptorSets()[currentFrame],
                    nullptr);
                this->UpdateUBOModel(objectPtr);
                objectPtr->GetRenderableObject()->Draw(commandBuffer);
            }
        }
    }

    // 结束渲染通道和Command Buffer记录
    commandBuffer.endRenderPass();
    commandBuffer.end();

    // 提交Command Buffer
    vk::PipelineStageFlags waitDstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::SubmitInfo submitInfo;
    submitInfo
        .setWaitSemaphores(imageAcquiredSemaphore)
        .setSignalSemaphores(renderFinishedSemaphore)
        .setWaitDstStageMask(waitDstStageMask)
        .setCommandBuffers(commandBuffer);
    
    graphicQueue.submit(submitInfo, taskFinishedFence);
    
    // 呈现
    vk::PresentInfoKHR presentInfo;
    presentInfo
        .setSwapchains(swapChain)
        .setImageIndices(swapchainImageIndex)
        .setWaitSemaphores(renderFinishedSemaphore);

    result = graphicQueue.presentKHR(presentInfo);
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to present image");
    }

    // 更新帧索引
    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void RenderSystem::UpdateUBOGlobal()
{
    Matrix& matrix = Matrix::GetInstance();

    static UBOGlobal ubo;
    ubo.view = matrix.GetViewMatrix();
    ubo.projection = matrix.GetProjectionMatrix();
    ubo.ambient = SceneLoader::GetInstance().GetAmbient();
    std::memcpy(uboGlobal.uniformBuffersMapped[currentFrame], &ubo, sizeof(ubo));
}

void RenderSystem::UpdateUBOMaterialInstance(const std::shared_ptr<MaterialInstance>& materialInstance)
{
    const auto& parameters = materialInstance->GetParameters();
    uint32_t offset = 0;
    for(auto& [name, parameter] : parameters)
    {
        if(parameter.type == ParamType::Float)
        {
            const auto& value = materialInstance->GetParameter<float>(name);
            uint8_t* uboData = static_cast<uint8_t*>(materialInstance->GetUboMaterialInstanceMapped()[currentFrame]) + offset;
            std::memcpy(uboData, &value, parameter.size);
            offset += parameter.size;
        }
        else if(parameter.type == ParamType::Vec2)
        {
            const auto& value = materialInstance->GetParameter<Eigen::Vector2f>(name);
            uint8_t* uboData = static_cast<uint8_t*>(materialInstance->GetUboMaterialInstanceMapped()[currentFrame]) + offset;
            std::memcpy(uboData, &value, parameter.size);
            offset += parameter.size;
        }
        else if(parameter.type == ParamType::Vec3)
        {
            const auto& value = materialInstance->GetParameter<Eigen::Vector3f>(name);
            uint8_t* uboData = static_cast<uint8_t*>(materialInstance->GetUboMaterialInstanceMapped()[currentFrame]) + offset;
            std::memcpy(uboData, &value, parameter.size);
        }
        else if(parameter.type == ParamType::Vec4)
        {
            const auto& value = materialInstance->GetParameter<Eigen::Vector4f>(name);
            uint8_t* uboData = static_cast<uint8_t*>(materialInstance->GetUboMaterialInstanceMapped()[currentFrame]) + offset;
            std::memcpy(uboData, &value, parameter.size);
        }
    }
}

void RenderSystem::UpdateUBOModel(const std::shared_ptr<SceneObject>& object)
{
    Matrix& matrix = Matrix::GetInstance();
    matrix.SetModelTransform(object->GetPosition(), object->GetRotation(), object->GetScale());

    static UBOModel ubo;
    ubo.model = matrix.GetModelMatrix();
    std::memcpy(object->GetUboModelMapped()[currentFrame], &ubo, sizeof(ubo));
}

void RenderSystem::RenderInitialize()
{
    CreateUniformBuffers();
    SetupDescriptors();
}

void RenderSystem::CreateUniformBuffers()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    for(auto& ubo: {&uboGlobal})
    {
        vk::DeviceSize uniformBufferSize = sizeof(UBOGlobal);
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
void RenderSystem::DestroyUniformBuffers()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    for(auto& ubo: {&uboGlobal})
    {
        for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            device.unmapMemory(ubo->uniformBufferMemories[i]);
            device.destroyBuffer(ubo->uniformBuffers[i]);
            device.freeMemory(ubo->uniformBufferMemories[i]);
        }
    }
}

void RenderSystem::SetupDescriptors()
{
    // 设置uniform缓冲区信息
    for(auto& ubo: {&uboGlobal})
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
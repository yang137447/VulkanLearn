#include "renderSystem.h"
#include <iostream>
#include "sceneObject.h"
#include "materialInstance.h"
#include "material.h"
#include "vulkanManager.h"
#include "renderPipline.h"
#include "sceneLoader.h"
#include "settings.h"
#include "commonFunction.h"
#include "texture.h"
#include "renderableObject.h"
#include "sceneLoader.h"
#include "lightManager.h"

RenderSystem::RenderSystem()
{
}
RenderSystem::~RenderSystem()
{
    DestroyUniformBuffers();
}

void RenderSystem::InitRenderObject()
{
    auto& scene = SceneLoader::GetInstance();
    auto& lightManager = LightManager::GetInstance();
    // 设置相机
    auto& camera = scene.GetCamera();
    camera->SetCamera(scene.GetCamera()->GetPosition(), Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0));
    camera->SetProjection(scene.GetCamera()->GetHFOV(), (float)width/(float)height, scene.GetCamera()->GetClipNear(), scene.GetCamera()->GetClipFar());
    
    // 将场景物体按shader，materialInstance分组，填充进hierarchyObjects
    for(const auto& [objectName, sceneObject] : scene.GetSceneObjects())
    {
        auto baseMaterial = sceneObject->GetMaterialInstance()->GetBaseMaterial().lock();
        auto& shaderName = baseMaterial->GetShaderName();
        auto& materialInstanceName = sceneObject->GetMaterialInstance()->GetName();
        
        auto& shaderMap = hierarchyObjects.emplace(shaderName, std::unordered_map<std::string, std::vector<std::weak_ptr<SceneObject>>>()).first->second;
        auto& objects = shaderMap.emplace(materialInstanceName, std::vector<std::weak_ptr<SceneObject>>()).first->second;
        objects.push_back(sceneObject);
    }

    //初始化渲染需要的资源
    this->RenderInitialize();
    lightManager.RenderInitialize();
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

    //onWorkFenceForSwapChainImage.resize(MAX_FRAMES_IN_FLIGHT, -1);
}

void RenderSystem::Render()
{
    static VulkanManager& instance = VulkanManager::GetInstance();
    static vk::Device& device = instance.GetDevice();
    static vk::SwapchainKHR& swapChain = instance.GetSwapChain();
    static uint32_t swapchainImageCount = instance.GetSwapChainImageCount();
    static vk::RenderPassBeginInfo& renderPassBeginInfo = instance.GetRenderPassBeginInfo();
    static std::vector<vk::Framebuffer>& framebuffers = instance.GetFrameBuffers();
    static vk::Queue& graphicQueue = instance.GetGraphicQueue();
    static LightManager& lightManager = LightManager::GetInstance();

    uint32_t cpuSyncIndex = currentFrame % MAX_FRAMES_IN_FLIGHT;
    uint32_t gpuSyncIndex = currentFrame % swapchainImageCount;
    vk::Fence& taskFinishedFence = instance.GetTaskFinishedFences()[cpuSyncIndex];
    vk::CommandBuffer& commandBuffer = instance.GetCommandBuffers()[gpuSyncIndex];
    vk::Semaphore& imageAcquiredSemaphore = instance.GetImageAcquiredSemaphores()[gpuSyncIndex];
    vk::Semaphore& renderFinishedSemaphore = instance.GetRenderFinishedSemaphores()[gpuSyncIndex];

    // 等待前一帧完成
    vk::Result result = device.waitForFences(taskFinishedFence, true, UINT64_MAX);
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to wait for fence");
    }
    device.resetFences(taskFinishedFence);
    
    // 获取下一帧
    result = device.acquireNextImageKHR(swapChain, UINT64_MAX, imageAcquiredSemaphore, nullptr, &swapChainImageIndex);
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to acquire next image");
    }
    if(swapChainImageIndex != gpuSyncIndex)
    {
        // throw std::runtime_error("swapChainImageIndex != gpuSyncIndex");
        //TODO: 使用 gpuSyncIndex 获取的commandBuffer、imageAcquiredSemaphore、renderFinishedSemaphore需要重新获取
    }

    // 重置并开始记录Command Buffer
    commandBuffer.reset();
    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    commandBuffer.begin(beginInfo);

    // 开始渲染通道
    renderPassBeginInfo.setFramebuffer(framebuffers[swapChainImageIndex]);
    commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);

    this->UpdateUBOGlobal();
    lightManager.UpdateLightBuffer(swapChainImageIndex);
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
                    object.lock()->GetDescriptorSets()[swapChainImageIndex],
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
        .setImageIndices(swapChainImageIndex)
        .setWaitSemaphores(renderFinishedSemaphore);

    result = graphicQueue.presentKHR(presentInfo);
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to present image");
    }

    // 更新帧索引
    currentFrame = currentFrame + 1;
}

void RenderSystem::UpdateUBOGlobal()
{

    static const auto& sceneLoader = SceneLoader::GetInstance();
    static Camera& camera = *sceneLoader.GetCamera();

    static UBOGlobal ubo;
    ubo.view = camera.GetViewMatrix();
    ubo.projection = camera.GetProjectionMatrix();
    ubo.ambient = sceneLoader.GetAmbient();
    ubo.cameraPosition = sceneLoader.GetCamera()->GetPosition();

    std::memcpy(uboGlobal.buffersMapped[swapChainImageIndex], &ubo, sizeof(ubo));
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
            uint8_t* uboData = static_cast<uint8_t*>(materialInstance->GetUboMaterialInstanceMapped()[swapChainImageIndex]) + offset;
            std::memcpy(uboData, &value, parameter.size);
            offset += parameter.size;
        }
        else if(parameter.type == ParamType::Vec2)
        {
            const auto& value = materialInstance->GetParameter<Eigen::Vector2f>(name);
            uint8_t* uboData = static_cast<uint8_t*>(materialInstance->GetUboMaterialInstanceMapped()[swapChainImageIndex]) + offset;
            std::memcpy(uboData, &value, parameter.size);
            offset += parameter.size;
        }
        else if(parameter.type == ParamType::Vec3)
        {
            const auto& value = materialInstance->GetParameter<Eigen::Vector3f>(name);
            uint8_t* uboData = static_cast<uint8_t*>(materialInstance->GetUboMaterialInstanceMapped()[swapChainImageIndex]) + offset;
            std::memcpy(uboData, &value, parameter.size);
        }
        else if(parameter.type == ParamType::Vec4)
        {
            const auto& value = materialInstance->GetParameter<Eigen::Vector4f>(name);
            uint8_t* uboData = static_cast<uint8_t*>(materialInstance->GetUboMaterialInstanceMapped()[swapChainImageIndex]) + offset;
            std::memcpy(uboData, &value, parameter.size);
        }
    }
}

void RenderSystem::UpdateUBOModel(const std::shared_ptr<SceneObject>& object)
{
    object->UpdateModelMatrix();

    static UBOModel ubo;
    ubo.model = object->GetModelMatrix();
    std::memcpy(object->GetUboModelMapped()[swapChainImageIndex], &ubo, sizeof(ubo));
}

void RenderSystem::RenderInitialize()
{
    CreateUniformBuffers();
    SetupDescriptors();
}

void RenderSystem::CreateUniformBuffers()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    for(auto& ubo: {&uboGlobal})
    {
        vk::DeviceSize bufferSize = sizeof(UBOGlobal);
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
void RenderSystem::DestroyUniformBuffers()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    for(auto& ubo: {&uboGlobal})
    {
        for(int i = 0; i < swapChainImageCount; i++)
        {
            device.unmapMemory(ubo->bufferMemories[i]);
            device.destroyBuffer(ubo->buffers[i]);
            device.freeMemory(ubo->bufferMemories[i]);
        }
    }
}

void RenderSystem::SetupDescriptors()
{
    uint32_t swapChainImageCount = VulkanManager::GetInstance().GetSwapChainImageCount();
    // 设置uniform缓冲区信息
    for(auto& ubo: {&uboGlobal})
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
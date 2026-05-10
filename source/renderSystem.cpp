#include "renderSystem.h"
#include <iostream>
#include <limits>
#include <algorithm>
#include <array>
#include "sceneObject.h"
#include "materialInstance.h"
#include "material.h"
#include "vulkanManager.h"
#include "pipeline/pipelineBase.h"
#include "sceneLoader.h"
#include "commonFunction.h"
#include "texture.h"
#include "renderableObject.h"
#include "sceneLoader.h"
#include "lightManager.h"
#include "renderGraph.h"
#include "shaderReflect.h"
#include "profiler.h"
#include "vulkanDebug.h"

RenderSystem::RenderSystem()
{
}
RenderSystem::~RenderSystem()
{
    DestroyUniformBuffers();
}

void RenderSystem::InitRenderObject()
{
    PROFILE_FUNCTION();
    auto& scene = SceneLoader::GetInstance();
    auto& lightManager = LightManager::GetInstance();
    auto& renderGraph = RenderGraph::GetInstance();
    // 设置相机
    auto& camera = scene.GetCamera();
    camera->SetCamera(scene.GetCamera()->GetPosition(), Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0));
    camera->SetProjection(
        scene.GetCamera()->GetHFOV(), 
        (float)CommonFunction::GetWindowSize().x()/(float)CommonFunction::GetWindowSize().y(), 
        scene.GetCamera()->GetClipNear(), 
        scene.GetCamera()->GetClipFar());
    // camera->EnableOrthographic(true);
    // camera->SetOrthographic(
    //     10.0f, 
    //     (float)CommonFunction::GetWindowSize().x()/(float)CommonFunction::GetWindowSize().y(), 
    //     scene.GetCamera()->GetClipNear(), 
    //     scene.GetCamera()->GetClipFar());
    
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
    renderGraph.RenderInitialize();
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
    PROFILE_SCOPE("RenderSystem::Render");
//     CPU 帧循环（frameIndex）
// ┌──────────────────────────────────────────────────────────────────────┐
// │ frameIndex = currentFrame % MAX_FRAMES_IN_FLIGHT                     │
// │ taskFinishedFence[frameIndex]                                        │
// │ imageAcquiredSemaphore[frameIndex]                                   │
// └──────────────────────────────────────────────────────────────────────┘
//                      │
//                      ▼
//         waitForFences(taskFinishedFence[frameIndex])
//                      │
//                      ▼
//  acquireNextImageKHR(..., imageAcquiredSemaphore[frameIndex], &imageIndex)
//                      │
//                      ▼
//      if imagesInFlightFences[imageIndex] != null
//             waitForFences(imagesInFlightFences[imageIndex])
//                      │
//                      ▼
//  imagesInFlightFences[imageIndex] = taskFinishedFence[frameIndex]
//                      │
//                      ▼
//       resetFences(taskFinishedFence[frameIndex])
//                      │
//                      ▼
//  commandBuffer[imageIndex]  +  renderFinishedSemaphore[imageIndex]
//                      │
//                      ▼
//  ┌───────────────────────── submit ─────────────────────────┐
//  │ wait:  imageAcquiredSemaphore[frameIndex]                │
//  │ cmd :  commandBuffer[imageIndex]                         │
//  │ signal: renderFinishedSemaphore[imageIndex]              │
//  │ fence: taskFinishedFence[frameIndex]                     │
//  └──────────────────────────────────────────────────────────┘
//                      │
//                      ▼
//           present(wait: renderFinishedSemaphore[imageIndex])
    VulkanManager& instance = VulkanManager::GetInstance();
    vk::Device& device = instance.GetDevice();
    vk::SwapchainKHR& swapChain = instance.GetSwapChain();
    uint32_t swapchainImageCount = instance.GetSwapChainImageCount();
    vk::Queue& graphicQueue = instance.GetGraphicQueue();
    LightManager& lightManager = LightManager::GetInstance();
    RenderGraph& renderGraph = RenderGraph::GetInstance();
    SceneLoader& sceneLoader = SceneLoader::GetInstance();

    uint32_t frameIndex = currentFrame % MAX_FRAMES_IN_FLIGHT;
    vk::Fence& taskFinishedFence = instance.GetTaskFinishedFences()[frameIndex];
    vk::Semaphore& imageAcquiredSemaphore = instance.GetImageAcquiredSemaphores()[frameIndex];
    auto& imagesInFlightFences = instance.GetImagesInFlightFences();

    // 等待前一帧完成
    vk::Result result;
    {
        PROFILE_SCOPE("Render:WaitFence");
        result = device.waitForFences(taskFinishedFence, true, UINT64_MAX);
    }
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to wait for fence");
    }
    // 获取下一帧
    {
        PROFILE_SCOPE("Render:AcquireImage");
        result = device.acquireNextImageKHR(swapChain, UINT64_MAX, imageAcquiredSemaphore, nullptr, &swapChainImageIndex);
    }
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to acquire next image");
    }
    
    if (imagesInFlightFences[swapChainImageIndex])
    {
        result = device.waitForFences(imagesInFlightFences[swapChainImageIndex], true, UINT64_MAX);
        if(result != vk::Result::eSuccess) {
            throw std::runtime_error("Failed to wait for image fence");
        }
    }
    imagesInFlightFences[swapChainImageIndex] = taskFinishedFence;

    device.resetFences(taskFinishedFence);

    vk::CommandBuffer& commandBuffer = instance.GetCommandBuffers()[swapChainImageIndex];
    vk::Semaphore& renderFinishedSemaphore = instance.GetRenderFinishedSemaphores()[swapChainImageIndex];

    // 重置并开始记录Command Buffer
    commandBuffer.reset();
    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    commandBuffer.begin(beginInfo);

    VulkanDebug::ScopedRegion frameRegion(commandBuffer, "Frame:" + std::to_string(frameIndex) + " Image:" + std::to_string(swapChainImageIndex), VulkanDebug::DebugCategory::eDefault);

    const auto& renderPassOrdered = renderGraph.GetRenderpassesOrdered();
    for (size_t passIndex = 0; passIndex < renderPassOrdered.size(); ++passIndex)
    {
        const auto& renderPassName = renderPassOrdered[passIndex];
        std::string renderPassScopeName = "RenderPass:" + renderPassName;
        PROFILE_SCOPE(renderPassScopeName.c_str());
        const auto& renderPass = renderGraph.GetRenderpasses().at(renderPassName);
        
        VulkanDebug::ScopedRegion passRegion(commandBuffer, renderPassName, VulkanDebug::DebugCategory::ePass);

        //TODO: 后续的shadow解决方案应该是生成专用的shadowshader以提高性能,当前先这样临时处理
        if (renderPassName == "shadow")
        {
            // 开始渲染通道
            this->UpdateUBOGlobalForShadow(commandBuffer, renderPass.width, renderPass.height);

            vk::RenderPassBeginInfo renderPassBeginInfo;
            renderPassBeginInfo.setRenderPass(renderPass.renderPass);
            renderPassBeginInfo.setFramebuffer(renderPass.framebuffers[swapChainImageIndex]);
            renderPassBeginInfo.setRenderArea(vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(renderPass.width, renderPass.height)));
            renderPassBeginInfo.setClearValues(renderPass.clearValues);
            commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);

            // pass级别更新, 对一般物体的统一处理
            const std::weak_ptr<MaterialInstance> materialInstance = renderPass.materialInstance;
            if(materialInstance.expired())
            {
                std::cout << "materialInstance is expired" << std::endl;
                continue;
            }
            else
            {
                this->UpdateUBOMaterialInstance(materialInstance.lock());
            }
            
            auto baseMaterial = materialInstance.lock()->GetBaseMaterial().lock();
            const PipelineBase& renderPipeline = *baseMaterial->GetRenderPipeline();
            commandBuffer.bindPipeline(renderPipeline.GetBindPoint(), renderPipeline.GetPipeline());

            // lightManager.UpdateLightBuffer(swapChainImageIndex);
            // 渲染每种材质
            for (const auto& [shaderName, shaderObjects] : hierarchyObjects) {
                // // 绑定Pipeline
                // const PipelineBase& renderPipeline = *sceneLoader.GetMaterials().at(shaderName)->GetRenderPipeline();
                // commandBuffer.bindPipeline(renderPipeline.GetBindPoint(), renderPipeline.GetPipeline());
                
                // 渲染每个材质实例
                for (auto& [materialInstanceName, objects] : shaderObjects) {
                    const std::weak_ptr<MaterialInstance> materialInstance = sceneLoader.GetMaterialInstances().at(materialInstanceName);
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
                        VulkanDebug::ScopedRegion region(commandBuffer, objectPtr->GetName(), VulkanDebug::DebugCategory::eObject);
                        // 绑定DescriptorSet
                        const auto& descriptorSets = objectPtr->GetDescriptorSetsForShadow(swapChainImageIndex);
                        commandBuffer.bindDescriptorSets(
                            vk::PipelineBindPoint::eGraphics,
                            renderPipeline.GetPipelineLayout(),
                            GlobalSetIndex,
                            renderPass.GetDescriptorSets()[swapChainImageIndex][GlobalSetIndex],
                            nullptr);
                        commandBuffer.bindDescriptorSets(
                            vk::PipelineBindPoint::eGraphics,
                            renderPipeline.GetPipelineLayout(),
                            ObjectSetIndex,
                            descriptorSets[ObjectSetIndex],
                            nullptr);
                        this->UpdateUBOModel(objectPtr);
                        objectPtr->GetRenderableObject()->Draw(commandBuffer, renderPass.width, renderPass.height);
                    }
                }
            }
        }
        else if (renderPassName == "geometry")
        {
            // 开始渲染通道
            this->UpdateUBOGlobal(commandBuffer);

            vk::RenderPassBeginInfo renderPassBeginInfo;
            renderPassBeginInfo.setRenderPass(renderPass.renderPass);
            renderPassBeginInfo.setFramebuffer(renderPass.framebuffers[swapChainImageIndex]);
            renderPassBeginInfo.setRenderArea(vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(renderPass.width, renderPass.height)));
            renderPassBeginInfo.setClearValues(renderPass.clearValues);
            commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);
            
            lightManager.UpdateLightBuffer(swapChainImageIndex);
            // 渲染每种材质
            for (const auto& [shaderName, shaderObjects] : hierarchyObjects) {
                // 绑定Pipeline
                const PipelineBase& renderPipeline = *sceneLoader.GetMaterials().at(shaderName)->GetRenderPipeline();
                
                VulkanDebug::ScopedRegion pipelineRegion(commandBuffer, "Pipeline: " + shaderName, VulkanDebug::DebugCategory::ePipeline);
                commandBuffer.bindPipeline(renderPipeline.GetBindPoint(), renderPipeline.GetPipeline());
                if (!renderPass.GetDescriptorSets()[swapChainImageIndex].empty())
                {
                    bool bHasPassSet = false;
                    for(const auto& binding : renderPipeline.GetShaderBindings())
                    {
                        if(binding.set == PassSetIndex)
                        {
                            bHasPassSet = true;
                            break;
                        }
                    }

                    if(bHasPassSet)
                    {
                        commandBuffer.bindDescriptorSets(
                            vk::PipelineBindPoint::eGraphics,
                            renderPipeline.GetPipelineLayout(),
                            PassSetIndex,
                            renderPass.GetDescriptorSets()[swapChainImageIndex][PassSetIndex],
                            nullptr);
                    }
                }
                
                // 渲染每个材质实例
                for (auto& [materialInstanceName, objects] : shaderObjects) {
                    const std::weak_ptr<MaterialInstance> materialInstance = sceneLoader.GetMaterialInstances().at(materialInstanceName);
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
                        VulkanDebug::ScopedRegion region(commandBuffer, objectPtr->GetName(), VulkanDebug::DebugCategory::eObject);
                        // 绑定DescriptorSet
                        commandBuffer.bindDescriptorSets(
                            vk::PipelineBindPoint::eGraphics,
                            renderPipeline.GetPipelineLayout(),
                            0,
                            objectPtr->GetDescriptorSets(swapChainImageIndex),
                            nullptr);
                        this->UpdateUBOModel(objectPtr);
                        objectPtr->GetRenderableObject()->Draw(commandBuffer, renderPass.width, renderPass.height);
                    }
                }
            }
        }
        else // 后处理都在这
        {
            // 获取输入资源
            for (const auto& input : renderPass.inputResources)
            {
                const RenderResource& inputResource = renderGraph.GetResourcesResolve()[input][swapChainImageIndex];
            }

            const std::weak_ptr<MaterialInstance> materialInstance = renderPass.materialInstance;
            if(materialInstance.expired())
            {
                std::cout << "materialInstance is expired" << std::endl;
                continue;
            }

            auto baseMaterial = materialInstance.lock()->GetBaseMaterial().lock();
            const PipelineBase& renderPipeline = *baseMaterial->GetRenderPipeline();
            const auto& shaderBindings = renderPipeline.GetShaderBindings();
            bool bNeedsGlobalSet = false;
            for(const auto& binding : shaderBindings)
            {
                if(binding.set == GlobalSetIndex)
                {
                    bNeedsGlobalSet = true;
                    break;
                }
            }
            if(bNeedsGlobalSet)
            {
                this->UpdateUBOGlobal(commandBuffer);
            }

            // 开始渲染通道
            vk::RenderPassBeginInfo renderPassBeginInfo;
            renderPassBeginInfo.setRenderPass(renderPass.renderPass);
            renderPassBeginInfo.setFramebuffer(renderPass.framebuffers[swapChainImageIndex]);
            renderPassBeginInfo.setRenderArea(vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(renderPass.width, renderPass.height)));
            renderPassBeginInfo.setClearValues(renderPass.clearValues);
            commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);
            commandBuffer.bindPipeline(renderPipeline.GetBindPoint(), renderPipeline.GetPipeline());

            for(uint32_t setIndex = 0; setIndex < renderPass.GetDescriptorSets()[swapChainImageIndex].size(); ++setIndex)
            {
                bool bNeedBindSet = false;
                for(const auto& binding : shaderBindings)
                {
                    if(binding.set == setIndex)
                    {
                        bNeedBindSet = true;
                        break;
                    }
                }
                if(!bNeedBindSet)
                {
                    continue;
                }

                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    renderPipeline.GetPipelineLayout(),
                    setIndex,
                    renderPass.GetDescriptorSets()[swapChainImageIndex][setIndex],
                    nullptr);
            }
            
            renderPass.Draw(commandBuffer);
        }
        commandBuffer.endRenderPass();

        // 添加Barrier, 确保pass之间的资源可见性（仅对后续会被采样的输出资源做 layout transition）
        PROFILE_SCOPE("Render:PassBarriers");
        enum class ResourceNextUse { None, Sampled, AttachmentWrite };
        auto FindNextUse = [&](const std::string& resourceName) -> ResourceNextUse
        {
            for(size_t nextIndex = passIndex + 1; nextIndex < renderPassOrdered.size(); ++nextIndex)
            {
                const auto& nextPass = renderGraph.GetRenderpasses().at(renderPassOrdered[nextIndex]);
                if (std::find(nextPass.inputResources.begin(), nextPass.inputResources.end(), resourceName) != nextPass.inputResources.end())
                {
                    return ResourceNextUse::Sampled;
                }
                if (std::find(nextPass.outputResources.begin(), nextPass.outputResources.end(), resourceName) != nextPass.outputResources.end())
                {
                    return ResourceNextUse::AttachmentWrite;
                }
            }
            return ResourceNextUse::None;
        };

        auto& resolveMap = renderGraph.GetResourcesResolve();
        for(const auto& resourceName : renderPass.outputResources)
        {
            if (resourceName == "swapChain") continue;

            if (FindNextUse(resourceName) != ResourceNextUse::Sampled)
            {
                continue;
            }

            auto it = resolveMap.find(resourceName);
            if (it == resolveMap.end())
            {
                continue;
            }

            auto& resource = it->second[swapChainImageIndex];
            const bool bIsDepth = CommonFunction::IsDepthFormat(resource.format);

            vk::ImageAspectFlags aspectMask = bIsDepth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
            if (bIsDepth && CommonFunction::HasStencilComponent(resource.format))
            {
                aspectMask |= vk::ImageAspectFlagBits::eStencil;
            }

            vk::ImageMemoryBarrier imageBarrier;
            imageBarrier
                .setImage(resource.image)
                .setSubresourceRange(vk::ImageSubresourceRange(aspectMask, 0, 1, 0, 1));

            vk::PipelineStageFlags srcStage;
            vk::PipelineStageFlags dstStage = vk::PipelineStageFlagBits::eFragmentShader;

            if (bIsDepth)
            {
                imageBarrier
                    .setSrcAccessMask(vk::AccessFlagBits::eDepthStencilAttachmentWrite)
                    .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
                    .setOldLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
                    .setNewLayout(vk::ImageLayout::eDepthStencilReadOnlyOptimal);

                srcStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
            }
            else
            {
                imageBarrier
                    .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
                    .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
                    .setOldLayout(vk::ImageLayout::eColorAttachmentOptimal)
                    .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

                srcStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
            }

            commandBuffer.pipelineBarrier(
                srcStage,
                dstStage,
                vk::DependencyFlagBits::eByRegion,
                0, nullptr,
                0, nullptr,
                1, &imageBarrier);
        }
    }

    // 结束渲染通道和Command Buffer记录
    commandBuffer.end();

    // 提交Command Buffer
    {
        PROFILE_SCOPE("Render:Submit");
        vk::PipelineStageFlags waitDstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        vk::SubmitInfo submitInfo;
        submitInfo
            .setWaitSemaphores(imageAcquiredSemaphore)
            .setSignalSemaphores(renderFinishedSemaphore)
            .setWaitDstStageMask(waitDstStageMask)
            .setCommandBuffers(commandBuffer);
        
        graphicQueue.submit(submitInfo, taskFinishedFence);
    }
    
    // 呈现
    vk::PresentInfoKHR presentInfo;
    presentInfo
        .setSwapchains(swapChain)
        .setImageIndices(swapChainImageIndex)
        .setWaitSemaphores(renderFinishedSemaphore);

    {
        PROFILE_SCOPE("Render:Present");
        result = graphicQueue.presentKHR(presentInfo);
    }
    if(result != vk::Result::eSuccess) {
        throw std::runtime_error("Failed to present image");
    }

    // 更新帧索引
    currentFrame = currentFrame + 1;
}

void RenderSystem::UpdateUBOGlobal(vk::CommandBuffer& commandBuffer)
{
    PROFILE_FUNCTION();
    static const auto& sceneLoader = SceneLoader::GetInstance();
    static Camera& camera = *sceneLoader.GetCamera();

    static UBOGlobal ubo;
    ubo.view = camera.GetViewMatrix();
    ubo.projection = camera.GetProjectionMatrix();
    ubo.invView = ubo.view.inverse();
    ubo.invProjection = ubo.projection.inverse();
    ubo.viewProjection = ubo.projection * ubo.view;
    ubo.invViewProjection = ubo.viewProjection.inverse();
    ubo.lightViewProj = lightViewProj;
    ubo.cameraPosition = sceneLoader.GetCamera()->GetPosition();

    //std::memcpy(uboGlobal.buffersMapped[swapChainImageIndex], &ubo, sizeof(ubo));
    commandBuffer.updateBuffer(uboGlobal.buffers[swapChainImageIndex], 0, sizeof(ubo), &ubo);
    vk::BufferMemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eUniformRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(uboGlobal.buffers[swapChainImageIndex])
        .setOffset(0)
        .setSize(sizeof(ubo));

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        0, nullptr,
        1, &barrier,
        0, nullptr
    );
}

void RenderSystem::UpdateUBOGlobalForShadow(vk::CommandBuffer& commandBuffer, uint32_t PassSizeWidth, uint32_t PassSizeHeight)
{
    PROFILE_FUNCTION();
    // 建立一个与光源同轴（Z）的shadowCoordinateSystem: 坐标系统原点为世界中心，旋转轴使用光源的旋转矩阵
    // 将点集从世界空间转换到shadowCoordinateSystem
    //  ->shadowCoordinateSystem
    //      获取一组Z=0的投影点集(Eigen::Vector2f)
    //      获取凸包点序(uint32_t)
    //      对每条边都建立EdgeCoordinateSystem(2D坐标系，方便加速运算)
    //      ->EdgeCoordinateSystem
    //          将凸包点转换到EdgeCoordinateSystem
    //          计算AABB，获取EdgeLengthMax
    //          在循环中找到最小的EdgeLengthMax
    //              找到后，记录EdgeLengthMax，记录CenterInEdge坐标(Eigen::Vector2f)
    //      <-shadowCoordinateSystem
    //      计算CenterInShadow(Eigen::Vector3f):CenterInEdge坐标转换到shadowCoordinateSystem
    //      计算ZNear和ZFar
    //      获取ShadowCameraPositonInShadow(Eigen::Vector3f):(CenterInShadow.x(), CenterInShadow.y(), ZNear)
    //  <-shadowCoordinateSystem
    //  计算ShadowCameraPositon(Eigen::Vector3f)
    static const auto& sceneLoader = SceneLoader::GetInstance();
    static Camera& camera = *sceneLoader.GetCamera();

    // 1. 先获取相机的数据
    Eigen::Vector3f cameraPosition = camera.GetPosition();
    Eigen::Vector3f cameraDirection = camera.GetForwardVector();
        // near, far
    float cameraNear = camera.GetClipNear();
    float cameraFar = camera.GetClipFar();
    cameraFar = 10.0f;
    float frustumPadding = 0.1f;

    Eigen::Vector3f cameraRight = camera.GetRightVector();
    Eigen::Vector3f cameraUp = camera.GetUpVector();
    float cameraHFov = camera.GetHFOV();
    float aspect = static_cast<float>(CommonFunction::GetWindowSize().x()) / static_cast<float>(CommonFunction::GetWindowSize().y());
    float cameraHFovRad = cameraHFov * static_cast<float>(M_PI) / 180.0f;
    Eigen::Matrix4f viewMatrix = camera.GetViewMatrix();

    float tanHalfFov = std::tan(cameraHFovRad * 0.5f);
    float nearHalfWidth = tanHalfFov * cameraNear;
    float nearHalfHeight = nearHalfWidth / aspect;
    float farHalfWidth = tanHalfFov * cameraFar;
    float farHalfHeight = farHalfWidth / aspect;

    Eigen::Vector3f nearCenter = cameraPosition + cameraDirection * cameraNear;
    Eigen::Vector3f farCenter = cameraPosition + cameraDirection * cameraFar;

    thread_local std::vector<Eigen::Vector3f> frustumPoints;
    frustumPoints.clear();
    frustumPoints.reserve(8);
    frustumPoints.emplace_back(nearCenter + cameraUp * nearHalfHeight - cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter + cameraUp * nearHalfHeight + cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter - cameraUp * nearHalfHeight - cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(nearCenter - cameraUp * nearHalfHeight + cameraRight * nearHalfWidth);
    frustumPoints.emplace_back(farCenter + cameraUp * farHalfHeight - cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter + cameraUp * farHalfHeight + cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter - cameraUp * farHalfHeight - cameraRight * farHalfWidth);
    frustumPoints.emplace_back(farCenter - cameraUp * farHalfHeight + cameraRight * farHalfWidth);
        
    // 2. 计算阴影映射相机的位置
        // 2.1以世界中心为原点，使用光源的旋转矩阵 构建shadowCoordinateSystem
    auto directionalLightIt = sceneLoader.GetDirectionalLights().begin();
    if (directionalLightIt == sceneLoader.GetDirectionalLights().end())
    {
        return;
    }
    std::weak_ptr<DirectionalLight> directionalLight = directionalLightIt->second;
        // 2.1构建worldCoordinateSystem -> shadowCoordinateSystem
    Eigen::Matrix3f worldToShadowMatrix;
    worldToShadowMatrix = CommonFunction::RotationToMatrix(directionalLight.lock()->GetRotation()).block<3, 3>(0, 0).transpose();

    // 2.3将点转换到shadowCoordinateSystem
    thread_local std::vector<Eigen::Vector3f> pointsInShadowSys;
    pointsInShadowSys.clear();
    pointsInShadowSys.reserve(frustumPoints.size());
    for (const auto& point : frustumPoints)
    {
        pointsInShadowSys.emplace_back(worldToShadowMatrix * point);
    }
    
    // ====================================================================================================
    // STABILIZATION LOGIC START
    // ====================================================================================================
    ShadowStrategy strategy = ShadowStrategy::StableBoundingSphere; // Default strategy: StableBoundingSphere (Safest)
    // Note: StableRectangular requires handling non-square shadow maps or viewport resizing to maintain isotropic texel density.
    // Since our physical shadow map texture is likely square, mapping a rectangular projection to it causes stretching/anisotropy.
    
    ShadowProjectionParams params;

    // Calculate Z bounds first as they are needed for all strategies
    float defaultMinZ = std::numeric_limits<float>::max();
    float defaultMaxZ = std::numeric_limits<float>::lowest();
    for(const auto& point : pointsInShadowSys)
    {
        defaultMaxZ = std::max(defaultMaxZ, point.z());
        defaultMinZ = std::min(defaultMinZ, point.z());
    }

    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();
    bool hasObjectBounds = false;

    Eigen::Vector3f shadowAxisWorld = worldToShadowMatrix.transpose() * Eigen::Vector3f(0.0f, 0.0f, 1.0f);

    #if !defined(VULKANLEARN_SHADOW_TIGHT_Z_BOUNDS)
        #define VULKANLEARN_SHADOW_TIGHT_Z_BOUNDS 0
    #endif

    #if VULKANLEARN_SHADOW_TIGHT_Z_BOUNDS
        for (const auto& [objectName, sceneObject] : sceneLoader.GetSceneObjects())
        {
            if (!sceneObject) continue;
            const auto& renderableObject = sceneObject->GetRenderableObject();
            if (!renderableObject) continue;

            const auto& localMin = renderableObject->GetBoundsMin();
            const auto& localMax = renderableObject->GetBoundsMax();
            const auto& modelMatrix = sceneObject->GetModelMatrix();
            auto worldCorners = BuildWorldCorners(localMin, localMax, modelMatrix);

            Eigen::Vector3f viewMin;
            Eigen::Vector3f viewMax;
            ComputeViewAabbFromWorldCorners(viewMatrix, worldCorners, viewMin, viewMax);

            if (!IntersectsSplitFrustumFast(viewMin, viewMax, cameraNear, cameraFar, cameraHFovRad, aspect, frustumPadding))
            {
                continue;
            }

            Eigen::Vector3f worldMin;
            Eigen::Vector3f worldMax;
            ComputeAabbFromCorners(worldCorners, worldMin, worldMax);

            auto axisRange = ComputeMinMaxAlongAxis(worldMin, worldMax, shadowAxisWorld);
            minZ = std::min(minZ, axisRange.first);
            maxZ = std::max(maxZ, axisRange.second);
            hasObjectBounds = true;
        }
    #endif

    if (!hasObjectBounds)
    {
        minZ = defaultMinZ;
        maxZ = defaultMaxZ;
    }

    float zNear = 0.0f;
    float zFar = maxZ - minZ;
    if (zFar < 0.1f) zFar = 1.0f;

    switch (strategy)
    {
    case ShadowStrategy::DynamicTightBox:
        params = CalculateShadowMatrix_DynamicTight(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), maxZ, zFar);
        break;
    case ShadowStrategy::StableBoundingSphere:
        params = CalculateShadowMatrix_StableSphere(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), static_cast<float>(PassSizeWidth), maxZ, zFar);
        break;
    case ShadowStrategy::StableRectangular:
        params = CalculateShadowMatrix_StableRectangular(pointsInShadowSys, worldToShadowMatrix.block<3, 3>(0, 0), static_cast<float>(PassSizeWidth), maxZ, zFar);
        break;
    }

    static UBOGlobal ubo;
    ubo.view = params.viewMatrix;
    ubo.projection = params.projectionMatrix;
    ubo.invView = ubo.view.inverse();
    ubo.invProjection = ubo.projection.inverse();
    ubo.viewProjection = ubo.projection * ubo.view;
    ubo.invViewProjection = ubo.viewProjection.inverse();
    lightViewProj = ubo.projection * ubo.view;
    ubo.lightViewProj = lightViewProj;
    
    {
        Eigen::Matrix3f rotT = ubo.view.block<3, 3>(0, 0);
        Eigen::Vector3f trans = ubo.view.block<3, 1>(0, 3);
        ubo.cameraPosition = -(rotT.transpose() * trans);
    }

    commandBuffer.updateBuffer(uboGlobal.buffers[swapChainImageIndex], 0, sizeof(ubo), &ubo);

    vk::BufferMemoryBarrier barrier;
    barrier.setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eUniformRead)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setBuffer(uboGlobal.buffers[swapChainImageIndex])
        .setOffset(0)
        .setSize(sizeof(ubo));

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        0, nullptr,
        1, &barrier,
        0, nullptr
    );
}

// ====================================================================================================
// Shadow Strategy Implementations
// ====================================================================================================

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_DynamicTight(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys, 
    const Eigen::Matrix3f& worldToShadowRotation, // Not used here as points are already transformed? 
                                                  // Wait, if we want to rotate the box, we need to know the base rotation?
                                                  // Points are in "Light Aligned World Space" (Shadow Space).
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. 进行凸包点查找，获取index
    std::vector<uint32_t> cullPointIndex;
    cullPointIndex = Algorithm::ConvexHull(pointsInShadowSys);
    
    // 2. 根据这些凸包点来计算面积最小的方形区域（等价于边长最小的方形区域）
    float minLength = std::numeric_limits<float>::max();
    Eigen::Vector2f centerInEdgeCoord;
    Eigen::Matrix2f shadowToEdgeCoordMatrix = Eigen::Matrix2f::Identity();
    Eigen::Vector2f EdgeCoordOriginInShadowSys;
    
    // 遍历所有边，将平面坐标系旋转到该边的方向，计算方形的aabb, 求得面积
    // 这样就能找到最小的方形区域
    for (size_t i = 0; i < cullPointIndex.size(); i++)
    {
        // 2.4.1 以p1为中心，p2-p1为方向，构建平面坐标系
            uint32_t p1Idx = cullPointIndex[i];
            uint32_t p2Idx = cullPointIndex[(i + 1) % cullPointIndex.size()];
            Eigen::Vector3f p1 = pointsInShadowSys[p1Idx];
            Eigen::Vector3f p2 = pointsInShadowSys[p2Idx];

            Eigen::Vector2f edge(p2.x() - p1.x(), p2.y() - p1.y());
            if (edge.norm() < 1e-6) continue;
            edge.normalize();

            // R = [ c  -s ]  这里是左乘
            //     [ s c ]
            float c = edge.x();
            float s = edge.y();
            Eigen::Matrix2f _shadowToEdgeCoordMatrix;
            _shadowToEdgeCoordMatrix << c, -s,
                        s, c;
            // 2.4.2 求取该平面变换后的aabb
            float minX = std::numeric_limits<float>::max();
            float maxX = std::numeric_limits<float>::lowest();
            float minY = std::numeric_limits<float>::max();
            float maxY = std::numeric_limits<float>::lowest();

            for (auto idx : cullPointIndex)
            {
                Eigen::Vector2f rotatedP = _shadowToEdgeCoordMatrix * (Eigen::Vector2f(pointsInShadowSys[idx].x(), pointsInShadowSys[idx].y()) - Eigen::Vector2f(p1.x(), p1.y()));
                minX = std::min(minX, rotatedP.x());
                maxX = std::max(maxX, rotatedP.x());
                minY = std::min(minY, rotatedP.y());
                maxY = std::max(maxY, rotatedP.y());
            }
            float width = std::abs(maxX - minX);
            float height = std::abs(maxY - minY);
            float length = std::max(width, height);
            // 2.4.3 有最小的length, 则找到了最小的方形区域
            if(length < minLength)
            {
                minLength = length;
                centerInEdgeCoord = Eigen::Vector2f((minX + maxX) / 2.0f, (minY + maxY) / 2.0f);
                shadowToEdgeCoordMatrix = _shadowToEdgeCoordMatrix;
                EdgeCoordOriginInShadowSys = Eigen::Vector2f(p1.x(), p1.y());
            }
    }
    
    // 3. 计算中心点的位置
    // x, y 在shadowCoordinateSystem中的值
    Eigen::Vector2f center2DInShadowSys = shadowToEdgeCoordMatrix.transpose() * centerInEdgeCoord + EdgeCoordOriginInShadowSys;

    Eigen::Vector3f ShadowCameraPositionInShadowSys = Eigen::Vector3f(center2DInShadowSys.x(), center2DInShadowSys.y(), sceneMaxZ);
    
    // 4. 计算shadowCamera的位置 (World Space)
    Eigen::Vector3f shadowCameraPosition = worldToShadowRotation.transpose() * ShadowCameraPositionInShadowSys;

    // 更新Camera
    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPosition, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose())));

    // 计算Near/Far
    float nearPlane = 0.0f;
    float farPlane = sceneZRange;

    shadowCamera.SetOrthographic(
        minLength, 
        1.0f, // Aspect ratio
        nearPlane, 
        farPlane);

    RenderSystem::ShadowProjectionParams params;
    
    // Apply the extra rotation (rollMatrix) to the View Matrix
    Eigen::Matrix4f rollMatrix = Eigen::Matrix4f::Identity();
    rollMatrix(0, 0) = shadowToEdgeCoordMatrix(0, 0);
    rollMatrix(0, 1) = shadowToEdgeCoordMatrix(0, 1);
    rollMatrix(1, 0) = shadowToEdgeCoordMatrix(1, 0);
    rollMatrix(1, 1) = shadowToEdgeCoordMatrix(1, 1);
    
    params.viewMatrix = rollMatrix * shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_StableSphere(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys, 
    const Eigen::Matrix3f& worldToShadowRotation, 
    float shadowMapResolution, 
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. Calculate Frustum Center
    Eigen::Vector3f frustumCenter = Eigen::Vector3f::Zero();
    for(const auto& p : pointsInShadowSys) {
        frustumCenter += p;
    }
    frustumCenter /= static_cast<float>(pointsInShadowSys.size());

    // 2. Calculate Radius
    float radius = 0.0f;
    for(const auto& p : pointsInShadowSys) {
        radius = std::max(radius, (p - frustumCenter).norm());
    }
    radius = std::ceil(radius * 16.0f) / 16.0f;

    // 3. Snapping
    float worldUnitsPerTexel = (2.0f * radius) / shadowMapResolution;
    frustumCenter.x() = std::floor(frustumCenter.x() / worldUnitsPerTexel) * worldUnitsPerTexel;
    frustumCenter.y() = std::floor(frustumCenter.y() / worldUnitsPerTexel) * worldUnitsPerTexel;

    // 4. Construct Camera
    // Camera placed at the center of the sphere in X/Y, but at sceneMaxZ in Z
    Eigen::Vector3f shadowCameraPosition = frustumCenter;
    shadowCameraPosition.z() = sceneMaxZ;
    
    // Transform back to World Space for Camera.SetCamera
    Eigen::Vector3f shadowCameraPositionWorld = worldToShadowRotation.transpose() * shadowCameraPosition;
    
    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPositionWorld, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose()))); // Use Light Rotation

    // 5. Construct Projection
    shadowCamera.SetOrthographic(
        radius * 2.0f, 
        1.0f, // Aspect ratio is 1.0
        0.0f, // Near plane relative to camera
        sceneZRange // Far plane
    );

    RenderSystem::ShadowProjectionParams params;
    params.viewMatrix = shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

RenderSystem::ShadowProjectionParams RenderSystem::CalculateShadowMatrix_StableRectangular(
    const std::vector<Eigen::Vector3f>& pointsInShadowSys, 
    const Eigen::Matrix3f& worldToShadowRotation, 
    float shadowMapResolution, 
    float sceneMaxZ, 
    float sceneZRange)
{
    PROFILE_FUNCTION();
    // 1. Calculate AABB of Frustum Slice in Shadow Space
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();

    for(const auto& p : pointsInShadowSys) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }

    // 2. Snapping
    // Based on the diagonal of the frustum slice to ensure stability during rotation?
    // Actually, for Rectangular, we usually just snap to texel grid.
    // However, if the frustum rotates, the AABB size changes.
    // Stable Rectangular usually implies the AABB is calculated from the bounding sphere of the frustum split,
    // OR we accept that the shadow map covers the current frustum AABB (tight fit) but snap the center/edges.
    
    // Let's implement basic snapping for the AABB edges.
    Eigen::Vector3f diagonal = pointsInShadowSys[6] - pointsInShadowSys[0]; // FarTopRight - NearBottomLeft (Approx)
    float diagonalLength = diagonal.norm();
    float worldUnitsPerTexel = diagonalLength / shadowMapResolution;
    
    // Snap min/max to this unit
    minX = std::floor(minX / worldUnitsPerTexel) * worldUnitsPerTexel;
    maxX = std::floor(maxX / worldUnitsPerTexel) * worldUnitsPerTexel;
    minY = std::floor(minY / worldUnitsPerTexel) * worldUnitsPerTexel;
    maxY = std::floor(maxY / worldUnitsPerTexel) * worldUnitsPerTexel;
    
    float width = maxX - minX;
    float height = maxY - minY;
    float centerX = (minX + maxX) * 0.5f;
    float centerY = (minY + maxY) * 0.5f;

    // 3. Construct Camera
    Eigen::Vector3f shadowCameraPosition = Eigen::Vector3f(centerX, centerY, sceneMaxZ);
    Eigen::Vector3f shadowCameraPositionWorld = worldToShadowRotation.transpose() * shadowCameraPosition;

    Camera shadowCamera;
    shadowCamera.SetCamera(shadowCameraPositionWorld, CommonFunction::QuatToRotation(Eigen::Quaternionf(worldToShadowRotation.transpose())));

    // 4. Construct Projection
    // Note: SetOrthographic usually takes (size, aspect, near, far)
    // We need to pass width and height explicitly.
    // If SetOrthographic only takes size (height) and aspect, we calculate aspect.
    
    float aspect = width / height;
    
    shadowCamera.SetOrthographic(
        height, // Or width? Usually size refers to height or max dimension. Let's assume height if aspect is w/h.
        aspect,
        0.0f,
        sceneZRange
    );
    // Note: My Camera::SetOrthographic might implement size as "height". Let's verify or assume standard behavior.
    // If it takes "size" as vertical size:
    // Width = Size * Aspect = Height * (Width/Height) = Width. Correct.

    RenderSystem::ShadowProjectionParams params;
    params.viewMatrix = shadowCamera.GetViewMatrix();
    params.projectionMatrix = shadowCamera.GetProjectionMatrix();
    return params;
}

void RenderSystem::UpdateUBOMaterialInstance(const std::shared_ptr<MaterialInstance>& materialInstance)
{
    PROFILE_FUNCTION();
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
    PROFILE_FUNCTION();
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
        vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst;
        vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        for(int i = 0; i < swapChainImageCount; i++)
        {
            std::tie(ubo->buffers[i], ubo->bufferMemories[i]) = CommonFunction::CreateBuffer(
                device,
                bufferSize, 
                usage, 
                VulkanManager::GetInstance().GetGpuMemoryProperties(), 
                memoryPropertyFlags,
                "UBO_Global (SwapchainIndex " + std::to_string(i) + ")"
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

std::pair<float, float> RenderSystem::ComputeMinMaxAlongAxis(const Eigen::Vector3f& aabbMin, const Eigen::Vector3f& aabbMax, const Eigen::Vector3f& axis) const
{
    Eigen::Vector3f dir = axis;
    float len = dir.norm();
    if (len > 0.0f)
    {
        dir /= len;
    }
    std::array<Eigen::Vector3f, 8> corners = {
        Eigen::Vector3f(aabbMin.x(), aabbMin.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMin.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMax.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMax.y(), aabbMin.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMin.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMin.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMin.x(), aabbMax.y(), aabbMax.z()),
        Eigen::Vector3f(aabbMax.x(), aabbMax.y(), aabbMax.z())
    };
    float minProj = dir.dot(corners[0]);
    float maxProj = minProj;
    for (size_t i = 1; i < corners.size(); ++i)
    {
        float proj = dir.dot(corners[i]);
        minProj = std::min(minProj, proj);
        maxProj = std::max(maxProj, proj);
    }
    return { minProj, maxProj };
}

std::array<Eigen::Vector3f, 8> RenderSystem::BuildWorldCorners(const Eigen::Vector3f& localMin, const Eigen::Vector3f& localMax, const Eigen::Matrix4f& modelMatrix)
{
    return {
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMin.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMin.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMax.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMax.y(), localMin.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMin.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMin.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMin.x(), localMax.y(), localMax.z(), 1.0f)).head<3>(),
        (modelMatrix * Eigen::Vector4f(localMax.x(), localMax.y(), localMax.z(), 1.0f)).head<3>()
    };
}

void RenderSystem::ComputeAabbFromCorners(const std::array<Eigen::Vector3f, 8>& corners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax)
{
    outMin = corners[0];
    outMax = corners[0];
    for (const auto& corner : corners)
    {
        outMin = outMin.cwiseMin(corner);
        outMax = outMax.cwiseMax(corner);
    }
}

void RenderSystem::ComputeViewAabbFromWorldCorners(const Eigen::Matrix4f& viewMatrix, const std::array<Eigen::Vector3f, 8>& worldCorners, Eigen::Vector3f& outMin, Eigen::Vector3f& outMax)
{
    outMin = (viewMatrix * Eigen::Vector4f(worldCorners[0].x(), worldCorners[0].y(), worldCorners[0].z(), 1.0f)).head<3>();
    outMax = outMin;
    for (const auto& corner : worldCorners)
    {
        Eigen::Vector3f viewCorner = (viewMatrix * Eigen::Vector4f(corner.x(), corner.y(), corner.z(), 1.0f)).head<3>();
        outMin = outMin.cwiseMin(viewCorner);
        outMax = outMax.cwiseMax(viewCorner);
    }
}

bool RenderSystem::IntersectsSplitFrustumFast(const Eigen::Vector3f& viewMin, const Eigen::Vector3f& viewMax, float splitNear, float splitFar, float fovRad, float aspect, float padding)
{
    float minZView = viewMin.z();
    float maxZView = viewMax.z();
    float splitNearP = std::max(0.0f, splitNear - padding);
    float splitFarP = splitFar + padding;
    if (maxZView < -splitFarP || minZView > -splitNearP)
    {
        return false;
    }
    float zForXY = std::min(minZView, -splitNearP);
    float distance = std::max(0.0f, -zForXY);
    float halfWidth = std::tan(fovRad * 0.5f) * distance;
    float halfHeight = halfWidth / aspect;
    if (viewMax.x() < -halfWidth || viewMin.x() > halfWidth || viewMax.y() < -halfHeight || viewMin.y() > halfHeight)
    {
        return false;
    }
    return true;
}

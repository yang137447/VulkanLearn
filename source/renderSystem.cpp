#include "renderSystem.h"
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

void RenderSystem::InitRenderObject()
{
    auto& scene = SceneLoader::GetInstance();
    // 设置相机
    auto& matrix = Matrix::GetInstance();
    matrix.SetCamera(scene.GetCamera()->GetPosition(), Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(0, 1, 0));
    matrix.SetProjection(scene.GetCamera()->GetHFOV(), (float)width/(float)height, scene.GetCamera()->GetClipNear(), scene.GetCamera()->GetClipFar());
    
    // 将场景中的物体按材质分组
    for(const auto& [objectName, sceneObject] : scene.GetSceneObjects())
    {
        auto& shaderName = sceneObject->GetMaterialInstance()->GetBaseMaterial()->GetShaderName();
        objectsByMaterial[shaderName].push_back(sceneObject);
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

    // 渲染每种材质
    for (const auto& [shaderName, objects] : objectsByMaterial) {
        // 绑定Pipeline
        const RenderPipline& renderPipline = *SceneLoader::GetInstance().GetMaterials().at(shaderName)->GetRenderPipline();
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, renderPipline.GetGraphicsPipeline());
        
        // 按材质实例分组
        std::unordered_map<std::string, std::vector<std::shared_ptr<SceneObject>>> objectsByInstance;  //材质实例名字，对象列表
        for (auto& object : objects) {
            objectsByInstance[object->GetMaterialInstance()->GetName()].push_back(object);
        }
        
        // 渲染每个材质实例
        for (auto& [materialInstanceName, instanceObjects] : objectsByInstance) {
            // // 绑定DescriptorSet
            // commandBuffer.bindDescriptorSets(
            //     vk::PipelineBindPoint::eGraphics,
            //     renderPipline.GetPipelineLayout(),
            //     0,
            //     renderPipline.GetDescriptorSets()[currentFrame],
            //     nullptr);

            // 渲染使用该材质实例的所有对象
            for (auto& object : instanceObjects) {
                // 绑定DescriptorSet
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    renderPipline.GetPipelineLayout(),
                    0,
                    object->GetDescriptorSets()[currentFrame],
                    nullptr);
                UpdateUniformBuffer(object);
                object->GetRenderableObject()->Draw(commandBuffer);
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


void RenderSystem::UpdateUniformBuffer(const std::shared_ptr<SceneObject> object)
{
    Matrix& matrix = Matrix::GetInstance();
    matrix.SetModelTransform(object->GetPosition(), object->GetRotation(), object->GetScale());

    static UniformBufferObject ubo;
    ubo.model = matrix.GetModelMatrix();
    ubo.view = matrix.GetViewMatrix();
    ubo.projection = matrix.GetProjectionMatrix();
    std::memcpy(object->GetUniformBuffersMapped()[currentFrame], &ubo, sizeof(ubo));
}
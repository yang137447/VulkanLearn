#include "renderSystem.h"
#include <iostream>
#include <limits>
#include <algorithm>
#include "sceneObject.h"
#include "materialInstance.h"
#include "material.h"
#include "vulkanManager.h"
#include "renderPipline.h"
#include "sceneLoader.h"
#include "commonFunction.h"
#include "texture.h"
#include "renderableObject.h"
#include "sceneLoader.h"
#include "lightManager.h"
#include "renderGraph.h"
#include "shaderReflect.h"

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
    static VulkanManager& instance = VulkanManager::GetInstance();
    static vk::Device& device = instance.GetDevice();
    static vk::SwapchainKHR& swapChain = instance.GetSwapChain();
    static uint32_t swapchainImageCount = instance.GetSwapChainImageCount();
    static vk::Queue& graphicQueue = instance.GetGraphicQueue();
    static LightManager& lightManager = LightManager::GetInstance();
    static RenderGraph& renderGraph = RenderGraph::GetInstance();
    static SceneLoader& sceneLoader = SceneLoader::GetInstance();

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

    for (const auto& renderPassName : renderGraph.GetRenderpassOrdered())
    {
        const auto& renderPass = renderGraph.GetRenderpasses().at(renderPassName);
        //TODO: 后续的shadow解决方案应该是生成专用的shadowshader以提高性能,当前先这样临时处理
        if (renderPassName == "shadow")
        {
            // 开始渲染通道
            this->UpdateUBOGlobalForShadow(commandBuffer, renderPass.width, renderPass.height);

            vk::RenderPassBeginInfo renderPassBeginInfo;
            renderPassBeginInfo.setRenderPass(renderPass.renderPass);
            renderPassBeginInfo.setFramebuffer(renderPass.framebuffers[0]);
            renderPassBeginInfo.setRenderArea(vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(renderPass.width, renderPass.height)));
            renderPassBeginInfo.setClearValues(renderPass.clearValues);
            commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);

            // 绑定Pipeline
            const RenderPipline& renderPipline = *sceneLoader.GetMaterials().at(renderPassName)->GetRenderPipline();
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, renderPipline.GetGraphicsPipeline());

            lightManager.UpdateLightBuffer(swapChainImageIndex);
            // 渲染每种材质
            for (const auto& [shaderName, shaderObjects] : hierarchyObjects) {
                // // 绑定Pipeline
                // const RenderPipline& renderPipline = *sceneLoader.GetMaterials().at(shaderName)->GetRenderPipline();
                // commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, renderPipline.GetGraphicsPipeline());
                
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
                        // 绑定DescriptorSet
                        const auto& descriptorSets = objectPtr->GetDescriptorSets(swapChainImageIndex);
                        commandBuffer.bindDescriptorSets(
                            vk::PipelineBindPoint::eGraphics,
                            renderPipline.GetPipelineLayout(),
                            GlobalSetIndex,
                            descriptorSets[GlobalSetIndex],
                            nullptr);
                        commandBuffer.bindDescriptorSets(
                            vk::PipelineBindPoint::eGraphics,
                            renderPipline.GetPipelineLayout(),
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
            renderPassBeginInfo.setFramebuffer(renderPass.framebuffers[0]);
            renderPassBeginInfo.setRenderArea(vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(renderPass.width, renderPass.height)));
            renderPassBeginInfo.setClearValues(renderPass.clearValues);
            commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);
            
            lightManager.UpdateLightBuffer(swapChainImageIndex);
            // 渲染每种材质
            for (const auto& [shaderName, shaderObjects] : hierarchyObjects) {
                // 绑定Pipeline
                const RenderPipline& renderPipline = *sceneLoader.GetMaterials().at(shaderName)->GetRenderPipline();
                commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, renderPipline.GetGraphicsPipeline());
                if (!renderPass.inputDescriptorSets.empty())
                {
                    bool bHasPassSet = false;
                    for(const auto& binding : renderPipline.GetShaderBindings())
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
                            renderPipline.GetPipelineLayout(),
                            PassSetIndex,
                            renderPass.inputDescriptorSets[0],
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
                        // 绑定DescriptorSet
                        commandBuffer.bindDescriptorSets(
                            vk::PipelineBindPoint::eGraphics,
                            renderPipline.GetPipelineLayout(),
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
                const RenderResource& inputResource = renderGraph.GetColorResourcesResolve()[input];
            }

            // 开始渲染通道
            vk::RenderPassBeginInfo renderPassBeginInfo;
            renderPassBeginInfo.setRenderPass(renderPass.renderPass);
            renderPassBeginInfo.setFramebuffer(renderPass.framebuffers[swapChainImageIndex]);
            renderPassBeginInfo.setRenderArea(vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(renderPass.width, renderPass.height)));
            renderPassBeginInfo.setClearValues(renderPass.clearValues);
            commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);
            
            const RenderPipline& renderPipline = *sceneLoader.GetMaterials().at(renderPassName)->GetRenderPipline();
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, renderPipline.GetGraphicsPipeline());

            // 绑定描述符集
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                renderPipline.GetPipelineLayout(),
                PassSetIndex,
                renderPass.inputDescriptorSets[0],
                nullptr);
            
            renderPass.Draw(commandBuffer);
        }
        commandBuffer.endRenderPass();

        // 添加Barrier, 确保pass之间的资源可见性
        for(const auto& resourceName : renderPass.outputResources)
        {
            if (resourceName == "swapChain" || resourceName == "sceneDepth") continue;
            
            if (resourceName == "shadowMap")
            {
                // 手动转换 ShadowMap格式
                auto& shadowMap = renderGraph.GetShadowMap();

                // 1. 将 Resolve 资源转换为 Shader Read (供后续 Pass 使用)
                vk::ImageMemoryBarrier barrierResolveToRead;
                barrierResolveToRead
                    .setSrcAccessMask(vk::AccessFlagBits::eDepthStencilAttachmentRead)
                    .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
                    .setOldLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
                    .setNewLayout(vk::ImageLayout::eDepthStencilReadOnlyOptimal)
                    .setImage(shadowMap.image)
                    .setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1));

                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
                    vk::PipelineStageFlagBits::eFragmentShader,
                    vk::DependencyFlagBits::eByRegion,
                    0, nullptr,
                    0, nullptr,
                    1, &barrierResolveToRead);
            }
            // 查找资源
            auto& resolveMap = renderGraph.GetColorResourcesResolve();
            if (resolveMap.find(resourceName) != resolveMap.end()) 
            {
                auto& resource = resolveMap.at(resourceName);
                vk::ImageMemoryBarrier imageBarrier;
                imageBarrier
                    .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
                    .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
                    .setOldLayout(vk::ImageLayout::eColorAttachmentOptimal)
                    .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
                    .setImage(resource.image)
                    .setSubresourceRange(vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
                
                commandBuffer.pipelineBarrier(
                    vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::PipelineStageFlagBits::eFragmentShader,
                    vk::DependencyFlagBits::eByRegion,
                    0, nullptr,
                    0, nullptr,
                    1, &imageBarrier);
            }
        }
    }

    // 结束渲染通道和Command Buffer记录
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

void RenderSystem::UpdateUBOGlobal(vk::CommandBuffer& commandBuffer)
{

    static const auto& sceneLoader = SceneLoader::GetInstance();
    static Camera& camera = *sceneLoader.GetCamera();

    static UBOGlobal ubo;
    ubo.view = camera.GetViewMatrix();
    ubo.projection = camera.GetProjectionMatrix();
    ubo.lightViewProj = lightViewProj;
    ubo.ambient = sceneLoader.GetAmbient();
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
        // 计算视锥体近平面四个点和远平面四个点（世界空间）
    std::vector<Eigen::Vector3f> FrustumPoints;
    Eigen::Vector3f cameraRight = camera.GetRightVector();
    Eigen::Vector3f cameraUp = camera.GetUpVector();
    float cameraHFov = camera.GetHFOV();
    float aspect = static_cast<float>(CommonFunction::GetWindowSize().x()) / static_cast<float>(CommonFunction::GetWindowSize().y());
    float cameraHFovRad = cameraHFov * static_cast<float>(M_PI) / 180.0f;
    float nearHalfWidth = std::tan(cameraHFovRad * 0.5f) * cameraNear;
    float nearHalfHeight = nearHalfWidth / aspect;
    float farHalfWidth = std::tan(cameraHFovRad * 0.5f) * cameraFar;
    float farHalfHeight = farHalfWidth / aspect;
    Eigen::Vector3f nearCenter = cameraPosition + cameraDirection * cameraNear;
    Eigen::Vector3f farCenter = cameraPosition + cameraDirection * cameraFar;
    Eigen::Vector3f nearTopLeft = nearCenter + cameraUp * nearHalfHeight - cameraRight * nearHalfWidth;
    Eigen::Vector3f nearTopRight = nearCenter + cameraUp * nearHalfHeight + cameraRight * nearHalfWidth;
    Eigen::Vector3f nearBottomLeft = nearCenter - cameraUp * nearHalfHeight - cameraRight * nearHalfWidth;
    Eigen::Vector3f nearBottomRight = nearCenter - cameraUp * nearHalfHeight + cameraRight * nearHalfWidth;
    Eigen::Vector3f farTopLeft = farCenter + cameraUp * farHalfHeight - cameraRight * farHalfWidth;
    Eigen::Vector3f farTopRight = farCenter + cameraUp * farHalfHeight + cameraRight * farHalfWidth;
    Eigen::Vector3f farBottomLeft = farCenter - cameraUp * farHalfHeight - cameraRight * farHalfWidth;
    Eigen::Vector3f farBottomRight = farCenter - cameraUp * farHalfHeight + cameraRight * farHalfWidth;
        // 存储近平面和远平面的四个点
    FrustumPoints.emplace_back(nearTopLeft);
    FrustumPoints.emplace_back(nearTopRight);
    FrustumPoints.emplace_back(nearBottomLeft);
    FrustumPoints.emplace_back(nearBottomRight);
    FrustumPoints.emplace_back(farTopLeft);
    FrustumPoints.emplace_back(farTopRight);
    FrustumPoints.emplace_back(farBottomLeft);
    FrustumPoints.emplace_back(farBottomRight);
        
    // 2. 计算阴影映射相机的位置
        // 2.1以世界中心为原点，使用光源的旋转矩阵 构建shadowCoordinateSystem
    std::weak_ptr<DirectinalLight> directionalLight;
    for(auto&[lightName, light] : sceneLoader.GetDirectinalLight())
    {
        directionalLight = light;
        break;
    }
        // 2.1构建worldCoordinateSystem -> shadowCoordinateSystem
    Eigen::Matrix3f worldToShadowMatrix;
    worldToShadowMatrix = CommonFunction::RotationToMatrix(directionalLight.lock()->GetRotation()).block<3, 3>(0, 0);

        // 2.3将点转换到shadowCoordinateSystem
    std::vector<Eigen::Vector3f> PointsInShadowSys;
    for(const auto& point : FrustumPoints)
    {
        PointsInShadowSys.emplace_back(worldToShadowMatrix * point);
    }
        // 2.3进行凸包点查找，获取index
    std::vector<uint32_t> cullPointIndex = Algorithm::ConvexHull(PointsInShadowSys);
        // 2.4 根据这些凸包点来计算面积最小的方形区域（等价于边长最小的方形区域）
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
        Eigen::Vector3f p1 = PointsInShadowSys[p1Idx];
        Eigen::Vector3f p2 = PointsInShadowSys[p2Idx];

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
            Eigen::Vector2f rotatedP = _shadowToEdgeCoordMatrix * (Eigen::Vector2f(PointsInShadowSys[idx].x(), PointsInShadowSys[idx].y()) - Eigen::Vector2f(p1.x(), p1.y()));
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
            // 2.4.4 计算中心点的位置
                // x, y 在shadowCoordinateSystem中的值
    Eigen::Vector2f center2DInShadowSys = shadowToEdgeCoordMatrix.transpose() * centerInEdgeCoord + EdgeCoordOriginInShadowSys;

    Eigen::Vector3f ShadowCameraPositionInShadowSys = Eigen::Vector3f(center2DInShadowSys.x(), center2DInShadowSys.y(), 0.0f);
    
                // z 在shadowCoordinateSystem中的值， 这个代表shadowCamera的Z值
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();
    for(const auto& point : PointsInShadowSys)
    {
        // Z axis is orthogonal to the 2D plane, so Z values are preserved
        maxZ = std::max(maxZ, point.z());
        minZ = std::min(minZ, point.z());
    }
    ShadowCameraPositionInShadowSys.z() = minZ;
        // 2.5 计算shadowCamera的位置
    Eigen::Vector3f shadowCameraPosition = worldToShadowMatrix.transpose() * ShadowCameraPositionInShadowSys;

    // 更新Camera
    // 使用临时Camera计算矩阵，避免修改主相机状态
    Camera shadowCamera;
    
    //shadowCamera.SetCamera(shadowCameraPosition, directionalLight.lock()->GetRotation());
    shadowCamera.SetCamera(shadowCameraPosition, directionalLight.lock()->GetRotation());

    // 计算Near/Far
    // Camera位于maxZ, 看向-Z方向. 点分布在[minZ, maxZ].
    // 相对Camera距离: Near = 0, Far = maxZ - minZ
    float nearPlane = 0.0f;
    float farPlane = maxZ - minZ;
    if (farPlane < 0.1f) farPlane = 1.0f; // 避免无效范围

    shadowCamera.SetOrthographic(
        minLength, 
        (float)PassSizeWidth/(float)PassSizeHeight, 
        nearPlane, 
        farPlane);

    static UBOGlobal ubo;
    Eigen::Matrix4f rollMatrix = Eigen::Matrix4f::Identity();
    rollMatrix(0, 0) = shadowToEdgeCoordMatrix(0, 0);
    rollMatrix(0, 1) = shadowToEdgeCoordMatrix(0, 1);
    rollMatrix(1, 0) = shadowToEdgeCoordMatrix(1, 0);
    rollMatrix(1, 1) = shadowToEdgeCoordMatrix(1, 1);
    ubo.view = rollMatrix * shadowCamera.GetViewMatrix();   //TODO: 这里需要验证roll方向正确性// rollMatrix相当于在shadowCoordinateSystem中进行旋转
    ubo.projection = shadowCamera.GetProjectionMatrix();
    lightViewProj = ubo.projection * ubo.view;
    ubo.lightViewProj = lightViewProj;
    //ubo.ambient = sceneLoader.GetAmbient();
    ubo.cameraPosition = shadowCameraPosition;

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
        vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst;
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

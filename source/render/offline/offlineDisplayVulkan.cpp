#include "render/offline/offlineDisplayVulkan.h"

#include <cstring>
#include <stdexcept>
#include "commonFunction.h"
#include "pipeline/graphicsPipelineBuilder.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderGraph.h"

namespace VL
{
OfflineDisplayVulkan::ViewportTarget::~ViewportTarget()
{
    if (backend) backend->DestroyFramebuffers(framebuffers);
}

void OfflineDisplayVulkan::Initialize(RendererBackendVulkan& rendererBackend)
{
    if (backend) return;
    backend = &rendererBackend;
    try
    {
        const auto extent = backend->GetSwapchainExtent();
        const uint32_t count = backend->GetSwapchainImageCount();
        vk::DescriptorSetLayoutBinding binding(
            0, vk::DescriptorType::eCombinedImageSampler, 1,
            vk::ShaderStageFlagBits::eFragment);
        vk::DescriptorSetLayoutCreateInfo layoutInfo;
        layoutInfo.setBindings(binding);
        descriptorLayout = backend->CreateDescriptorSetLayout(layoutInfo, "Offline film layout");
        vk::DescriptorPoolSize poolSize(vk::DescriptorType::eCombinedImageSampler, count);
        vk::DescriptorPoolCreateInfo poolInfo;
        poolInfo.setMaxSets(count).setPoolSizes(poolSize);
        descriptorPool = backend->CreateDescriptorPool(poolInfo, "Offline film pool");
        vk::PushConstantRange push(vk::ShaderStageFlagBits::eFragment, 0, 8);
        vk::PipelineLayoutCreateInfo pipelineInfo;
        pipelineInfo.setSetLayouts(descriptorLayout).setPushConstantRanges(push);
        pipelineLayout = backend->CreatePipelineLayout(pipelineInfo, "Offline display layout");

        vk::AttachmentDescription2 attachment;
        attachment.setFormat(backend->GetSwapchainImageFormat())
            .setSamples(vk::SampleCountFlagBits::e1)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStoreOp(vk::AttachmentStoreOp::eStore)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
        vk::AttachmentReference2 reference;
        reference.setAttachment(0).setLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setAspectMask(vk::ImageAspectFlagBits::eColor);
        vk::SubpassDescription2 subpass;
        subpass.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
            .setColorAttachments(reference);
        vk::SubpassDependency2 dependency;
        dependency.setSrcSubpass(VK_SUBPASS_EXTERNAL).setDstSubpass(0)
            .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
            .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
            .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite);
        vk::RenderPassCreateInfo2 passInfo;
        passInfo.setAttachments(attachment).setSubpasses(subpass).setDependencies(dependency);
        renderPass = backend->CreateRenderPass(passInfo, "Offline display");
        // format/sample/subpass 不变，与现有离线显示管线兼容；图像由 graph
        // 持有，离线显示只将线性 film 实际绘制到该目标，不读取 swapchain。
        attachment.setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal);
        passInfo.setAttachments(attachment);
        viewportRenderPass = backend->CreateRenderPass(passInfo, "Offline viewport display");
        framebuffers.reserve(count);
        for (const vk::ImageView view : backend->GetSwapchainImageViews())
        {
            vk::FramebufferCreateInfo info;
            info.setRenderPass(renderPass).setAttachments(view)
                .setWidth(extent.width).setHeight(extent.height).setLayers(1);
            framebuffers.push_back(backend->CreateFramebuffer(info, "Offline framebuffer"));
        }
        CreatePipeline();
        viewportTargets.resize(count);
        frames.resize(count);
        std::vector<vk::DescriptorSetLayout> layouts(count, descriptorLayout);
        std::vector<vk::DescriptorSet> descriptors(count);
        vk::DescriptorSetAllocateInfo allocation;
        allocation.setDescriptorPool(descriptorPool).setSetLayouts(layouts);
        backend->AllocateDescriptorSets(allocation, descriptors);
        for (uint32_t i = 0; i < count; ++i)
        {
            auto& frame = frames[i];
            const vk::DeviceSize bytes = vk::DeviceSize(extent.width) * extent.height * 4 * sizeof(float);
            const auto staging = backend->CreateBuffer(
                bytes, vk::BufferUsageFlagBits::eTransferSrc,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                "Offline film staging");
            frame.staging = staging.first;
            frame.stagingMemory = staging.second;
            frame.mapped = backend->MapMemory(frame.stagingMemory, bytes);
            const auto image = backend->CreateImage(
                extent.width, extent.height, 1, vk::SampleCountFlagBits::e1,
                vk::Format::eR32G32B32A32Sfloat, vk::ImageTiling::eOptimal,
                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
                vk::MemoryPropertyFlagBits::eDeviceLocal, "Offline linear film");
            frame.image = image.first;
            frame.imageMemory = image.second;
            frame.view = backend->Create2DImageView(frame.image, 1, vk::Format::eR32G32B32A32Sfloat,
                vk::ImageAspectFlagBits::eColor, "Offline film view");
            // LOD0 最近邻显示避免依赖浮点格式的 linear-filter 可选能力。
            frame.sampler = backend->Create2DSampler(vk::Filter::eNearest,
                vk::SamplerAddressMode::eClampToEdge, false, "Offline film sampler");
            frame.descriptor = descriptors[i];
            vk::DescriptorImageInfo imageInfo(frame.sampler, frame.view,
                vk::ImageLayout::eShaderReadOnlyOptimal);
            vk::WriteDescriptorSet write;
            write.setDstSet(frame.descriptor).setDstBinding(0)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler).setImageInfo(imageInfo);
            backend->UpdateDescriptorSets({write});
        }
    }
    catch (...)
    {
        Shutdown();
        throw;
    }
}

void OfflineDisplayVulkan::CreatePipeline()
{
    vk::ShaderModule vertex, fragment;
    try
    {
        const std::string vertexBytes = CommonFunction::ReadFile(
            CommonFunction::Path("shader/spv/offlineDisplay_vert.spv"));
        const std::string fragmentBytes = CommonFunction::ReadFile(
            CommonFunction::Path("shader/spv/offlineDisplay_frag.spv"));
        vk::ShaderModuleCreateInfo vertexInfo({}, vertexBytes.size(),
            reinterpret_cast<const uint32_t*>(vertexBytes.data()));
        vk::ShaderModuleCreateInfo fragmentInfo({}, fragmentBytes.size(),
            reinterpret_cast<const uint32_t*>(fragmentBytes.data()));
        vertex = backend->CreateShaderModule(vertexInfo, "Offline display vertex");
        fragment = backend->CreateShaderModule(fragmentInfo, "Offline display fragment");
        std::vector<vk::PipelineShaderStageCreateInfo> stages{
            {{}, vk::ShaderStageFlagBits::eVertex, vertex, "main"},
            {{}, vk::ShaderStageFlagBits::eFragment, fragment, "main"}};
        GraphicsPipelineStateDesc state;
        state.bUseVertexInput = false;
        state.bDepthTestEnable = false;
        state.bDepthWriteEnable = false;
        state.cullMode = vk::CullModeFlagBits::eNone;
        const vk::VertexInputBindingDescription binding;
        const std::vector<vk::VertexInputAttributeDescription> attributes;
        const std::string name = "Offline display";
        GraphicsPipelineBuildDesc desc{
            renderPass, pipelineLayout, stages, binding, attributes, name,
            vk::SampleCountFlagBits::e1, 1, state, false};
        const auto result = backend->BuildGraphicsPipeline(desc);
        pipeline = result.graphicsPipeline;
        pipelineCache = result.pipelineCache;
    }
    catch (...)
    {
        backend->DestroyShaderModule(vertex);
        backend->DestroyShaderModule(fragment);
        throw;
    }
    backend->DestroyShaderModule(vertex);
    backend->DestroyShaderModule(fragment);
}

void OfflineDisplayVulkan::Shutdown()
{
    if (!backend) return;
    // 仅在既有 swapchain/退出安全边界销毁；渐进发布不更换 GPU 资源。
    for (auto& frame : frames)
    {
        if (frame.mapped) backend->UnmapMemory(frame.stagingMemory);
        if (frame.staging) backend->DestroyBuffer(frame.staging, frame.stagingMemory);
        backend->DestroyImageResource(frame.image, frame.imageMemory, frame.view, frame.sampler);
    }
    frames.clear();
    backend->DestroyDescriptorPool(descriptorPool);
    backend->DestroyPipeline(pipeline);
    backend->DestroyPipelineCache(pipelineCache);
    backend->DestroyPipelineLayout(pipelineLayout);
    backend->DestroyDescriptorSetLayout(descriptorLayout);
    backend->DestroyFramebuffers(framebuffers);
    viewportTargets.clear();
    backend->DestroyRenderPass(renderPass);
    backend->DestroyRenderPass(viewportRenderPass);
    retainedSource = {};
    previewNearest.clear();
    backend = nullptr;
}

vk::Framebuffer OfflineDisplayVulkan::PrepareViewportFramebuffer(
    uint32_t imageIndex, const RenderResource& output, uint64_t ownerGeneration)
{
    auto& current = viewportTargets.at(imageIndex);
    const vk::ImageView view = output.GetFramebufferAttachmentView();
    if (current && current->imageView == view &&
        current->imageViewHandleId == output.imageViewHandles.at(0).id &&
        current->ownerGeneration == ownerGeneration)
    {
        return current->framebuffers.front();
    }

    // 只在新目标绑定边界校验；图源保证同代尺寸/格式不变，不做逐帧修正。
    const auto extent = backend->GetSwapchainExtent();
    if (output.width != extent.width || output.height != extent.height ||
        output.format != backend->GetSwapchainImageFormat() ||
        output.arrayLayers != 1 || !output.image || !output.imageView)
    {
        throw std::runtime_error(
            "Offline viewport output must match the single-layer swapchain backing extent/format");
    }
    auto candidate = std::make_shared<ViewportTarget>();
    candidate->backend = backend;
    candidate->imageView = view;
    candidate->imageViewHandleId = output.imageViewHandles.at(0).id;
    candidate->ownerGeneration = ownerGeneration;
    // 先预留容器再创建 Vulkan 对象，异常时 candidate 负责释放所有新句柄。
    candidate->framebuffers.reserve(1);
    vk::FramebufferCreateInfo info;
    info.setRenderPass(viewportRenderPass).setAttachments(view)
        .setWidth(extent.width).setHeight(extent.height).setLayers(1);
    candidate->framebuffers.push_back(
        backend->CreateFramebuffer(info, "Offline viewport framebuffer"));
    if (current)
    {
        auto& retireQueue = ResourceRetireQueue::GetInstance();
        std::vector<RetiredResource> retired;
        retired.push_back({"OfflineViewportFramebuffer:" + std::to_string(imageIndex),
            current->ownerGeneration, retireQueue.GetLastSubmittedEpoch(), current});
        auto prepared = retireQueue.PrepareRetirements(std::move(retired));
        // 队列扩容完成后才发布新引用；失败时旧缓存/在途 framebuffer 完整保留。
        current = std::move(candidate);
        retireQueue.CommitPreparedRetirements(std::move(prepared));
    }
    else
    {
        current = std::move(candidate);
    }
    return current->framebuffers.front();
}

void OfflineDisplayVulkan::Record(vk::CommandBuffer commandBuffer, uint32_t imageIndex,
    std::shared_ptr<const Offline::FilmSnapshot> film,
    std::shared_ptr<const Offline::PreviewSnapshot> preview,
    OfflineAov aov, float exposure,
    const RenderResource* viewportOutput, uint64_t viewportOwnerGeneration,
    vk::Extent2D logicalExtent)
{
    const vk::Framebuffer outputFramebuffer = viewportOutput
        ? PrepareViewportFramebuffer(imageIndex, *viewportOutput, viewportOwnerGeneration)
        : framebuffers.at(imageIndex);
    auto& frame = frames.at(imageIndex);
    const auto extent = backend->GetSwapchainExtent();
    // film 使用 Dock 内逻辑视口的相机宽高比；backing 图像只决定上传尺寸。
    // 显示时再由 UI 映射回逻辑矩形，两次缩放抵消，不裁剪 film 内容。
    const vk::Extent2D sourceExtent = logicalExtent.width == 0 && logicalExtent.height == 0
        ? extent : logicalExtent;
    auto selected = SelectOfflineDisplaySource(retainedSource,
        std::move(film), std::move(preview), sourceExtent.width, sourceExtent.height);
    if (selected.preview != retainedSource.preview)
        previewNearest = selected.preview
            ? OfflinePreviewNearestValid(*selected.preview) : std::vector<size_t>{};
    retainedSource = std::move(selected);
    // 同一份最新候选逐 slot 补传，不能随 swapchain image 轮转显示旧视角。
    const auto& source = retainedSource;
    const bool uploadFilm = source.film && NeedsOfflineFilmUpload(*source.film,
        sourceExtent.width, sourceExtent.height, aov, source.Generation(), frame.uploaded);
    const bool uploadPreview = source.preview && !previewNearest.empty() &&
        NeedsOfflinePreviewUpload(*source.preview, sourceExtent.width, sourceExtent.height,
            aov, frame.uploaded);
    if (uploadFilm || uploadPreview)
    {
        // 缺失像素只取同代预览内最近有效像素，绝不与旧相机 film 拼接。
        float* pixels = static_cast<float*>(frame.mapped);
        const uint32_t filmWidth = static_cast<uint32_t>(
            uploadFilm ? source.film->width : source.preview->width);
        const uint32_t filmHeight = static_cast<uint32_t>(
            uploadFilm ? source.film->height : source.preview->height);
        std::vector<uint32_t> sourceXs(extent.width);
        for (uint32_t x = 0; x < extent.width; ++x)
            sourceXs[x] = OfflineFilmSourceCoordinate(x, extent.width, filmWidth);
        for (uint32_t y = 0; y < extent.height; ++y)
        {
            const uint32_t sourceY = OfflineFilmSourceCoordinate(y, extent.height, filmHeight);
            for (uint32_t x = 0; x < extent.width; ++x)
            {
                const size_t pixel = size_t(sourceY) * filmWidth + sourceXs[x];
                const size_t sourceIndex = uploadFilm ? pixel : previewNearest[pixel];
                const size_t destination = (size_t(y) * extent.width + x) * 4;
                const auto& direct = uploadFilm ? source.film->direct : source.preview->direct;
                const auto& indirect = uploadFilm ? source.film->indirect : source.preview->indirect;
                const Offline::Vec3 color = uploadPreview
                    ? OfflinePreviewColor(*source.preview, sourceIndex, aov)
                    : aov == OfflineAov::Direct ? direct[sourceIndex]
                    : aov == OfflineAov::Indirect ? indirect[sourceIndex]
                    : direct[sourceIndex] + indirect[sourceIndex];
                pixels[destination] = static_cast<float>(color.x);
                pixels[destination + 1] = static_cast<float>(color.y);
                pixels[destination + 2] = static_cast<float>(color.z);
                pixels[destination + 3] = 1.0f;
            }
        }
        vk::ImageMemoryBarrier barrier;
        barrier.setImage(frame.image)
            .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setOldLayout(frame.uploaded.initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined)
            .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
            .setSrcAccessMask(frame.uploaded.initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags{})
            .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
        commandBuffer.pipelineBarrier(
            frame.uploaded.initialized ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
        vk::BufferImageCopy copy;
        copy.setImageSubresource({vk::ImageAspectFlagBits::eColor, 0, 0, 1})
            .setImageExtent({extent.width, extent.height, 1});
        commandBuffer.copyBufferToImage(frame.staging, frame.image,
            vk::ImageLayout::eTransferDstOptimal, copy);
        barrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
            .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
        commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, barrier);
        frame.uploaded = {source.Generation(), uploadFilm ? source.film->completedSamples : 0,
            aov, true, uploadPreview, uploadPreview ? source.preview->validCount : 0};
    }
    vk::ClearValue clear(vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 1}));
    vk::RenderPassBeginInfo pass;
    // CLEAR 使用 Undefined 丢弃目标旧内容；采样前的 ColorAttachment -> ShaderRead
    // 和复用时的同步由 RT 负责，离线显示不拥有 graph 的跨帧布局状态。
    pass.setRenderPass(viewportOutput ? viewportRenderPass : renderPass)
        .setFramebuffer(outputFramebuffer)
        .setRenderArea({{0, 0}, extent}).setClearValues(clear);
    commandBuffer.beginRenderPass(pass, vk::SubpassContents::eInline);
    if (frame.uploaded.initialized)
    {
        commandBuffer.setViewport(0, vk::Viewport(0, 0,
            static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1));
        commandBuffer.setScissor(0, vk::Rect2D({0, 0}, extent));
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
            pipelineLayout, 0, frame.descriptor, {});
        const auto format = backend->GetSwapchainImageFormat();
        const struct { float exposure; int encodeSrgb; } settings{
            exposure, (format == vk::Format::eB8G8R8A8Srgb ||
                       format == vk::Format::eR8G8B8A8Srgb) ? 0 : 1};
        commandBuffer.pushConstants(pipelineLayout, vk::ShaderStageFlagBits::eFragment,
            0, sizeof(settings), &settings);
        commandBuffer.draw(3, 1, 0, 0);
    }
    commandBuffer.endRenderPass();
}
} // namespace VL

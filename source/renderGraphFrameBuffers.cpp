#include "renderGraph.h"
#include "commonFunction.h"
#include "shaderReflect.h"
#include "material.h"
#include "material/materialAssetUtils.h"
#include "materialInstance.h"
#include "pipeline/graphicsPipeline.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorWriter.h"
#include "render/rendergraph/renderGraphCompiler.h"
#include "render/rendergraph/sceneViewportOutputValidation.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/resource/rendererResourceCache.h"
#include "render/camera/autoExposureCompute.h"
#include "render/backend/rendererDescriptorContext.h"
#include "render/volumetricFog/volumetricFogRenderer.h"
#include "render/lightCulling/lightCullingRenderer.h"
#include "hair/hairSimulationPass.h"
#include "render/skinning/skinDeformPass.h"
#include "texture.h"
#include <algorithm>
#include <memory>
#include <stdint.h>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <unordered_set>
#include "profiler.h"

vk::RenderPass RenderGraph::CreateVkRenderPass(
    Renderpass& renderpass,
    VL::RendererBackendVulkan& rendererBackend,
    bool bUseMsaa,
    bool renderToViewport)
{
    vk::SampleCountFlagBits sampleCount = bUseMsaa ? CommonFunction::GetMsaaSampleCount() : vk::SampleCountFlagBits::e1;
    const std::vector<std::string>& outputResources = renderpass.outputResources;

    // 总览：按输出资源顺序构建 RenderPass
    // 1) 颜色附件（MSAA/非MSAA）
    // 2) 如启用 MSAA，追加 resolve 附件
    // 3) 如包含深度资源，追加 depth 附件
    // 4) 构建 subpass 与依赖
    //创建渲染通道
    std::vector<vk::AttachmentDescription2> attachmentDescriptions;
    std::vector<vk::AttachmentReference2> colorAttachmentReferences;
    std::vector<vk::AttachmentReference2> resolveAttachmentReferences;
    vk::AttachmentReference2 depthAttachmentReference;
    vk::AttachmentReference2 depthResolveAttachmentReference;

    bool bHasDepth = false;

    // 1. Color Attachments (MSAA & Resolve)
    for (const auto& resourceName : outputResources)
    { 
        if (IsDepthResource(resourceName))
        {
            bHasDepth = true;
            continue;
        }

        vk::AttachmentDescription2 colorAttachmentDescription;
        vk::Format format = vk::Format::eUndefined;
        if (resourceName == "swapChain")
        {
            format = rendererBackend.GetSwapchainImageFormat();
        }
        else{
            format = resourcesMsaa.at(resourceName)[0].format;
        }

        // MSAA / Main Attachment
        const vk::AttachmentLoadOp loadOp = renderpass.outputLoadOps.at(resourceName);
        const vk::AttachmentStoreOp storeOp = renderpass.outputStoreOps.at(resourceName);
        colorAttachmentDescription
            .setFormat(format)
            .setSamples(sampleCount)
            .setLoadOp(loadOp)
            .setStoreOp(storeOp)
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setInitialLayout(loadOp == vk::AttachmentLoadOp::eLoad ? vk::ImageLayout::eColorAttachmentOptimal : vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal);

        if (resourceName == "swapChain" && !renderToViewport)
        {
            colorAttachmentDescription.setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
        }
        
        attachmentDescriptions.push_back(colorAttachmentDescription);

        vk::AttachmentReference2 attachmentReference;
        attachmentReference.setAttachment(colorAttachmentReferences.size());
        attachmentReference.setLayout(vk::ImageLayout::eColorAttachmentOptimal);
        attachmentReference.setAspectMask(vk::ImageAspectFlagBits::eColor);
        colorAttachmentReferences.push_back(attachmentReference);
    }
    
    // Fix up attachment indices for Color Refs
    // Current attachmentDescriptions size is colorAttachmentReferences.size()

    // 2. Resolve Attachments (Only if MSAA is enabled)
    if (bUseMsaa)
    {
        for (const auto& resourceName : outputResources)
        {
            if (IsDepthResource(resourceName)) continue;

            vk::AttachmentDescription2 resolveAttachmentDescription;
            vk::Format format = (resourceName == "swapChain")
                ? rendererBackend.GetSwapchainImageFormat()
                : resourcesMsaa.at(resourceName)[0].format;
            
            resolveAttachmentDescription
                .setFormat(format)
                .setSamples(vk::SampleCountFlagBits::e1)
                .setLoadOp(vk::AttachmentLoadOp::eDontCare)
                .setStoreOp(renderpass.outputStoreOps.at(resourceName))
                .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
                .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                .setInitialLayout(vk::ImageLayout::eUndefined)
                .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal);

            if (resourceName == "swapChain" && !renderToViewport)
            {
                resolveAttachmentDescription.setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
            }

            attachmentDescriptions.push_back(resolveAttachmentDescription);

            vk::AttachmentReference2 attachmentReference;
            attachmentReference.setAttachment(attachmentDescriptions.size() - 1);
            attachmentReference.setLayout(vk::ImageLayout::eColorAttachmentOptimal);
            attachmentReference.setAspectMask(vk::ImageAspectFlagBits::eColor);
            resolveAttachmentReferences.push_back(attachmentReference);
        }
    }

    // 3. Depth Attachment
    bool bHasDepthResolve = false;
    if (bHasDepth)
    {
        // Find depth resource
        std::string depthName = "";
        for(const auto& resource : outputResources)
        {
            if (IsDepthResource(resource))
            {
                depthName = resource;
                break;
            }
        }

        vk::AttachmentDescription2 depthStencilAttachmentDescription;
        vk::Format depthFormat = vk::Format::eUndefined;
        
        if(bUseMsaa) 
        {
            depthFormat = resourcesMsaa[depthName][0].format;
        }
        else {
            depthFormat = resourcesResolve[depthName][0].format;
        }

        depthStencilAttachmentDescription
            .setFormat(depthFormat)
            .setSamples(sampleCount)
            .setLoadOp(renderpass.outputLoadOps.at(depthName))
            .setStoreOp(renderpass.outputStoreOps.at(depthName))
            .setStencilLoadOp(
                renderpass.outputStencilLoadOps.at(depthName))
            .setStencilStoreOp(
                renderpass.outputStencilStoreOps.at(depthName))
            .setInitialLayout(
                (renderpass.outputLoadOps.at(depthName) ==
                     vk::AttachmentLoadOp::eLoad ||
                 renderpass.outputStencilLoadOps.at(depthName) ==
                     vk::AttachmentLoadOp::eLoad)
                    ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                    : vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
        
        attachmentDescriptions.push_back(depthStencilAttachmentDescription);

        depthAttachmentReference.setAttachment(attachmentDescriptions.size() - 1);
        depthAttachmentReference.setLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
        vk::ImageAspectFlags depthAspect =
            vk::ImageAspectFlagBits::eDepth;
        if (CommonFunction::HasStencilComponent(depthFormat))
        {
            depthAspect |= vk::ImageAspectFlagBits::eStencil;
        }
        depthAttachmentReference.setAspectMask(depthAspect);

        if (bUseMsaa)
        {
            vk::AttachmentDescription2 depthResolveAttachmentDescription;
            depthResolveAttachmentDescription
                .setFormat(resourcesResolve[depthName][0].format)
                .setSamples(vk::SampleCountFlagBits::e1)
                .setLoadOp(vk::AttachmentLoadOp::eDontCare)
                .setStoreOp(renderpass.outputStoreOps.at(depthName))
                .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
                .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
                .setInitialLayout(vk::ImageLayout::eUndefined)
                .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
            attachmentDescriptions.push_back(depthResolveAttachmentDescription);

            depthResolveAttachmentReference.setAttachment(attachmentDescriptions.size() - 1);
            depthResolveAttachmentReference.setLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
            depthResolveAttachmentReference.setAspectMask(vk::ImageAspectFlagBits::eDepth);
            bHasDepthResolve = true;
        }
    }

        // subpass
    vk::SubpassDescription2 subpassDescription;
    subpassDescription.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics);
    if(!colorAttachmentReferences.empty())
    {
        subpassDescription.setColorAttachments(colorAttachmentReferences);
    }
    if(!resolveAttachmentReferences.empty())
    {
        subpassDescription.setResolveAttachments(resolveAttachmentReferences);
    }
    if(bHasDepth)
    {
        subpassDescription.setPDepthStencilAttachment(&depthAttachmentReference);
    }

    vk::SubpassDescriptionDepthStencilResolve depthStencilResolve;
    if (bHasDepthResolve)
    {
        depthStencilResolve
            .setDepthResolveMode(vk::ResolveModeFlagBits::eSampleZero)
            .setStencilResolveMode(vk::ResolveModeFlagBits::eNone)
            .setPDepthStencilResolveAttachment(&depthResolveAttachmentReference);
        subpassDescription.setPNext(&depthStencilResolve);
    }

        // subpass dependency
    vk::SubpassDependency2 subpassDependency;
    subpassDependency
        .setSrcSubpass(0)
        .setDstSubpass(0)
        .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests)
        .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite)
        .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests)
        .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite)
        .setDependencyFlags(vk::DependencyFlagBits::eByRegion);

    std::vector<vk::SubpassDependency2> dependencies{ subpassDependency };
    if (renderpass.outputLoadOps.find("swapChain") != renderpass.outputLoadOps.end())
    {
        // 游戏/Viewport 使用相同依赖，不能因重定向改变 render-pass 兼容性。
        // LOAD 保留前一 writer 内容；首个 LOAD 不能读取未初始化目标，跨帧
        // ShaderRead -> ColorAttachment 的布局转换仍由 RT 调用方负责。
        vk::SubpassDependency2 viewportDependency;
        viewportDependency
            .setSrcSubpass(VK_SUBPASS_EXTERNAL)
            .setDstSubpass(0)
            .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput |
                vk::PipelineStageFlagBits::eFragmentShader)
            .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput)
            .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite |
                vk::AccessFlagBits::eShaderRead)
            .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentRead |
                vk::AccessFlagBits::eColorAttachmentWrite)
            .setDependencyFlags(vk::DependencyFlagBits::eByRegion);
        dependencies.push_back(viewportDependency);
    }
    vk::RenderPassCreateInfo2 renderPassCreateInfo;
    renderPassCreateInfo
        .setAttachments(attachmentDescriptions)
        .setSubpasses(subpassDescription)
        .setDependencies(dependencies);

    // 构建 RenderPass 兼容性键，用于在 PipelineFactory 中跨兼容的 render pass 复用 graphics pipeline。
    // 仅记录 Vulkan render-pass 兼容性规则关心的字段（format / samples / attachment 索引），
    // load/store 操作和 image layout 不影响兼容性，因此不纳入。
    RenderPassCompatibilityKey compatibilityKey;
    compatibilityKey.attachments.reserve(attachmentDescriptions.size());
    for (const vk::AttachmentDescription2& attachment : attachmentDescriptions)
    {
        compatibilityKey.attachments.push_back({attachment.format, attachment.samples});
    }
    compatibilityKey.colorAttachments.reserve(colorAttachmentReferences.size());
    for (const vk::AttachmentReference2& attachment : colorAttachmentReferences)
    {
        compatibilityKey.colorAttachments.push_back(attachment.attachment);
    }
    compatibilityKey.resolveAttachments.reserve(resolveAttachmentReferences.size());
    for (const vk::AttachmentReference2& attachment : resolveAttachmentReferences)
    {
        compatibilityKey.resolveAttachments.push_back(attachment.attachment);
    }
    compatibilityKey.hasDepthAttachment = bHasDepth;
    if (bHasDepth)
    {
        compatibilityKey.depthAttachment = depthAttachmentReference.attachment;
    }
    compatibilityKey.hasDepthResolveAttachment = bHasDepthResolve;
    if (bHasDepthResolve)
    {
        compatibilityKey.depthResolveAttachment = depthResolveAttachmentReference.attachment;
    }
    if (!renderToViewport)
    {
        renderpass.pipelineContractKey.renderPassCompatibilityKey = std::move(compatibilityKey);
    }
    else if (!(renderpass.pipelineContractKey.renderPassCompatibilityKey == compatibilityKey))
    {
        throw std::runtime_error(
            "Scene viewport render pass is not pipeline-compatible: " + renderpass.name);
    }

    return rendererBackend.CreateRenderPass(renderPassCreateInfo,
        renderpass.name + (renderToViewport ? ":SceneViewport" : ""));
}

std::vector<vk::Framebuffer> RenderGraph::CreateVkFrameBuffers(
    const Renderpass& renderpass,
    const std::vector<std::string>& outputResources,
    VL::RendererBackendVulkan& rendererBackend,
    bool bUseMsaa,
    bool renderToViewport)
{
    uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    
    // 总览：attachments 顺序需与 RenderPass 保持一致
    // 1) (可选) MSAA 颜色视图
    // 2) resolve/非MSAA 颜色视图
    // 3) 深度视图
    // 确定framebuffer数量，含swapchain，数量需要等于swapChainImageCount，否者为1
    uint32_t framebufferSize = swapChainImageCount;

    std::vector<vk::Framebuffer> framebuffers;
    framebuffers.reserve(framebufferSize);

    try
    {
        for (uint32_t i = 0;
             i < framebufferSize;
             i++)
        {
            MaybeFailFramebufferCreation();
            std::vector<vk::ImageView> attachments;
        // 1. MSAA view (Only if MSAA enabled)
        if (bUseMsaa)
        {
            for (const auto& outputResource : outputResources)
            {
                if (IsDepthResource(outputResource)) continue;
                const uint32_t layer = renderpass.outputLayers.at(outputResource);
                attachments.push_back(resourcesMsaa[outputResource][i].GetFramebufferAttachmentView(layer));
            }
        }

        // 2. Resolve view (or Color view if MSAA disabled)
        for (const auto& outputResource : outputResources)
        {
            if (IsDepthResource(outputResource)) continue;
            
            if (outputResource == "swapChain")
            {
                attachments.push_back(renderToViewport
                    ? sceneViewportOutputs.at(i).GetFramebufferAttachmentView()
                    : rendererBackend.GetSwapchainImageViews()[i]);
            }
            else{
                const uint32_t layer = renderpass.outputLayers.at(outputResource);
                attachments.push_back(resourcesResolve[outputResource][i].GetFramebufferAttachmentView(layer));
            }
        }

        // 3. Depth view (Only if present in output)
        std::string depthName = "";
        for(const auto& r : outputResources)
        {
            if (IsDepthResource(r))
            {
                depthName = r;
                break;
            }
        }

        if(!depthName.empty())
        {
            if(bUseMsaa) 
            {
                const uint32_t layer = renderpass.outputLayers.at(depthName);
                attachments.push_back(resourcesMsaa[depthName][i].GetFramebufferAttachmentView(layer));
                attachments.push_back(resourcesResolve[depthName][i].GetFramebufferAttachmentView(layer));
            }
            else {
                const uint32_t layer = renderpass.outputLayers.at(depthName);
                attachments.push_back(resourcesResolve[depthName][i].GetFramebufferAttachmentView(layer));
            }
        }

        vk::FramebufferCreateInfo framebufferCreateInfo;
        framebufferCreateInfo
            .setRenderPass(renderToViewport
                ? renderpass.sceneViewportRenderPass : renderpass.renderPass)
            .setAttachments(attachments)
            .setWidth(renderpass.width)
            .setHeight(renderpass.height)
            .setLayers(1);
        
            framebuffers.push_back(
                rendererBackend.CreateFramebuffer(
                    framebufferCreateInfo,
                    "Framebuffer: " +
                        renderpass.name +
                        (renderToViewport ? ":SceneViewport" : "") +
                        " (SwapchainIndex " +
                        std::to_string(i) +
                        ")"));
        }
        return framebuffers;
    }
    catch (...)
    {
        rendererBackend.DestroyFramebuffers(
            framebuffers);
        throw;
    }
}

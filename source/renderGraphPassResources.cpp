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

namespace
{

static_assert(std::is_nothrow_move_assignable_v<Renderpass>);
static_assert(std::is_nothrow_move_constructible_v<RenderResource>);

vk::CompareOp GetDepthCompareOp(VL::CompiledDepthCompareOp compareOp)
{
    switch (compareOp)
    {
    case VL::CompiledDepthCompareOp::Less:
        return vk::CompareOp::eLess;
    case VL::CompiledDepthCompareOp::LessOrEqual:
        return vk::CompareOp::eLessOrEqual;
    case VL::CompiledDepthCompareOp::Equal:
        return vk::CompareOp::eEqual;
    case VL::CompiledDepthCompareOp::Greater:
        return vk::CompareOp::eGreater;
    case VL::CompiledDepthCompareOp::GreaterOrEqual:
        return vk::CompareOp::eGreaterOrEqual;
    case VL::CompiledDepthCompareOp::Always:
        return vk::CompareOp::eAlways;
    }
    throw std::runtime_error("Compiled render graph contains an unknown depthCompareOp");
}

vk::CompareOp GetStencilCompareOp(VL::CompiledStencilCompareOp compareOp)
{
    switch (compareOp)
    {
    case VL::CompiledStencilCompareOp::Never:
        return vk::CompareOp::eNever;
    case VL::CompiledStencilCompareOp::Less:
        return vk::CompareOp::eLess;
    case VL::CompiledStencilCompareOp::Equal:
        return vk::CompareOp::eEqual;
    case VL::CompiledStencilCompareOp::LessOrEqual:
        return vk::CompareOp::eLessOrEqual;
    case VL::CompiledStencilCompareOp::Greater:
        return vk::CompareOp::eGreater;
    case VL::CompiledStencilCompareOp::NotEqual:
        return vk::CompareOp::eNotEqual;
    case VL::CompiledStencilCompareOp::GreaterOrEqual:
        return vk::CompareOp::eGreaterOrEqual;
    case VL::CompiledStencilCompareOp::Always:
        return vk::CompareOp::eAlways;
    }
    throw std::runtime_error(
        "Compiled render graph contains an unknown stencilCompareOp");
}

vk::StencilOp GetStencilOp(VL::CompiledStencilOp stencilOp)
{
    switch (stencilOp)
    {
    case VL::CompiledStencilOp::Keep:
        return vk::StencilOp::eKeep;
    case VL::CompiledStencilOp::Zero:
        return vk::StencilOp::eZero;
    case VL::CompiledStencilOp::Replace:
        return vk::StencilOp::eReplace;
    case VL::CompiledStencilOp::IncrementAndClamp:
        return vk::StencilOp::eIncrementAndClamp;
    case VL::CompiledStencilOp::DecrementAndClamp:
        return vk::StencilOp::eDecrementAndClamp;
    case VL::CompiledStencilOp::Invert:
        return vk::StencilOp::eInvert;
    case VL::CompiledStencilOp::IncrementAndWrap:
        return vk::StencilOp::eIncrementAndWrap;
    case VL::CompiledStencilOp::DecrementAndWrap:
        return vk::StencilOp::eDecrementAndWrap;
    }
    throw std::runtime_error(
        "Compiled render graph contains an unknown stencil operation");
}

bool HasRenderResourceHandles(const RenderResource& resource)
{
    return resource.imageHandle.IsValid() ||
        resource.imageViewHandle.IsValid() ||
        !resource.imageViewHandles.empty() ||
        resource.samplerHandle.IsValid() ||
        resource.image ||
        resource.memory ||
        resource.imageView ||
        !resource.imageViews.empty() ||
        resource.sampler;
}

void ClearRenderResourceFields(RenderResource& resource)
{
    resource.imageHandle = VL::RHIImageHandle();
    resource.image = nullptr;
    resource.memory = nullptr;
    resource.imageViewHandle = VL::RHIImageViewHandle();
    resource.imageView = nullptr;
    resource.imageViews.clear();
    resource.imageViewHandles.clear();
    resource.samplerHandle = VL::RHISamplerHandle();
    resource.sampler = nullptr;
}

void DestroyImageViews(
    RenderResource& resource,
    VL::RendererBackendVulkan& rendererBackend)
{
    if (resource.imageView)
    {
        rendererBackend.DestroyImageView(resource.imageView);
        resource.imageView = nullptr;
        resource.imageViewHandle = VL::RHIImageViewHandle();
    }
    for (vk::ImageView& imageView : resource.imageViews)
    {
        rendererBackend.DestroyImageView(imageView);
    }
    resource.imageViews.clear();
    resource.imageViewHandles.clear();
}

RenderResource TakeRenderResource(RenderResource& resource)
{
    RenderResource taken = resource;
    ClearRenderResourceFields(resource);
    return taken;
}

struct RetiredRenderGraphImageResource
{
    VL::RendererBackendVulkan* rendererBackend = nullptr;
    RenderResource resource;

    ~RetiredRenderGraphImageResource()
    {
        if (rendererBackend == nullptr)
        {
            return;
        }

        if (resource.imageHandle.IsValid() ||
            resource.imageViewHandle.IsValid() ||
            resource.samplerHandle.IsValid())
        {
            DestroyImageViews(resource, *rendererBackend);
            rendererBackend->DestroyImageResource(
                resource.imageHandle,
                resource.imageViewHandle,
                resource.samplerHandle);
            ClearRenderResourceFields(resource);
            return;
        }

        DestroyImageViews(resource, *rendererBackend);
        rendererBackend->DestroyImageResource(
            resource.image,
            resource.memory,
            resource.imageView,
            resource.sampler);
    }
};

struct RetiredRenderGraphPassResource
{
    VL::RendererBackendVulkan* rendererBackend = nullptr;
    std::string name;
    std::vector<VL::RHIFramebufferHandle> framebufferHandles;
    std::vector<vk::Framebuffer> framebuffers;
    VL::RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle;
    vk::DescriptorSetLayout descriptorSetLayout;
    VL::RHIDescriptorSetLayoutHandle emptyDescriptorSetLayoutHandle;
    vk::DescriptorSetLayout emptyDescriptorSetLayout;
    VL::RHIDescriptorPoolHandle descriptorPoolHandle;
    vk::DescriptorPool descriptorPool;
    VL::RHIRenderPassHandle renderPassHandle;
    vk::RenderPass renderPass;
    std::vector<VL::RHIFramebufferHandle> sceneViewportFramebufferHandles;
    std::vector<vk::Framebuffer> sceneViewportFramebuffers;
    VL::RHIRenderPassHandle sceneViewportRenderPassHandle;
    vk::RenderPass sceneViewportRenderPass;

    ~RetiredRenderGraphPassResource()
    {
        if (rendererBackend == nullptr)
        {
            return;
        }

        if (!framebufferHandles.empty())
        {
            rendererBackend->DestroyFramebuffers(framebufferHandles);
            framebuffers.clear();
        }
        else
        {
            rendererBackend->DestroyFramebuffers(framebuffers);
        }
        if (!sceneViewportFramebufferHandles.empty())
        {
            rendererBackend->DestroyFramebuffers(sceneViewportFramebufferHandles);
            sceneViewportFramebuffers.clear();
        }
        else
        {
            rendererBackend->DestroyFramebuffers(sceneViewportFramebuffers);
        }

        if (descriptorSetLayoutHandle.IsValid())
        {
            rendererBackend->DestroyDescriptorSetLayout(descriptorSetLayoutHandle);
            descriptorSetLayout = nullptr;
        }
        else
        {
            rendererBackend->DestroyDescriptorSetLayout(descriptorSetLayout);
        }

        if (emptyDescriptorSetLayoutHandle.IsValid())
        {
            rendererBackend->DestroyDescriptorSetLayout(emptyDescriptorSetLayoutHandle);
            emptyDescriptorSetLayout = nullptr;
        }
        else
        {
            rendererBackend->DestroyDescriptorSetLayout(emptyDescriptorSetLayout);
        }

        if (descriptorPoolHandle.IsValid())
        {
            rendererBackend->DestroyDescriptorPool(descriptorPoolHandle);
            descriptorPool = nullptr;
        }
        else
        {
            rendererBackend->DestroyDescriptorPool(descriptorPool);
        }

        if (renderPassHandle.IsValid())
        {
            rendererBackend->DestroyRenderPass(renderPassHandle);
            renderPass = nullptr;
        }
        else
        {
            rendererBackend->DestroyRenderPass(renderPass);
        }
        if (sceneViewportRenderPassHandle.IsValid())
        {
            rendererBackend->DestroyRenderPass(sceneViewportRenderPassHandle);
            sceneViewportRenderPass = nullptr;
        }
        else
        {
            rendererBackend->DestroyRenderPass(sceneViewportRenderPass);
        }
    }
};

bool HasRenderpassHandles(const Renderpass& renderpass)
{
    return renderpass.renderPassHandle.IsValid() ||
        renderpass.sceneViewportRenderPassHandle.IsValid() ||
        !renderpass.sceneViewportFramebufferHandles.empty() ||
        !renderpass.sceneViewportFramebuffers.empty() ||
        renderpass.sceneViewportRenderPass ||
        !renderpass.framebufferHandles.empty() ||
        renderpass.descriptorSetLayoutHandle.IsValid() ||
        renderpass.emptyDescriptorSetLayoutHandle.IsValid() ||
        renderpass.descriptorPoolHandle.IsValid() ||
        !renderpass.framebuffers.empty() ||
        renderpass.descriptorSetLayout ||
        renderpass.emptyDescriptorSetLayout ||
        renderpass.descriptorPool ||
        renderpass.renderPass;
}

void ClearRenderpassResourceFields(Renderpass& renderpass)
{
    renderpass.renderPassHandle = VL::RHIRenderPassHandle();
    renderpass.renderPass = nullptr;
    renderpass.framebufferHandles.clear();
    renderpass.framebuffers.clear();
    renderpass.sceneViewportRenderPassHandle = VL::RHIRenderPassHandle();
    renderpass.sceneViewportRenderPass = nullptr;
    renderpass.sceneViewportFramebufferHandles.clear();
    renderpass.sceneViewportFramebuffers.clear();
    renderpass.descriptorSetLayoutHandle = VL::RHIDescriptorSetLayoutHandle();
    renderpass.descriptorSetLayout = nullptr;
    renderpass.emptyDescriptorSetLayoutHandle = VL::RHIDescriptorSetLayoutHandle();
    renderpass.emptyDescriptorSetLayout = nullptr;
    renderpass.descriptorPoolHandle = VL::RHIDescriptorPoolHandle();
    renderpass.descriptorPool = nullptr;
    renderpass.descriptorSetHandles.clear();
    renderpass.descriptorSets.clear();
    renderpass.writeDescriptorSets.clear();
    renderpass.inputDescriptorImageInfos.clear();
}

std::shared_ptr<RetiredRenderGraphPassResource> TakeRenderpassResource(
    Renderpass& renderpass,
    VL::RendererBackendVulkan& rendererBackend)
{
    auto retiredPass = std::make_shared<RetiredRenderGraphPassResource>();
    retiredPass->rendererBackend = &rendererBackend;
    retiredPass->name = renderpass.name;
    retiredPass->framebufferHandles = std::move(renderpass.framebufferHandles);
    retiredPass->framebuffers = std::move(renderpass.framebuffers);
    retiredPass->descriptorSetLayoutHandle = renderpass.descriptorSetLayoutHandle;
    retiredPass->descriptorSetLayout = renderpass.descriptorSetLayout;
    retiredPass->emptyDescriptorSetLayoutHandle = renderpass.emptyDescriptorSetLayoutHandle;
    retiredPass->emptyDescriptorSetLayout = renderpass.emptyDescriptorSetLayout;
    retiredPass->descriptorPoolHandle = renderpass.descriptorPoolHandle;
    retiredPass->descriptorPool = renderpass.descriptorPool;
    retiredPass->renderPassHandle = renderpass.renderPassHandle;
    retiredPass->renderPass = renderpass.renderPass;
    retiredPass->sceneViewportFramebufferHandles =
        std::move(renderpass.sceneViewportFramebufferHandles);
    retiredPass->sceneViewportFramebuffers =
        std::move(renderpass.sceneViewportFramebuffers);
    retiredPass->sceneViewportRenderPassHandle = renderpass.sceneViewportRenderPassHandle;
    retiredPass->sceneViewportRenderPass = renderpass.sceneViewportRenderPass;

    ClearRenderpassResourceFields(renderpass);
    return retiredPass;
}

uint64_t GetGraphResourceLastUsedEpoch()
{
    return VL::ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch();
}

} // namespace

RenderResource RenderGraph::CreateRenderResource(
    const VL::CompiledRenderGraphResource& resourceDesc,
    VL::RendererBackendVulkan& rendererBackend,
    bool bIsMsaaSource,
    vk::Format formatOverride)
{
    MaybeFailResourceCreation();
    RenderResource resource;
    try
    {
        resource.name = resourceDesc.name;
        resource.type = resourceDesc.type;
        resource.arrayLayers =
            resourceDesc.arrayLayers;
        resource.clearColor = resourceDesc.clearColor;
        resource.format = formatOverride == vk::Format::eUndefined
            ? GetFormat(resourceDesc.format) : formatOverride;

        bool bIsDepthFormat =
            CommonFunction::IsDepthFormat(
                resource.format);
        const vk::Extent2D swapchainExtent =
            rendererBackend.GetSwapchainExtent();
        const float baseWidth =
            static_cast<float>(
                swapchainExtent.width);
        const float baseHeight =
            static_cast<float>(
                swapchainExtent.height);
        resource.width = static_cast<uint32_t>(
            resourceDesc.hasFixedWidth
                ? resourceDesc.widthValue
                : std::max(
                      1.0f,
                      baseWidth *
                          resourceDesc.widthValue));
        resource.height = static_cast<uint32_t>(
            resourceDesc.hasFixedHeight
                ? resourceDesc.heightValue
                : std::max(
                      1.0f,
                      baseHeight *
                          resourceDesc.heightValue));

        vk::ImageUsageFlags usage =
            GetImageUsage(resourceDesc.usage);
        vk::MemoryPropertyFlags memoryPropertyFlags =
            vk::MemoryPropertyFlagBits::eDeviceLocal;
        const vk::SampleCountFlagBits sampleCount =
            bIsMsaaSource
                ? CommonFunction::GetMsaaSampleCount()
                : vk::SampleCountFlagBits::e1;
        vk::ImageCreateInfo imageCreateInfo;
        imageCreateInfo
            .setImageType(vk::ImageType::e2D)
            .setExtent(vk::Extent3D(
                resource.width,
                resource.height,
                1))
            .setMipLevels(1)
            .setArrayLayers(resource.arrayLayers)
            .setSamples(sampleCount)
            .setFormat(resource.format)
            .setTiling(vk::ImageTiling::eOptimal)
            .setUsage(usage)
            .setSharingMode(
                vk::SharingMode::eExclusive)
            .setInitialLayout(
                vk::ImageLayout::eUndefined);
        std::tie(resource.image, resource.memory) =
            rendererBackend.CreateImage(
                imageCreateInfo,
                memoryPropertyFlags,
                "Image: " + resource.name);
        if (resourceDesc.initializeShaderRead)
        {
            // 只初始化布局，不表示像素有效。曝光 CS 每帧写完输出才允许下游采样；
            // 真正的跨帧历史在 owner 的 float32 buffer，不采样该纹理的旧像素。
            rendererBackend.TransitionImageLayout(
                resource.image,
                1,
                resource.format,
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                0,
                resource.arrayLayers);
        }
        resource.imageHandle =
            rendererBackend.GetImageHandle(
                resource.image);
        // shader 采样视图只取 depth aspect；带 stencil 的 framebuffer
        // attachment 视图必须同时包含 stencil，否则 stencil test 写入
        // 不会真正作用到该资源。
        const vk::ImageAspectFlagBits shaderAspect = bIsDepthFormat
            ? vk::ImageAspectFlagBits::eDepth
            : vk::ImageAspectFlagBits::eColor;
        // 现有 backend 接口用单个 ImageAspectFlagBits 接收 aspect；combined
        // depth-stencil 需要位掩码，Vulkan-Hpp 的 Flags 构造会保留该位组合。
        const vk::ImageAspectFlagBits attachmentAspect =
            bIsDepthFormat &&
                CommonFunction::HasStencilComponent(resource.format)
            ? static_cast<vk::ImageAspectFlagBits>(
                  static_cast<uint32_t>(
                      vk::ImageAspectFlagBits::eDepth) |
                  static_cast<uint32_t>(
                      vk::ImageAspectFlagBits::eStencil))
            : shaderAspect;
        const vk::ImageViewType imageViewType =
            resource.type == "texture2DArray"
                ? vk::ImageViewType::e2DArray
                : vk::ImageViewType::e2D;
        resource.imageView =
            rendererBackend.CreateImageView(
                resource.image,
                imageViewType,
                resource.format,
                shaderAspect,
                0,
                1,
                0,
                resource.arrayLayers,
                resource.name + "_View");
        resource.imageViewHandle =
            rendererBackend.GetImageViewHandle(
                resource.imageView);
        resource.imageViews.reserve(
            resource.arrayLayers);
        resource.imageViewHandles.reserve(
            resource.arrayLayers);
        for (uint32_t layer = 0;
             layer < resource.arrayLayers;
             ++layer)
        {
            vk::ImageView layerImageView =
                rendererBackend.CreateImageView(
            resource.image,
            vk::ImageViewType::e2D,
            resource.format,
            attachmentAspect,
            0,
            1,
            layer,
            1,
            resource.name + "_Layer" + std::to_string(layer) + "_View");
            resource.imageViews.push_back(
                layerImageView);
            resource.imageViewHandles.push_back(
                rendererBackend.GetImageViewHandle(
                    layerImageView));
        }

        resource.sampler = bIsDepthFormat
            ? ((resource.name == "shadowMap" ||
                resource.name == "localShadowMap")
                ? rendererBackend
                      .CreateDepthCompareSampler(
                          "Sampler: " +
                          resource.name)
                : rendererBackend
                      .CreateDepthSampler(
                          "Sampler: " +
                          resource.name))
            : rendererBackend.Create2DSampler(
                  "Sampler: " +
                  resource.name);
        resource.samplerHandle =
            rendererBackend.GetSamplerHandle(
                resource.sampler);
        return resource;
    }
    catch (...)
    {
        DestroyRenderResource(
            resource,
            rendererBackend,
            VL::RenderGraphReleaseMode::Immediate);
        throw;
    }
}

void RenderGraph::DestroyRenderResource(
    RenderResource& resource,
    VL::RendererBackendVulkan& rendererBackend,
    VL::RenderGraphReleaseMode releaseMode)
{
    if (!HasRenderResourceHandles(resource))
    {
        return;
    }

    if (releaseMode == VL::RenderGraphReleaseMode::Retire)
    {
        auto retiredResource = std::make_shared<RetiredRenderGraphImageResource>();
        retiredResource->rendererBackend = &rendererBackend;
        retiredResource->resource = TakeRenderResource(resource);
        // Keep the label before moving the payload; function argument
        // evaluation order must not decide whether debug metadata is readable.
        const std::string debugName =
            "RenderGraphResource:" + retiredResource->resource.name;
        VL::ResourceRetireQueue::GetInstance().RetireShared(
            debugName,
            0,
            GetGraphResourceLastUsedEpoch(),
            std::move(retiredResource));
        return;
    }

    if (resource.imageHandle.IsValid() ||
        resource.imageViewHandle.IsValid() ||
        resource.samplerHandle.IsValid())
    {
        DestroyImageViews(resource, rendererBackend);
        rendererBackend.DestroyImageResource(
            resource.imageHandle,
            resource.imageViewHandle,
            resource.samplerHandle);
        ClearRenderResourceFields(resource);
        return;
    }

    DestroyImageViews(resource, rendererBackend);
    rendererBackend.DestroyImageResource(
        resource.image,
        resource.memory,
        resource.imageView,
        resource.sampler);
}

Renderpass RenderGraph::CreateRenderpass(
    const VL::CompiledRenderGraphPass& passDesc,
    VL::RendererBackendVulkan& rendererBackend)
{
    Renderpass renderpass;
    try
    {
        renderpass.name = passDesc.name;
        renderpass.type = passDesc.type;
        renderpass.needsMaterial = passDesc.needCreateMaterial;
        renderpass.materialInstancePath = passDesc.materialInstancePath;
        const bool bIsShadowPass =
            renderpass.type ==
            VL::RenderGraphPassType::Shadow;
        const bool bIsLocalShadowPass =
            renderpass.type ==
            VL::RenderGraphPassType::LocalShadow;
        const bool bUseMsaa = passDesc.needMsaa;
        renderpass.pipelineContractKey.useVertexInput =
            passDesc.pipelineState.useVertexInput;
        renderpass.pipelineContractKey.depthTestEnable =
            passDesc.pipelineState.depthTestEnable;
        renderpass.pipelineContractKey.depthWriteEnable =
            passDesc.pipelineState.depthWriteEnable;
        renderpass.pipelineContractKey.depthCompareOp =
            GetDepthCompareOp(passDesc.pipelineState.depthCompareOp);
        renderpass.pipelineContractKey.stencilTestEnable =
            passDesc.pipelineState.stencilTestEnable;
        renderpass.pipelineContractKey.stencilWriteMask =
            passDesc.pipelineState.stencilWriteMask;
        renderpass.pipelineContractKey.stencilCompareMask =
            passDesc.pipelineState.stencilCompareMask;
        renderpass.pipelineContractKey.stencilReference =
            passDesc.pipelineState.stencilReference;
        renderpass.pipelineContractKey.stencilCompareOp =
            GetStencilCompareOp(
                passDesc.pipelineState.stencilCompareOp);
        renderpass.pipelineContractKey.stencilFailOp =
            GetStencilOp(passDesc.pipelineState.stencilFailOp);
        renderpass.pipelineContractKey.stencilPassOp =
            GetStencilOp(passDesc.pipelineState.stencilPassOp);
        renderpass.pipelineContractKey.stencilDepthFailOp =
            GetStencilOp(
                passDesc.pipelineState.stencilDepthFailOp);
        renderpass.pipelineContractKey.isShadowPass =
            bIsShadowPass || bIsLocalShadowPass;

        renderpass.inputResources = passDesc.inputResources;
        renderpass.inputDescriptorPlan = passDesc.inputDescriptors;
        for (const VL::CompiledRenderGraphPassOutput& output :
             passDesc.outputResources)
        {
            const std::string& resourceName = output.resource;
            renderpass.outputResources.push_back(resourceName);
            renderpass.outputLayers[resourceName] = output.layer;
            renderpass.outputLoadOps[resourceName] =
                GetAttachmentLoadOp(output.loadOp);
            renderpass.outputStoreOps[resourceName] =
                GetAttachmentStoreOp(output.storeOp);
            renderpass.outputStencilLoadOps[resourceName] =
                GetAttachmentLoadOp(output.stencilLoadOp);
            renderpass.outputStencilStoreOps[resourceName] =
                GetAttachmentStoreOp(output.stencilStoreOp);
            if (bIsShadowPass)
            {
                renderpass.shadowCascadeIndex = output.layer;
            }
            if (bIsLocalShadowPass)
            {
                renderpass.localShadowIndex = output.layer;
            }
        }
        // 当前游戏图没有 swapChain 的 MSAA 源附件，不能把单采样 Viewport
        // 误绑为多采样附件。保持现有支持边界，并在任何 Vulkan 创建前报错。
        if (passDesc.outputsSwapchain && bUseMsaa)
        {
            throw std::runtime_error(
                "Swapchain-output passes require single-sample attachments: " + renderpass.name);
        }

        // 专用曝光计算节点不创建 VkRenderPass/framebuffer 或 Material descriptors。
        if (renderpass.type == VL::RenderGraphPassType::AutoExposure)
        {
            renderpass.width = 1;
            renderpass.height = 1;
            return renderpass;
        }
        // 体积雾注入是 compute 节点：输出只有 storage usage，不能生成
        // framebuffer；尺寸直接取雾 texture2DArray 的分辨率。
        if (renderpass.type == VL::RenderGraphPassType::VolumetricFog)
        {
            const auto& fogResources =
                resourcesResolve.at(renderpass.outputResources.at(0));
            renderpass.width = fogResources.at(0).width;
            renderpass.height = fogResources.at(0).height;
            return renderpass;
        }
        if (renderpass.type == VL::RenderGraphPassType::ParticleSoftDepth)
        {
            const auto& softDepthResources =
                resourcesResolve.at(renderpass.outputResources.at(0));
            renderpass.width = softDepthResources.at(0).width;
            renderpass.height = softDepthResources.at(0).height;
            return renderpass;
        }
        // tile 光照聚类是纯 compute 节点：不创建 VkRenderPass/framebuffer，
        // 尺寸取主颜色目标，保证与光照 shader 的 gl_FragCoord 一致。
        if (renderpass.type == VL::RenderGraphPassType::LightCulling)
        {
            const auto colorIt = resourcesResolve.find("sceneColor");
            if (colorIt == resourcesResolve.end() ||
                colorIt->second.empty())
            {
                throw std::runtime_error(
                    "LightCulling pass requires a sceneColor render target: " +
                    renderpass.name);
            }
            renderpass.width = colorIt->second[0].width;
            renderpass.height = colorIt->second[0].height;
            return renderpass;
        }
        MaybeFailRenderPassCreation();
        renderpass.renderPass =
            CreateVkRenderPass(
                renderpass,
                rendererBackend,
                bUseMsaa);
        renderpass.renderPassHandle =
            rendererBackend.GetRenderPassHandle(
                renderpass.renderPass);

        if (passDesc.outputsSwapchain)
        {
            const auto extent =
                rendererBackend.GetSwapchainExtent();
            renderpass.width = extent.width;
            renderpass.height = extent.height;
        }
        else if (bIsShadowPass)
        {
            renderpass.width =
                resourcesResolve.at("shadowMap")[0].width;
            renderpass.height =
                resourcesResolve.at("shadowMap")[0].height;
        }
        else if (bIsLocalShadowPass)
        {
            renderpass.width =
                resourcesResolve.at("localShadowMap")[0].width;
            renderpass.height =
                resourcesResolve.at("localShadowMap")[0].height;
        }
        else
        {
            const std::vector<RenderResource>& resources =
                resourcesMsaa.at(
                    renderpass.outputResources[0]);
            renderpass.width = resources[0].width;
            renderpass.height = resources[0].height;
        }

        renderpass.clearValues =
            GetClearValues(
                renderpass.outputResources,
                bUseMsaa);
        renderpass.framebuffers =
            CreateVkFrameBuffers(
                renderpass,
                renderpass.outputResources,
                rendererBackend,
                bUseMsaa);

        std::vector<VL::RHIFramebufferHandle>
            framebufferHandles;
        framebufferHandles.reserve(
            renderpass.framebuffers.size());
        for (vk::Framebuffer framebuffer :
             renderpass.framebuffers)
        {
            framebufferHandles.push_back(
                rendererBackend
                    .GetFramebufferHandle(
                        framebuffer));
        }
        renderpass.framebufferHandles =
            std::move(framebufferHandles);
        if (passDesc.outputsSwapchain)
        {
            MaybeFailRenderPassCreation();
            renderpass.sceneViewportRenderPass =
                CreateVkRenderPass(renderpass, rendererBackend, bUseMsaa, true);
            renderpass.sceneViewportRenderPassHandle =
                rendererBackend.GetRenderPassHandle(renderpass.sceneViewportRenderPass);
            renderpass.sceneViewportFramebuffers = CreateVkFrameBuffers(
                renderpass, renderpass.outputResources, rendererBackend, bUseMsaa, true);
            std::vector<VL::RHIFramebufferHandle> viewportHandles;
            viewportHandles.reserve(renderpass.sceneViewportFramebuffers.size());
            for (vk::Framebuffer framebuffer : renderpass.sceneViewportFramebuffers)
            {
                viewportHandles.push_back(rendererBackend.GetFramebufferHandle(framebuffer));
            }
            renderpass.sceneViewportFramebufferHandles = std::move(viewportHandles);
        }
        return renderpass;
    }
    catch (...)
    {
        DestroyRenderpass(
            renderpass,
            rendererBackend,
            VL::RenderGraphReleaseMode::Immediate);
        throw;
    }
}

void RenderGraph::DestroyRenderpass(
    Renderpass& renderpass,
    VL::RendererBackendVulkan& rendererBackend,
    VL::RenderGraphReleaseMode releaseMode)
{
    if (!HasRenderpassHandles(renderpass))
    {
        return;
    }

    if (releaseMode == VL::RenderGraphReleaseMode::Retire)
    {
        std::shared_ptr<RetiredRenderGraphPassResource> retiredPass =
            TakeRenderpassResource(renderpass, rendererBackend);
        // Keep the label before moving the payload; function argument
        // evaluation order must not decide whether debug metadata is readable.
        const std::string debugName = "RenderGraphPass:" + retiredPass->name;
        VL::ResourceRetireQueue::GetInstance().RetireShared(
            debugName,
            0,
            GetGraphResourceLastUsedEpoch(),
            std::move(retiredPass));
        return;
    }

    if (!renderpass.framebufferHandles.empty())
    {
        rendererBackend.DestroyFramebuffers(renderpass.framebufferHandles);
        renderpass.framebuffers.clear();
    }
    else
    {
        rendererBackend.DestroyFramebuffers(renderpass.framebuffers);
    }
    if (!renderpass.sceneViewportFramebufferHandles.empty())
    {
        rendererBackend.DestroyFramebuffers(renderpass.sceneViewportFramebufferHandles);
        renderpass.sceneViewportFramebuffers.clear();
    }
    else
    {
        rendererBackend.DestroyFramebuffers(renderpass.sceneViewportFramebuffers);
    }

    if (renderpass.descriptorSetLayoutHandle.IsValid())
    {
        rendererBackend.DestroyDescriptorSetLayout(renderpass.descriptorSetLayoutHandle);
        renderpass.descriptorSetLayout = nullptr;
    }
    else
    {
        rendererBackend.DestroyDescriptorSetLayout(renderpass.descriptorSetLayout);
    }

    if (renderpass.emptyDescriptorSetLayoutHandle.IsValid())
    {
        rendererBackend.DestroyDescriptorSetLayout(renderpass.emptyDescriptorSetLayoutHandle);
        renderpass.emptyDescriptorSetLayout = nullptr;
    }
    else
    {
        rendererBackend.DestroyDescriptorSetLayout(renderpass.emptyDescriptorSetLayout);
    }

    if (renderpass.descriptorPoolHandle.IsValid())
    {
        rendererBackend.DestroyDescriptorPool(renderpass.descriptorPoolHandle);
        renderpass.descriptorPool = nullptr;
    }
    else
    {
        rendererBackend.DestroyDescriptorPool(renderpass.descriptorPool);
    }

    if (renderpass.renderPassHandle.IsValid())
    {
        rendererBackend.DestroyRenderPass(renderpass.renderPassHandle);
        renderpass.renderPass = nullptr;
    }
    else
    {
        rendererBackend.DestroyRenderPass(renderpass.renderPass);
    }
    if (renderpass.sceneViewportRenderPassHandle.IsValid())
    {
        rendererBackend.DestroyRenderPass(renderpass.sceneViewportRenderPassHandle);
        renderpass.sceneViewportRenderPass = nullptr;
    }
    else
    {
        rendererBackend.DestroyRenderPass(renderpass.sceneViewportRenderPass);
    }

    renderpass.descriptorSetHandles.clear();
    renderpass.descriptorSets.clear();
    renderpass.writeDescriptorSets.clear();
}

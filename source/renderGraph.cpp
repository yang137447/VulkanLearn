#include "renderGraph.h"
#include "commonFunction.h"
#include "shaderReflect.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/graphicsPipeline.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorWriter.h"
#include "render/rendergraph/renderGraphCompiler.h"
#include "render/resource/resourceRetireQueue.h"
#include <algorithm>
#include <memory>
#include <stdint.h>
#include <stdexcept>
#include <utility>
#include "profiler.h"

vk::ImageView RenderResource::GetShaderDescriptorView() const
{
    return imageView;
}

vk::ImageView RenderResource::GetFramebufferAttachmentView(uint32_t layer) const
{
    if (layer >= imageViews.size())
    {
        throw std::runtime_error(
            "Render resource framebuffer attachment layer is not available: " +
            name + "[" + std::to_string(layer) + "]");
    }
    return imageViews[layer];
}

namespace
{

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
    }
};

bool HasRenderpassHandles(const Renderpass& renderpass)
{
    return renderpass.renderPassHandle.IsValid() ||
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

    ClearRenderpassResourceFields(renderpass);
    return retiredPass;
}

uint64_t GetGraphResourceLastUsedEpoch()
{
    return VL::ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch();
}

} // namespace

void Renderpass::Draw(vk::CommandBuffer& commandBuffer) const
{
        PROFILE_FUNCTION();
        vk::Viewport viewport;
        viewport
            .setX(0.0f)
            .setY(0.0f)
            .setWidth(static_cast<float>(width))
            .setHeight(static_cast<float>(height))
            .setMinDepth(0.0f)
            .setMaxDepth(1.0f);
        vk::Rect2D scissor;
        scissor
            .setOffset({ 0, 0 })
            .setExtent({ 
                static_cast<uint32_t>(width), 
                static_cast<uint32_t>(height) });
        commandBuffer.setViewport(0, 1, &viewport);
        commandBuffer.setScissor(0, 1, &scissor);
        commandBuffer.draw(3, 1, 0,0);
}

void Renderpass::CreateUniformBuffers()
{
    // Pass parameters live in MaterialInstance UBOs; Renderpass itself owns no
    // extra per-pass uniform buffer.
}
void Renderpass::SetupDescriptors(
    const RenderGraph& renderGraph,
    VL::RendererBackendVulkan& rendererBackend)
{
    uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    inputDescriptorImageInfos.clear();
    inputDescriptorImageInfos.resize(swapChainImageCount);
    // 总览：按 compiled descriptor plan 为每个输入资源建立 DescriptorImageInfo
    // 1) 先从 resolve 里找，找不到再从 msaa 里找
    // 2) shadowMap 使用深度只读 layout
    // 3) 按 binding 写入 inputDescriptorImageInfos
    for(uint32_t imageIndex = 0; imageIndex < swapChainImageCount; ++imageIndex)
    {
        for(const VL::CompiledRenderGraphPassInputDescriptor& inputDescriptor : inputDescriptorPlan)
        {
            const std::string& inputResource = inputDescriptor.resource;
            const RenderResource* resource = nullptr;
            
            const auto& resolveMap = renderGraph.GetResourcesResolve();
            auto resolveIt = resolveMap.find(inputResource);
            if(resolveIt != resolveMap.end() && imageIndex < resolveIt->second.size())
            {
                resource = &resolveIt->second[imageIndex];
            }
            else
            {
                const auto& msaaMap = renderGraph.GetResourcesMsaa();
                auto msaaIt = msaaMap.find(inputResource);
                if(msaaIt != msaaMap.end() && imageIndex < msaaIt->second.size())
                {
                    resource = &msaaIt->second[imageIndex];
                }
            }

            if(resource == nullptr)
            {
                continue;
            }

            vk::ImageLayout imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            if(CommonFunction::IsDepthFormat(resource->format))
            {
                imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
            }

            vk::DescriptorImageInfo imageInfo = vk::DescriptorImageInfo();
            imageInfo
                .setImageLayout(imageLayout)
                .setImageView(resource->GetShaderDescriptorView())
                .setSampler(resource->sampler);
            if (inputDescriptor.binding >= inputDescriptorImageInfos[imageIndex].size())
            {
                inputDescriptorImageInfos[imageIndex].resize(inputDescriptor.binding + 1);
            }
            inputDescriptorImageInfos[imageIndex][inputDescriptor.binding] = imageInfo;
        }
    }
}

void Renderpass::CreatePassDescriptorSetLayout(VL::RendererBackendVulkan& rendererBackend)
{
    // 创建空的 DescriptorSetLayout，用于绑定空的 DescriptorSet, 主要用于geometrypass对齐使用，渲染时能用PassSetIndex调用到passSet
    vk::DescriptorSetLayoutCreateInfo emptyLayoutCreateInfo;
    emptyDescriptorSetLayout = rendererBackend.CreateDescriptorSetLayout(
        emptyLayoutCreateInfo,
        "DescriptorSetLayout: Empty");
    emptyDescriptorSetLayoutHandle =
        rendererBackend.GetDescriptorSetLayoutHandle(emptyDescriptorSetLayout);

    std::vector<vk::DescriptorSetLayoutBinding> descriptorSetLayoutBindings;
    if(!materialInstance.expired())
    {
        auto baseMaterial = materialInstance.lock()->GetBaseMaterial().lock();
        const auto& shaderBindings = baseMaterial->GetRenderPipeline()->GetShaderBindings();
        for(const auto& binding : shaderBindings)
        {
            if(binding.set != PassSetIndex)
            {
                continue;
            }
            vk::DescriptorSetLayoutBinding layoutBinding;
            layoutBinding
                .setBinding(binding.binding)
                .setDescriptorType(binding.type)
                .setDescriptorCount(1)
                .setStageFlags(binding.stageFlags)
                .setPImmutableSamplers(nullptr);
            descriptorSetLayoutBindings.push_back(layoutBinding);
        }
    }
    else
    {
        for(const VL::CompiledRenderGraphPassInputDescriptor& inputDescriptor : inputDescriptorPlan)
        {
            vk::DescriptorSetLayoutBinding layoutBinding;
            layoutBinding
                .setBinding(inputDescriptor.binding)
                .setDescriptorType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eFragment)
                .setPImmutableSamplers(nullptr);
            descriptorSetLayoutBindings.push_back(layoutBinding);
        }
    }

    vk::DescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo;
    descriptorSetLayoutCreateInfo
        .setBindings(descriptorSetLayoutBindings);

    descriptorSetLayout = rendererBackend.CreateDescriptorSetLayout(
        descriptorSetLayoutCreateInfo,
        "DescriptorSetLayout: " + name);
    descriptorSetLayoutHandle =
        rendererBackend.GetDescriptorSetLayoutHandle(descriptorSetLayout);
}

void Renderpass::CreateDescriptorSets(VL::RendererBackendVulkan& rendererBackend)
{
    uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    std::vector<vk::DescriptorSetLayout> allocateLayouts;
    std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
    if (!materialInstance.expired()) //geometryPass没有MaterialInstance,需要额外处理
    {
        auto baseMaterial = materialInstance.lock()->GetBaseMaterial().lock();
        const std::vector<ShaderBinding>& shaderBindings = baseMaterial->GetRenderPipeline()->GetShaderBindings();
        // std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
        for(const auto& binding : shaderBindings)
        {
            vk::DescriptorPoolSize poolSize;
            poolSize
                .setType(binding.type)
                .setDescriptorCount(swapChainImageCount);
            descriptorPoolSizes.push_back(poolSize);
        }
        const auto& pipelineSetLayouts = baseMaterial->GetRenderPipeline()->GetDescriptorSetLayouts();
        
        // 只分配前4个Set (0:Global, 1:Material, 2:Object)，Set 3 (Pass)
        // std::vector<vk::DescriptorSetLayout> allocateLayouts;
        for(size_t i = 0; i < pipelineSetLayouts.size(); ++i)
        {
            // if(i != PassSetIndex)
            {
                allocateLayouts.push_back(pipelineSetLayouts[i]);
            }
        }
    }
    else {
        uint32_t descriptorCount = swapChainImageCount * static_cast<uint32_t>(inputDescriptorPlan.size());
        if (descriptorCount > 0)
        {
            vk::DescriptorPoolSize poolSize;
            poolSize
                .setType(vk::DescriptorType::eCombinedImageSampler)
                .setDescriptorCount(descriptorCount);
            descriptorPoolSizes.push_back(poolSize);
        }
        
        // 分配空的 DescriptorSetLayout
        allocateLayouts.push_back(emptyDescriptorSetLayout);
        allocateLayouts.push_back(emptyDescriptorSetLayout);
        allocateLayouts.push_back(emptyDescriptorSetLayout);
        allocateLayouts.push_back(descriptorSetLayout); //保持passDescriptorSetLayout在最后
    }
    uint32_t SetLayoutCount = allocateLayouts.size();

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(swapChainImageCount * SetLayoutCount)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    descriptorPool = rendererBackend.CreateDescriptorPool(
        descriptorPoolCreateInfo,
        "DescriptorPool: " + name);
    descriptorPoolHandle = rendererBackend.GetDescriptorPoolHandle(descriptorPool);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(allocateLayouts);
    
    descriptorSets.resize(swapChainImageCount);
    descriptorSetHandles.resize(swapChainImageCount);
    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        descriptorSets[i].resize(SetLayoutCount);
        rendererBackend.AllocateDescriptorSets(descriptorSetAllocateInfo, descriptorSets[i]);
        descriptorSetHandles[i].resize(descriptorSets[i].size());
        for(uint32_t j = 0; j < SetLayoutCount; j++)
        {
            descriptorSetHandles[i][j] =
                rendererBackend.GetDescriptorSetHandle(descriptorSets[i][j]);
            rendererBackend.SetDescriptorSetDebugName(
                descriptorSets[i][j],
                "DescriptorSet: " + name +
                    " (SwapchainIndex " + std::to_string(i) +
                    ", Set " + std::to_string(j) + ")");
        }
    }
}

void Renderpass::UpdateDescriptorSets(
    VL::RendererBackendVulkan& rendererBackend,
    const VL::RendererDescriptorContext& descriptorContext,
    const VL::RendererPassDescriptorPlan& descriptorPlan)
{
    uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    writeDescriptorSets.resize(swapChainImageCount);

    std::shared_ptr<MaterialInstance> passMaterialInstance = materialInstance.lock();

    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        writeDescriptorSets[i].clear();
        VL::RendererDescriptorWriteInputs writeInputs;
        writeInputs.descriptorContext = &descriptorContext;
        writeInputs.materialInstance = passMaterialInstance.get();
        if (i < inputDescriptorImageInfos.size())
        {
            writeInputs.passInputImageInfos = &inputDescriptorImageInfos[i];
        }

        for(const VL::RendererDescriptorUpdate& descriptorUpdate : descriptorPlan.updates)
        {
            if (i >= descriptorSets.size() ||
                descriptorUpdate.setIndex >= descriptorSets[i].size())
            {
                continue;
            }

            vk::WriteDescriptorSet write;
            if (VL::BuildRendererDescriptorWrite(
                    descriptorUpdate,
                    descriptorSets[i][descriptorUpdate.setIndex],
                    i,
                    writeInputs,
                    write))
            {
                writeDescriptorSets[i].push_back(write);
            }
        }

        if (!writeDescriptorSets[i].empty())
        {
            rendererBackend.UpdateDescriptorSets(writeDescriptorSets[i]);
        }
    }
}

RenderGraph::~RenderGraph()
{
    // Runtime shutdown owns Vulkan resource release while the backend is still
    // valid. The destructor intentionally has no fallback device path.
}

void RenderGraph::Shutdown(
    VL::RendererBackendVulkan& rendererBackend,
    VL::RenderGraphReleaseMode releaseMode)
{
    for(auto& [name, renderpass] : renderpasses)
    {
        DestroyRenderpass(renderpass, rendererBackend, releaseMode);
    }

    for(auto& [name, resources] : resourcesMsaa)
    {
        for(auto& resource : resources)
        {
            DestroyRenderResource(resource, rendererBackend, releaseMode);
        }
    }
    for(auto& [name, resources] : resourcesResolve)
    {
        for(auto& resource : resources)
        {
            DestroyRenderResource(resource, rendererBackend, releaseMode);
        }
    }

    resourcesMsaa.clear();
    resourcesResolve.clear();
    renderpasses.clear();
    renderpassesOrdered.clear();
    descriptorPlanCache.Clear();
}

void RenderGraph::LoadRenderGraph(
    const nlohmann::json& renderGraphJson,
    VL::RendererBackendVulkan& rendererBackend)
{
    VL::RenderGraphCompiler compiler;
    auto compileResult = compiler.Compile(renderGraphJson);
    if (compileResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(compileResult.Error()));
    }
    compiledFrameGraph = std::move(compileResult.Value());
    descriptorPlanCache.Clear();

    uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    // GPU object creation still happens here; the compiler above now owns the
    // validated pass/resource order that future FrameGraph work will consume.
    for (const VL::CompiledRenderGraphResource& resourceDesc : compiledFrameGraph.resources)
    {
        std::string name = resourceDesc.name;
        if(name == "swapChain") continue;

        // 先建立msaa资源
        if (name != "shadowMap")
        {
            std::vector<RenderResource> msaaResources;
            msaaResources.reserve(swapChainImageCount);
            for(uint32_t i = 0; i < swapChainImageCount; ++i)
            {
                RenderResource resource = CreateRenderResource(resourceDesc, rendererBackend, true);
                msaaResources.push_back(std::move(resource));
            }
            resourcesMsaa.emplace(name, std::move(msaaResources));
        }

        // 再建立resolve资源
        std::vector<RenderResource> resolveResources;
        resolveResources.reserve(swapChainImageCount);
        for(uint32_t i = 0; i < swapChainImageCount; ++i)
        {
            RenderResource resourceResolve = CreateRenderResource(resourceDesc, rendererBackend);
            resolveResources.push_back(std::move(resourceResolve));
        }
        resourcesResolve.emplace(name, std::move(resolveResources));
    }
    for (const VL::CompiledRenderGraphPass& passDesc : compiledFrameGraph.passes)
    {
        std::string passName = passDesc.name;
        renderpassesOrdered.push_back(passName);
        Renderpass renderpass = CreateRenderpass(passDesc, rendererBackend);
        renderpasses[passName] = renderpass;
    }
}

void RenderGraph::RenderInitialize(
    VL::RendererBackendVulkan& rendererBackend,
    const VL::RendererDescriptorContext& descriptorContext)
{
    for(auto& [passName, renderpass] : renderpasses)
    {
        renderpass.CreateUniformBuffers();
        renderpass.SetupDescriptors(*this, rendererBackend);
        if (auto passMaterialInstance = renderpass.materialInstance.lock())
        {
            auto baseMaterial = passMaterialInstance->GetBaseMaterial().lock();
            if (baseMaterial)
            {
                for (const auto& binding : baseMaterial->GetRenderPipeline()->GetShaderBindings())
                {
                    if (binding.set == MaterialSetIndex &&
                        binding.binding == 0 &&
                        binding.type == vk::DescriptorType::eUniformBuffer)
                    {
                        passMaterialInstance->RenderInitialize(rendererBackend);
                        break;
                    }
                }
            }
        }
        descriptorPlanCache.RebuildPassPlan(
            passName,
            renderpass.inputDescriptorPlan,
            renderpass.materialInstance);
        renderpass.CreatePassDescriptorSetLayout(rendererBackend);
        renderpass.CreateDescriptorSets(rendererBackend);
        renderpass.UpdateDescriptorSets(
            rendererBackend,
            descriptorContext,
            descriptorPlanCache.GetPassPlan(passName));
    }
}

void RenderGraph::RefreshRuntimeDescriptors(
    VL::RendererBackendVulkan& rendererBackend,
    const VL::RendererDescriptorContext& descriptorContext)
{
    for (auto& [passName, renderpass] : renderpasses)
    {
        if (auto passMaterialInstance = renderpass.materialInstance.lock())
        {
            passMaterialInstance->RenderInitialize(rendererBackend);
        }
        descriptorPlanCache.RebuildPassPlan(
            passName,
            renderpass.inputDescriptorPlan,
            renderpass.materialInstance);
        renderpass.UpdateDescriptorSets(
            rendererBackend,
            descriptorContext,
            descriptorPlanCache.GetPassPlan(passName));
    }
}

std::unordered_map<std::string, std::weak_ptr<MaterialInstance>> RenderGraph::CapturePassMaterialInstances() const
{
    std::unordered_map<std::string, std::weak_ptr<MaterialInstance>> passMaterials;
    for (const auto& [passName, renderpass] : renderpasses)
    {
        passMaterials.emplace(passName, renderpass.materialInstance);
    }
    return passMaterials;
}

void RenderGraph::RestorePassMaterialInstances(
    const std::unordered_map<std::string, std::weak_ptr<MaterialInstance>>& passMaterials)
{
    for (auto& [passName, materialInstance] : passMaterials)
    {
        auto passIt = renderpasses.find(passName);
        if (passIt != renderpasses.end())
        {
            passIt->second.materialInstance = materialInstance;
        }
    }
}

RenderResource RenderGraph::CreateRenderResource(
    const VL::CompiledRenderGraphResource& resourceDesc,
    VL::RendererBackendVulkan& rendererBackend,
    bool bIsMsaaSource)
{
    RenderResource resource;

    resource.name = resourceDesc.name;
    resource.type = resourceDesc.type;
    resource.arrayLayers = resourceDesc.arrayLayers;

    resource.format = GetFormat(resourceDesc.format);

    bool bIsDepthFormat = CommonFunction::IsDepthFormat(resource.format);

    const vk::Extent2D swapchainExtent = rendererBackend.GetSwapchainExtent();
    const float baseWidth = static_cast<float>(swapchainExtent.width);
    const float baseHeight = static_cast<float>(swapchainExtent.height);
    resource.width = static_cast<uint32_t>(
        resourceDesc.hasFixedWidth
            ? resourceDesc.widthValue
            : std::max(1.0f, baseWidth * resourceDesc.widthValue));
    resource.height = static_cast<uint32_t>(
        resourceDesc.hasFixedHeight
            ? resourceDesc.heightValue
            : std::max(1.0f, baseHeight * resourceDesc.heightValue));

    vk::ImageUsageFlags usage = GetImageUsage(resourceDesc.usage);

    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    const vk::SampleCountFlagBits sampleCount =
        bIsMsaaSource ? CommonFunction::GetMsaaSampleCount() : vk::SampleCountFlagBits::e1;
    vk::ImageCreateInfo imageCreateInfo;
    imageCreateInfo
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D(resource.width, resource.height, 1))
        .setMipLevels(1)
        .setArrayLayers(resource.arrayLayers)
        .setSamples(sampleCount)
        .setFormat(resource.format)
        .setTiling(tiling)
        .setUsage(usage)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);
    std::tie(resource.image, resource.memory) = rendererBackend.CreateImage(
        imageCreateInfo,
        memoryPropertyFlags,
        "Image: " + resource.name);
    resource.imageHandle = rendererBackend.GetImageHandle(resource.image);
    vk::ImageAspectFlagBits aspect = vk::ImageAspectFlagBits::eColor;
    if(bIsDepthFormat)
    {
        aspect = vk::ImageAspectFlagBits::eDepth;
        if(CommonFunction::HasStencilComponent(resource.format))
        {
            // Depth/stencil formats are only used as depth attachments today.
            // Keep stencil out of the view until a pass explicitly declares
            // stencil load/store and matching barriers.
            // aspect |= vk::ImageAspectFlagBits::eStencil;
        }
    }
    const vk::ImageViewType imageViewType =
        resource.type == "texture2DArray" ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
    resource.imageView = rendererBackend.CreateImageView(
        resource.image,
        imageViewType,
        resource.format,
        aspect,
        0,
        1,
        0,
        resource.arrayLayers,
        resource.name + "_View");
    resource.imageViewHandle = rendererBackend.GetImageViewHandle(resource.imageView);
    resource.imageViews.reserve(resource.arrayLayers);
    resource.imageViewHandles.reserve(resource.arrayLayers);
    for (uint32_t layer = 0; layer < resource.arrayLayers; ++layer)
    {
        vk::ImageView layerImageView = rendererBackend.CreateImageView(
            resource.image,
            vk::ImageViewType::e2D,
            resource.format,
            aspect,
            0,
            1,
            layer,
            1,
            resource.name + "_Layer" + std::to_string(layer) + "_View");
        resource.imageViews.push_back(layerImageView);
        resource.imageViewHandles.push_back(
            rendererBackend.GetImageViewHandle(layerImageView));
    }

    resource.sampler = bIsDepthFormat
        ? (resource.name == "shadowMap"
            ? rendererBackend.CreateDepthCompareSampler("Sampler: " + resource.name)
            : rendererBackend.CreateDepthSampler("Sampler: " + resource.name))
        : rendererBackend.Create2DSampler("Sampler: " + resource.name);
    resource.samplerHandle = rendererBackend.GetSamplerHandle(resource.sampler);

    return resource;
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
    renderpass.name = passDesc.name;
    renderpass.type = passDesc.type;
    bool bIsShadowPass = renderpass.type == "shadow";
    bool bUseMsaa = passDesc.needMsaa;
    renderpass.sampleCount = bUseMsaa ? CommonFunction::GetMsaaSampleCount() : vk::SampleCountFlagBits::e1;

    renderpass.inputResources = passDesc.inputResources;
    renderpass.inputDescriptorPlan = passDesc.inputDescriptors;
    for (const VL::CompiledRenderGraphPassOutput& output : passDesc.outputResources)
    {
        const std::string& resourceName = output.resource;
        renderpass.outputResources.push_back(resourceName);
        renderpass.outputLayers[resourceName] = output.layer;
        renderpass.outputLoadOps[resourceName] = GetAttachmentLoadOp(output.loadOp);
        renderpass.outputStoreOps[resourceName] = GetAttachmentStoreOp(output.storeOp);
        if (bIsShadowPass)
        {
            renderpass.shadowCascadeIndex = output.layer;
        }
    }
    renderpass.colorAttachmentCount = passDesc.colorOutputCount;

    vk::RenderPass vkRenderPass = CreateVkRenderPass(renderpass, rendererBackend, bUseMsaa);
    renderpass.renderPass = vkRenderPass;
    renderpass.renderPassHandle = rendererBackend.GetRenderPassHandle(renderpass.renderPass);
    
    if (renderpass.outputResources[0] == "swapChain")
    {
        auto extent = rendererBackend.GetSwapchainExtent();
        renderpass.width = extent.width;
        renderpass.height = extent.height;
    }
    else if (bIsShadowPass)
    {
        renderpass.width = resourcesResolve["shadowMap"][0].width;
        renderpass.height = resourcesResolve["shadowMap"][0].height;
    }
    else
    {
        renderpass.width = resourcesMsaa[renderpass.outputResources[0]][0].width;
        renderpass.height = resourcesMsaa[renderpass.outputResources[0]][0].height;
    }
    
    renderpass.clearValues = GetClearValues(renderpass.outputResources, bUseMsaa);
    renderpass.framebuffers = CreateVkFrameBuffers(
        renderpass,
        renderpass.outputResources,
        rendererBackend,
        bUseMsaa);
    renderpass.framebufferHandles.reserve(renderpass.framebuffers.size());
    for (vk::Framebuffer framebuffer : renderpass.framebuffers)
    {
        renderpass.framebufferHandles.push_back(
            rendererBackend.GetFramebufferHandle(framebuffer));
    }
    
    return renderpass;
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

    renderpass.descriptorSetHandles.clear();
    renderpass.descriptorSets.clear();
    renderpass.writeDescriptorSets.clear();
}

vk::RenderPass RenderGraph::CreateVkRenderPass(
    const Renderpass& renderpass,
    VL::RendererBackendVulkan& rendererBackend,
    bool bUseMsaa)
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

        if (resourceName == "swapChain")
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

            if (resourceName == "swapChain") 
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
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setInitialLayout(renderpass.outputLoadOps.at(depthName) == vk::AttachmentLoadOp::eLoad ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
        
        attachmentDescriptions.push_back(depthStencilAttachmentDescription);

        depthAttachmentReference.setAttachment(attachmentDescriptions.size() - 1);
        depthAttachmentReference.setLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
        depthAttachmentReference.setAspectMask(vk::ImageAspectFlagBits::eDepth);

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

    vk::RenderPassCreateInfo2 renderPassCreateInfo;
    renderPassCreateInfo
        .setAttachments(attachmentDescriptions)
        .setSubpasses(subpassDescription)
        .setDependencies(subpassDependency);

    return rendererBackend.CreateRenderPass(renderPassCreateInfo, renderpass.name);
}

std::vector<vk::Framebuffer> RenderGraph::CreateVkFrameBuffers(
    const Renderpass& renderpass,
    const std::vector<std::string>& outputResources,
    VL::RendererBackendVulkan& rendererBackend,
    bool bUseMsaa)
{
    uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    
    // 总览：attachments 顺序需与 RenderPass 保持一致
    // 1) (可选) MSAA 颜色视图
    // 2) resolve/非MSAA 颜色视图
    // 3) 深度视图
    // 确定framebuffer数量，含swapchain，数量需要等于swapChainImageCount，否者为1
    uint32_t framebufferSize = swapChainImageCount;

    std::vector<vk::Framebuffer> framebuffers;
    framebuffers.resize(framebufferSize);

    for (uint32_t i = 0; i < framebufferSize; i++)
    {
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
                attachments.push_back(rendererBackend.GetSwapchainImageViews()[i]);
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
            .setRenderPass(renderpass.renderPass)
            .setAttachments(attachments)
            .setWidth(renderpass.width)
            .setHeight(renderpass.height)
            .setLayers(1);
        
        framebuffers[i] = rendererBackend.CreateFramebuffer(
            framebufferCreateInfo,
            "Framebuffer: " + renderpass.name +
                " (SwapchainIndex " + std::to_string(i) + ")");
    }
    
    return framebuffers;
}

vk::Format RenderGraph::GetFormat(const std::string& formatStr)
{
    if (formatStr == "R8G8B8A8_SRGB")
    {
        return vk::Format::eR8G8B8A8Srgb;
    }
    else if (formatStr == "D32_SFLOAT")
    {
        return vk::Format::eD32Sfloat;
    }
    else if (formatStr == "R8G8B8A8_UNORM")
    {
        return vk::Format::eR8G8B8A8Unorm;
    }
    else if (formatStr == "R16G16B16A16_SFLOAT")
    {
        return vk::Format::eR16G16B16A16Sfloat;
    }
    else
    {
        throw std::runtime_error("Unsupported format string");
    }
}

vk::ImageUsageFlags RenderGraph::GetImageUsage(const std::vector<std::string>& usageStr)
{
    vk::ImageUsageFlags imageUsage;
    for(const auto& usage : usageStr)
    {
        if (usage == "colorAttachment")
        {
            imageUsage |= vk::ImageUsageFlagBits::eColorAttachment;
        }
        else if (usage == "depthStencilAttachment")
        {
            imageUsage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        }
        else if (usage == "sampled")
        {
            imageUsage |= vk::ImageUsageFlagBits::eSampled;
        }
        else if (usage == "transferSrc")
        {
            imageUsage |= vk::ImageUsageFlagBits::eTransferSrc;
        }
        else if (usage == "transferDst")
        {
            imageUsage |= vk::ImageUsageFlagBits::eTransferDst;
        }
    }
    return imageUsage;
}

vk::AttachmentLoadOp RenderGraph::GetAttachmentLoadOp(const std::string& loadOpStr)
{
    if (loadOpStr == "load")
    {
        return vk::AttachmentLoadOp::eLoad;
    }
    if (loadOpStr == "dontCare")
    {
        return vk::AttachmentLoadOp::eDontCare;
    }
    return vk::AttachmentLoadOp::eClear;
}

vk::AttachmentStoreOp RenderGraph::GetAttachmentStoreOp(const std::string& storeOpStr)
{
    if (storeOpStr == "dontCare")
    {
        return vk::AttachmentStoreOp::eDontCare;
    }
    return vk::AttachmentStoreOp::eStore;
}

bool RenderGraph::IsDepthResource(const std::string& resourceName) const
{
    auto msaaIt = resourcesMsaa.find(resourceName);
    if (msaaIt != resourcesMsaa.end() && !msaaIt->second.empty())
    {
        return CommonFunction::IsDepthFormat(msaaIt->second[0].format);
    }

    auto resolveIt = resourcesResolve.find(resourceName);
    if (resolveIt != resourcesResolve.end() && !resolveIt->second.empty())
    {
        return CommonFunction::IsDepthFormat(resolveIt->second[0].format);
    }

    return false;
}

std::vector<vk::ClearValue> RenderGraph::GetClearValues(
    const std::vector<std::string>& outputResources,
    bool bUseMsaa)
{
    std::vector<vk::ClearValue> clearValues;
    vk::ClearValue clearColor;
    clearColor.setColor(vk::ClearColorValue(std::array<float, 4>{0.2f, 0.2f, 0.2f, 1.0f}));
    vk::ClearValue clearDepth;
    clearDepth.setDepthStencil(vk::ClearDepthStencilValue(1.0f, 0));
    // 总览：清除值顺序需与 RenderPass attachments 顺序一致
    // 1) 颜色（MSAA/非MSAA）
    // 2) 仅在启用 MSAA 时追加 resolve 颜色
    // 3) 若包含深度资源，追加深度清除
    // 1. Color Attachments (MSAA & Resolve)
    for (const auto& resourceName : outputResources)
    {
        if (IsDepthResource(resourceName)) continue;

        // MSAA / Main Attachment
        clearValues.push_back(clearColor);
    }
    
    // 2. Resolve Attachments
    if (bUseMsaa)
    {
        for (const auto& resourceName : outputResources)
        {
            if (IsDepthResource(resourceName)) continue;
            
            clearValues.push_back(clearColor);
        }
    }

    // 3. Depth Attachment
    bool bHasDepth = false;
    for(const auto& r : outputResources)
    {
        if (IsDepthResource(r))
        {
            bHasDepth = true;
            break;
        }
    }
    if (bHasDepth)
    {
        clearValues.push_back(clearDepth);
        if (bUseMsaa)
        {
            clearValues.push_back(clearDepth);
        }
    }

    return clearValues;
}

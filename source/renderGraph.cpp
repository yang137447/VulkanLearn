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
#include "render/resource/resourceRetireQueue.h"
#include "render/resource/rendererResourceCache.h"
#include "texture.h"
#include <algorithm>
#include <memory>
#include <stdint.h>
#include <stdexcept>
#include <utility>
#include <unordered_set>
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

void AddDescriptorPoolBinding(
    std::vector<vk::DescriptorPoolSize>& descriptorPoolSizes,
    std::unordered_set<uint64_t>& seenBindings,
    uint32_t set,
    uint32_t binding,
    vk::DescriptorType type,
    uint32_t descriptorCount)
{
    const uint64_t bindingKey =
        (static_cast<uint64_t>(set) << 32u) | binding;
    if (!seenBindings.insert(bindingKey).second)
    {
        return;
    }

    const uint32_t requiredCount = descriptorCount;
    for (vk::DescriptorPoolSize& poolSize : descriptorPoolSizes)
    {
        if (poolSize.type == type)
        {
            poolSize.descriptorCount += requiredCount;
            return;
        }
    }

    vk::DescriptorPoolSize poolSize;
    poolSize
        .setType(type)
        .setDescriptorCount(requiredCount);
    descriptorPoolSizes.push_back(poolSize);
}
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
    // PassRuntime 已在 render pass 开始后设置 viewport/scissor；全屏三角形
    // 只负责提交几何，不能在这里恢复整张资源的动态状态。
    commandBuffer.draw(3, 1, 0, 0);
}

void Renderpass::CreateUniformBuffers()
{
    // Pass parameters live in MaterialInstance UBOs; Renderpass itself owns no
    // extra per-pass uniform buffer.
}
void Renderpass::SetupDescriptors(
    const RenderGraph& renderGraph,
    VL::RendererBackendVulkan& rendererBackend,
    const VL::RendererDescriptorContext& descriptorContext)
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
            if (inputDescriptor.source == "worldTexture")
            {
                if (descriptorContext.resourceCache == nullptr)
                {
                    throw std::runtime_error(
                        "Renderpass external input is missing a resource cache: " +
                        name + ":" + inputResource);
                }
                const std::shared_ptr<Texture>* texture =
                    descriptorContext.resourceCache->GetWorldTexture(inputResource);
                if (texture == nullptr || *texture == nullptr)
                {
                    throw std::runtime_error(
                        "Renderpass external worldTexture is not active: " +
                        name + ":" + inputResource);
                }
                if (inputDescriptor.binding >= inputDescriptorImageInfos[imageIndex].size())
                {
                    inputDescriptorImageInfos[imageIndex].resize(
                        inputDescriptor.binding + 1);
                }
                inputDescriptorImageInfos[imageIndex][inputDescriptor.binding] =
                    (*texture)->GetDescriptorInfo();
                continue;
            }
            
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
    // Pass Set 3 的 layout 必须由 RenderGraph 输入全集决定，不能跟随当前
    // pass 材质的反射子集变化；否则 Hair 等场景材质无法安全复用同一 pass。
    for (const VL::CompiledRenderGraphPassInputDescriptor& inputDescriptor :
         inputDescriptorPlan)
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
        std::unordered_set<uint64_t> seenDescriptorBindings;
        for (const ShaderBinding& binding : shaderBindings)
        {
            AddDescriptorPoolBinding(
                descriptorPoolSizes,
                seenDescriptorBindings,
                binding.set,
                binding.binding,
                binding.type,
                swapChainImageCount * binding.descriptorCount);
        }
        // Set 1 layout 使用完整 M_ schema；即使编译器优化掉未使用纹理，
        // descriptor pool 仍必须覆盖实际分配的完整 layout。
        for (const ShaderBinding& binding :
             baseMaterial->GetMaterialDescriptorSchema().GetSetBindings())
        {
            AddDescriptorPoolBinding(
                descriptorPoolSizes,
                seenDescriptorBindings,
                binding.set,
                binding.binding,
                binding.type,
                swapChainImageCount * binding.descriptorCount);
        }
        // Set 3 layout 使用 RenderGraph 输入全集；Hair LUT 可能不在普通
        // ThinTranslucent variant 的反射子集中，但仍属于同一 pass contract。
        for (const VL::CompiledRenderGraphPassInputDescriptor& inputDescriptor :
             inputDescriptorPlan)
        {
            AddDescriptorPoolBinding(
                descriptorPoolSizes,
                seenDescriptorBindings,
                PassSetIndex,
                inputDescriptor.binding,
                vk::DescriptorType::eCombinedImageSampler,
                swapChainImageCount);
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

void RenderGraph::SwapState(RenderGraph& other) noexcept
{
    resourcesMsaa.swap(other.resourcesMsaa);
    resourcesResolve.swap(other.resourcesResolve);
    renderpassesOrdered.swap(other.renderpassesOrdered);
    renderpasses.swap(other.renderpasses);
    canonicalShadowPassName.swap(other.canonicalShadowPassName);
    using std::swap;
    swap(ownerGeneration, other.ownerGeneration);
    compiledFrameGraph.resources.swap(
        other.compiledFrameGraph.resources);
    compiledFrameGraph.passes.swap(
        other.compiledFrameGraph.passes);
    compiledFrameGraph.passOrder.swap(
        other.compiledFrameGraph.passOrder);
    compiledFrameGraph.resourceUsagePlans.swap(
        other.compiledFrameGraph.resourceUsagePlans);
    descriptorPlanCache.Swap(other.descriptorPlanCache);
}

bool RenderGraph::HasState() const noexcept
{
    return !resourcesMsaa.empty() ||
        !resourcesResolve.empty() ||
        !renderpasses.empty() ||
        !renderpassesOrdered.empty();
}

void RenderGraph::SetTestFaultInjection(
    TestFaultInjection injection) noexcept
{
    testFaultInjection = injection;
    testResourceCreationCount = 0;
    testRenderPassCreationCount = 0;
    testFramebufferCreationCount = 0;
    testDescriptorCreationCount = 0;
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
    canonicalShadowPassName.clear();
    descriptorPlanCache.Clear();
    ownerGeneration = 0;
}

void RenderGraph::LoadRenderGraph(
    const nlohmann::json& renderGraphJson,
    VL::RendererBackendVulkan& rendererBackend)
{
    try
    {
        VL::RenderGraphCompiler compiler;
        auto compileResult = compiler.Compile(renderGraphJson);
        if (compileResult.IsFailure())
        {
            throw std::runtime_error(
                VL::FormatRuntimeError(
                    compileResult.Error()));
        }
        compiledFrameGraph =
            std::move(compileResult.Value());
        descriptorPlanCache.Clear();

        uint32_t swapChainImageCount =
            rendererBackend.GetSwapchainImageCount();
        // GPU object creation still happens here; the compiler above now owns
        // the validated pass/resource order.
        for (const VL::CompiledRenderGraphResource& resourceDesc :
             compiledFrameGraph.resources)
        {
            std::string name = resourceDesc.name;
            if(name == "swapChain") continue;

            // 先建立msaa资源
            if (name != "shadowMap")
            {
                std::vector<RenderResource>& msaaResources =
                    resourcesMsaa.try_emplace(name).first->second;
                msaaResources.reserve(swapChainImageCount);
                for(uint32_t i = 0; i < swapChainImageCount; ++i)
                {
                    RenderResource resource =
                        CreateRenderResource(
                            resourceDesc,
                            rendererBackend,
                            true);
                    msaaResources.push_back(
                        std::move(resource));
                }
            }

            // 再建立resolve资源
            std::vector<RenderResource>& resolveResources =
                resourcesResolve.try_emplace(name).first->second;
            resolveResources.reserve(
                swapChainImageCount);
            for(uint32_t i = 0; i < swapChainImageCount; ++i)
            {
                RenderResource resourceResolve =
                    CreateRenderResource(
                        resourceDesc,
                        rendererBackend);
                resolveResources.push_back(
                    std::move(resourceResolve));
            }
        }
        for (const VL::CompiledRenderGraphPass& passDesc :
             compiledFrameGraph.passes)
        {
            std::string passName = passDesc.name;
            renderpassesOrdered.push_back(passName);
            Renderpass renderpass =
                CreateRenderpass(
                    passDesc,
                    rendererBackend);
            renderpasses.emplace(
                passName,
                std::move(renderpass));
        }
        ValidateAndResolveCanonicalShadowPass();
    }
    catch (...)
    {
        Shutdown(
            rendererBackend,
            VL::RenderGraphReleaseMode::Immediate);
        throw;
    }
}

Renderpass* RenderGraph::FindCanonicalShadowPass()
{
    if (canonicalShadowPassName.empty())
    {
        return nullptr;
    }
    return &renderpasses.at(canonicalShadowPassName);
}

Renderpass& RenderGraph::RequireUniquePass(
    VL::RenderGraphPassType type)
{
    return const_cast<Renderpass&>(
        static_cast<const RenderGraph&>(*this)
            .RequireUniquePass(type));
}

const Renderpass& RenderGraph::RequireUniquePass(
    VL::RenderGraphPassType type) const
{
    const Renderpass* matchedPass = nullptr;
    for (const std::string& passName : renderpassesOrdered)
    {
        const Renderpass& renderpass =
            renderpasses.at(passName);
        if (renderpass.type != type)
        {
            continue;
        }
        if (matchedPass != nullptr)
        {
            throw std::runtime_error(
                "RenderGraph expected one pass of type '" +
                std::string(VL::RenderGraphPassTypeToString(type)) +
                "', but found both '" + matchedPass->name +
                "' and '" + renderpass.name + "'");
        }
        matchedPass = &renderpass;
    }

    if (matchedPass == nullptr)
    {
        throw std::runtime_error(
            "RenderGraph is missing required pass type '" +
            std::string(VL::RenderGraphPassTypeToString(type)) +
            "'");
    }
    return *matchedPass;
}

void RenderGraph::ValidateAndResolveCanonicalShadowPass()
{
    canonicalShadowPassName.clear();
    const Renderpass* canonicalShadowPass = nullptr;
    std::string canonicalShadowMaterialPath;

    // 第一条 Shadow pass 是专用 Shadow pipeline 的创建模板。其他 cascade
    // 必须共享相同管线合同和公共材质资产，才能复用同一套 pipeline/MI。
    for (const std::string& passName : renderpassesOrdered)
    {
        const Renderpass& renderpass = renderpasses.at(passName);
        if (renderpass.type !=
            VL::RenderGraphPassType::Shadow)
        {
            continue;
        }

        const std::string materialPath =
            MaterialAssetUtils::NormalizeAssetPath(renderpass.materialInstancePath);
        if (canonicalShadowPass == nullptr)
        {
            canonicalShadowPass = &renderpass;
            canonicalShadowPassName = passName;
            canonicalShadowMaterialPath = materialPath;
            continue;
        }

        if (renderpass.pipelineContractKey != canonicalShadowPass->pipelineContractKey)
        {
            throw std::runtime_error(
                "Shadow pass '" + renderpass.name +
                "' is not pipeline-compatible with canonical shadow pass '" +
                canonicalShadowPass->name + "'");
        }
        if (materialPath != canonicalShadowMaterialPath)
        {
            throw std::runtime_error(
                "Shadow pass '" + renderpass.name +
                "' must use the canonical shadow material instance path '" +
                canonicalShadowMaterialPath + "'");
        }
    }
}

void RenderGraph::RenderInitialize(
    VL::RendererBackendVulkan& rendererBackend,
    const VL::RendererDescriptorContext& descriptorContext)
{
    for (const std::string& passName : renderpassesOrdered)
    {
        Renderpass& renderpass = renderpasses.at(passName);
        renderpass.CreateUniformBuffers();
        renderpass.SetupDescriptors(*this, rendererBackend, descriptorContext);
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
        MaybeFailDescriptorCreation();
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
        // World-local 纹理替换会同时改变缓存的 image view/sampler 与写入计划。
        // 必须先重建 pass-input 快照，避免刷新时把待退休的 Eye LUT 写回活动 descriptor。
        renderpass.SetupDescriptors(*this, rendererBackend, descriptorContext);
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

std::unordered_map<std::string, std::shared_ptr<MaterialInstance>> RenderGraph::CapturePassMaterialInstances() const
{
    std::unordered_map<std::string, std::shared_ptr<MaterialInstance>> passMaterials;
    for (const auto& [passName, renderpass] : renderpasses)
    {
        // Snapshot 在 graph/world 重建窗口內負責保活；Renderpass 本身仍只持有 weak_ptr，
        // 不改變正常運行時的資源所有權，也不形成循環引用。
        passMaterials.emplace(passName, renderpass.materialInstance.lock());
    }
    return passMaterials;
}

void RenderGraph::RestorePassMaterialInstances(
    const std::unordered_map<std::string, std::shared_ptr<MaterialInstance>>& passMaterials)
{
    if (testFaultInjection.failPassMaterialContract)
    {
        throw std::runtime_error(
            "Injected pass material contract failure");
    }

    // RenderGraph 在 resize 或 graph reload 后会生成新的 Renderpass 对象，旧 MI
    // 只有在资产身份和完整管线合同都兼容时才能继续复用。先预检所有需要材质的
    // pass，避免校验中途失败后留下部分恢复、部分未恢复的状态。
    for (const auto& [passName, renderpass] : renderpasses)
    {
        // geometry 等由场景对象提供材质的 pass 不持有 pass material，无需恢复。
        if (!renderpass.needsMaterial)
        {
            continue;
        }

        auto materialIt = passMaterials.find(passName);
        if (materialIt == passMaterials.end())
        {
            throw std::runtime_error(
                "Cannot restore missing material binding for pass: " + passName);
        }

        const std::shared_ptr<MaterialInstance>& restoredInstance = materialIt->second;
        // 初次 World 載入前 pass material 尚未建立，合法快照可以為空。
        // shared_ptr 快照已消除「捕獲後過期」狀態，因此空值表示原狀就是未綁定。
        if (!restoredInstance)
        {
            continue;
        }

        // pass 名称相同不代表配置未变；必须确认旧 MI 仍是新图声明的同一资产。
        if (restoredInstance->GetName() !=
            MaterialAssetUtils::NormalizeAssetPath(renderpass.materialInstancePath))
        {
            throw std::runtime_error(
                "Cannot restore a different material asset for pass: " + passName);
        }

        std::shared_ptr<Material> restoredMaterial =
            restoredInstance->GetBaseMaterial().lock();
        // RenderPass 兼容性不足以覆盖全部管线状态。采样数、附件数量、顶点输入、
        // 深度状态和 Shadow pass 类型均由 PassPipelineContractKey 统一校验。
        if (!restoredMaterial ||
            restoredMaterial->GetPassPipelineContractKey() != renderpass.pipelineContractKey)
        {
            throw std::runtime_error(
                "Cannot restore pass material across an incompatible pipeline contract: " +
                passName);
        }
    }

    // 全部预检通过后再统一写回，保证恢复操作对 RenderGraph 是原子的。
    for (auto& [passName, renderpass] : renderpasses)
    {
        auto materialIt = passMaterials.find(passName);
        if (materialIt != passMaterials.end())
        {
            renderpass.materialInstance = materialIt->second;
        }
    }
}

RenderResource RenderGraph::CreateRenderResource(
    const VL::CompiledRenderGraphResource& resourceDesc,
    VL::RendererBackendVulkan& rendererBackend,
    bool bIsMsaaSource)
{
    MaybeFailResourceCreation();
    RenderResource resource;
    try
    {
        resource.name = resourceDesc.name;
        resource.type = resourceDesc.type;
        resource.arrayLayers =
            resourceDesc.arrayLayers;
        resource.format =
            GetFormat(resourceDesc.format);

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
        resource.imageHandle =
            rendererBackend.GetImageHandle(
                resource.image);
        vk::ImageAspectFlagBits aspect =
            vk::ImageAspectFlagBits::eColor;
        if (bIsDepthFormat)
        {
            aspect =
                vk::ImageAspectFlagBits::eDepth;
        }
        const vk::ImageViewType imageViewType =
            resource.type == "texture2DArray"
                ? vk::ImageViewType::e2DArray
                : vk::ImageViewType::e2D;
        resource.imageView =
            rendererBackend.CreateImageView(
                resource.image,
                imageViewType,
                resource.format,
                aspect,
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
            aspect,
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
            ? (resource.name == "shadowMap"
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
        const bool bUseMsaa = passDesc.needMsaa;
        renderpass.pipelineContractKey.useVertexInput =
            passDesc.pipelineState.useVertexInput;
        renderpass.pipelineContractKey.depthTestEnable =
            passDesc.pipelineState.depthTestEnable;
        renderpass.pipelineContractKey.depthWriteEnable =
            passDesc.pipelineState.depthWriteEnable;
        renderpass.pipelineContractKey.depthCompareOp =
            GetDepthCompareOp(passDesc.pipelineState.depthCompareOp);
        renderpass.pipelineContractKey.isShadowPass = bIsShadowPass;

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
            if (bIsShadowPass)
            {
                renderpass.shadowCascadeIndex = output.layer;
            }
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

        if (renderpass.outputResources[0] == "swapChain")
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
    Renderpass& renderpass,
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
    renderpass.pipelineContractKey.renderPassCompatibilityKey = std::move(compatibilityKey);

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
        
            framebuffers.push_back(
                rendererBackend.CreateFramebuffer(
                    framebufferCreateInfo,
                    "Framebuffer: " +
                        renderpass.name +
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

void RenderGraph::MaybeFailResourceCreation()
{
    ++testResourceCreationCount;
    if (testFaultInjection.failResourceCreationAt != 0 &&
        testResourceCreationCount ==
            testFaultInjection.failResourceCreationAt)
    {
        throw std::runtime_error(
            "Injected render graph resource creation failure");
    }
}

void RenderGraph::MaybeFailRenderPassCreation()
{
    ++testRenderPassCreationCount;
    if (testFaultInjection.failRenderPassCreationAt != 0 &&
        testRenderPassCreationCount ==
            testFaultInjection.failRenderPassCreationAt)
    {
        throw std::runtime_error(
            "Injected render pass creation failure");
    }
}

void RenderGraph::MaybeFailFramebufferCreation()
{
    ++testFramebufferCreationCount;
    if (testFaultInjection.failFramebufferCreationAt != 0 &&
        testFramebufferCreationCount ==
            testFaultInjection.failFramebufferCreationAt)
    {
        throw std::runtime_error(
            "Injected framebuffer creation failure");
    }
}

void RenderGraph::MaybeFailDescriptorCreation()
{
    ++testDescriptorCreationCount;
    if (testFaultInjection.failDescriptorCreationAt != 0 &&
        testDescriptorCreationCount ==
            testFaultInjection.failDescriptorCreationAt)
    {
        throw std::runtime_error(
            "Injected render graph descriptor creation failure");
    }
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

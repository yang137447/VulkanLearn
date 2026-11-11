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
                const std::shared_ptr<Texture> texture =
                    descriptorContext.resourceCache->GetWorldTexture(inputResource);
                if (texture == nullptr)
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
                    texture->GetDescriptorInfo();
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
    autoExposure.swap(other.autoExposure);
    volumetricFog.swap(other.volumetricFog);
    lightCulling.swap(other.lightCulling);
    hairSimulation.swap(other.hairSimulation);
    skinDeform.swap(other.skinDeform);
    resourcesResolve.swap(other.resourcesResolve);
    sceneViewportOutputs.swap(other.sceneViewportOutputs);
    renderpassesOrdered.swap(other.renderpassesOrdered);
    renderpasses.swap(other.renderpasses);
    canonicalShadowPassName.swap(other.canonicalShadowPassName);
    canonicalLocalShadowPassName.swap(other.canonicalLocalShadowPassName);
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
    sourceJson.swap(other.sourceJson);
}

bool RenderGraph::HasState() const noexcept
{
    return !resourcesMsaa.empty() ||
        !resourcesResolve.empty() ||
        !sceneViewportOutputs.empty() ||
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
    if (autoExposure && releaseMode == VL::RenderGraphReleaseMode::Retire)
    {
        auto& retireQueue = VL::ResourceRetireQueue::GetInstance();
        retireQueue.RetireShared("AutoExposure:Graph", ownerGeneration,
            retireQueue.GetLastSubmittedEpoch(), std::move(autoExposure));
    }
    if (hairSimulation && releaseMode == VL::RenderGraphReleaseMode::Retire)
    {
        auto& retireQueue = VL::ResourceRetireQueue::GetInstance();
        retireQueue.RetireShared("HairSimulation:Graph", ownerGeneration,
            retireQueue.GetLastSubmittedEpoch(), std::move(hairSimulation));
    }
    if (skinDeform && releaseMode == VL::RenderGraphReleaseMode::Retire)
    {
        auto& retireQueue = VL::ResourceRetireQueue::GetInstance();
        retireQueue.RetireShared("SkinDeform:Graph", ownerGeneration,
            retireQueue.GetLastSubmittedEpoch(), std::move(skinDeform));
    }
    skinDeform.reset();
    hairSimulation.reset();
    autoExposure.reset();
    if (volumetricFog)
    {
        if (releaseMode == VL::RenderGraphReleaseMode::Retire)
        {
            auto& retireQueue = VL::ResourceRetireQueue::GetInstance();
            retireQueue.RetireShared(
                "VolumetricFog:Graph", ownerGeneration,
                retireQueue.GetLastSubmittedEpoch(),
                std::move(volumetricFog));
        }
        else
        {
            volumetricFog->Shutdown(rendererBackend);
            volumetricFog.reset();
        }
    }
    if (lightCulling)
    {
        if (releaseMode == VL::RenderGraphReleaseMode::Retire)
        {
            auto& retireQueue = VL::ResourceRetireQueue::GetInstance();
            retireQueue.RetireShared(
                "LightCulling:Graph", ownerGeneration,
                retireQueue.GetLastSubmittedEpoch(),
                std::move(lightCulling));
        }
        else
        {
            lightCulling->Shutdown(rendererBackend);
            lightCulling.reset();
        }
    }
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
    // framebuffer 先退休/释放，再处理它引用的 Viewport 图像；候选失败走
    // Immediate，已提交的旧 graph 走同一 GPU epoch 退休路径。
    for (RenderResource& resource : sceneViewportOutputs)
    {
        DestroyRenderResource(resource, rendererBackend, releaseMode);
    }

    resourcesMsaa.clear();
    resourcesResolve.clear();
    sceneViewportOutputs.clear();
    renderpasses.clear();
    renderpassesOrdered.clear();
    canonicalShadowPassName.clear();
    canonicalLocalShadowPassName.clear();
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
        sourceJson = renderGraphJson;
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
            if (name != "shadowMap" &&
                name != "localShadowMap" &&
                std::find(resourceDesc.usage.begin(), resourceDesc.usage.end(), "storage") == resourceDesc.usage.end())
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
        CreateSceneViewportOutputs(rendererBackend);
        for (const VL::CompiledRenderGraphPass& passDesc :
             compiledFrameGraph.passes)
        {
            std::string passName = passDesc.name;
            renderpassesOrdered.push_back(passName);
            // map 分配先于 GPU 创建；否则 emplace 分配失败会遗失局部 pass
            // 的裸 Vulkan 所有权。创建成功后仅以 noexcept move 接收状态。
            Renderpass& renderpass = renderpasses.try_emplace(passName).first->second;
            renderpass = CreateRenderpass(passDesc, rendererBackend);
        }
        ValidateAndResolveCanonicalShadowPass();
        ValidateAndResolveCanonicalLocalShadowPass();
    }
    catch (...)
    {
        Shutdown(
            rendererBackend,
            VL::RenderGraphReleaseMode::Immediate);
        throw;
    }
}

const RenderResource* RenderGraph::GetSceneViewportOutput(uint32_t imageIndex) const noexcept
{
    return imageIndex < sceneViewportOutputs.size()
        ? &sceneViewportOutputs[imageIndex] : nullptr;
}

void RenderGraph::CreateSceneViewportOutputs(VL::RendererBackendVulkan& rendererBackend)
{
    if (!VL::ValidateSceneViewportOutputs(compiledFrameGraph)) return;

    // backing extent 与 swapchain 一致，UI 使用完整 UV 而非拷贝/裁剪屏幕。
    // 非 JSON 资源名只用于调试；格式必须来自实际 swapchain，不能假定 RGBA。
    VL::CompiledRenderGraphResource description;
    description.name = "sceneViewportOutput";
    description.usage = {"colorAttachment", "sampled"};
    const uint32_t imageCount = rendererBackend.GetSwapchainImageCount();
    sceneViewportOutputs.reserve(imageCount);
    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        description.name = "sceneViewportOutput_" + std::to_string(imageIndex);
        sceneViewportOutputs.push_back(CreateRenderResource(
            description, rendererBackend, false, rendererBackend.GetSwapchainImageFormat()));
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

Renderpass* RenderGraph::FindCanonicalLocalShadowPass()
{
    if (canonicalLocalShadowPassName.empty())
    {
        return nullptr;
    }
    return &renderpasses.at(canonicalLocalShadowPassName);
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

void RenderGraph::ValidateAndResolveCanonicalLocalShadowPass()
{
    canonicalLocalShadowPassName.clear();
    const Renderpass* canonicalLocalShadowPass = nullptr;
    std::string canonicalLocalShadowMaterialPath;

    // 所有局部阴影层必须共享同一管线合同和公共材质，才能复用同一套
    // LocalShadow pipeline/描述符布局；layer 的连续性由编译期 output.layer 保证。
    for (const std::string& passName : renderpassesOrdered)
    {
        const Renderpass& renderpass = renderpasses.at(passName);
        if (renderpass.type !=
            VL::RenderGraphPassType::LocalShadow)
        {
            continue;
        }

        const std::string materialPath =
            MaterialAssetUtils::NormalizeAssetPath(renderpass.materialInstancePath);
        if (canonicalLocalShadowPass == nullptr)
        {
            canonicalLocalShadowPass = &renderpass;
            canonicalLocalShadowPassName = passName;
            canonicalLocalShadowMaterialPath = materialPath;
            continue;
        }

        if (renderpass.pipelineContractKey !=
            canonicalLocalShadowPass->pipelineContractKey)
        {
            throw std::runtime_error(
                "LocalShadow pass '" + renderpass.name +
                "' is not pipeline-compatible with canonical local shadow pass '" +
                canonicalLocalShadowPass->name + "'");
        }
        if (materialPath != canonicalLocalShadowMaterialPath)
        {
            throw std::runtime_error(
                "LocalShadow pass '" + renderpass.name +
                "' must use the canonical local shadow material instance path '" +
                canonicalLocalShadowMaterialPath + "'");
        }
    }
}

void RenderGraph::InitializeAutoExposure(PipelineFactory& factory,
    VL::RendererBackendVulkan& backend)
{
    if (autoExposure) return;
    for (const auto& pass : compiledFrameGraph.passes)
    {
        if (pass.type != VL::RenderGraphPassType::AutoExposure) continue;
        auto candidate = std::make_shared<VL::AutoExposureCompute>();
        candidate->Initialize(factory, backend, *this);
        autoExposure = std::move(candidate);
        return;
    }
}

void RenderGraph::InitializeVolumetricFog(
    PipelineFactory& factory,
    VL::RendererBackendVulkan& backend,
    const VL::RendererDescriptorContext& descriptorContext)
{
    if (volumetricFog)
    {
        return;
    }
    // 没有 fog 节点的自定义图不创建 owner；调用方按空指针处理。
    bool hasInjectionPass = false;
    for (const auto& pass : compiledFrameGraph.passes)
    {
        if (pass.type == VL::RenderGraphPassType::VolumetricFog)
        {
            hasInjectionPass = true;
            break;
        }
    }
    if (!hasInjectionPass)
    {
        return;
    }
    auto candidate = std::make_shared<VL::VolumetricFogRenderer>();
    candidate->Initialize(factory, backend, *this, descriptorContext);
    volumetricFog = std::move(candidate);
}

void RenderGraph::InitializeLightCulling(
    PipelineFactory& factory,
    VL::RendererBackendVulkan& backend,
    const VL::RendererDescriptorContext& descriptorContext)
{
    if (lightCulling)
    {
        return;
    }
    // 没有聚类节点的自定义图不创建 owner；shader 的 enabled=0 回退分支
    // 保证行为与引入聚类前一致。
    bool hasCullingPass = false;
    for (const auto& pass : compiledFrameGraph.passes)
    {
        if (pass.type == VL::RenderGraphPassType::LightCulling)
        {
            hasCullingPass = true;
            break;
        }
    }
    if (!hasCullingPass)
    {
        return;
    }
    auto candidate = std::make_shared<VL::LightCullingRenderer>();
    candidate->Initialize(factory, backend, *this, descriptorContext);
    lightCulling = std::move(candidate);
}

vk::Extent2D RenderGraph::GetSceneColorExtent() const noexcept
{
    const auto found = resourcesResolve.find("sceneColor");
    if (found == resourcesResolve.end() || found->second.empty())
    {
        return vk::Extent2D(0, 0);
    }
    const RenderResource& resource = found->second.front();
    return vk::Extent2D(resource.width, resource.height);
}

void RenderGraph::InitializeHairSimulation(PipelineFactory& factory,
    VL::RendererBackendVulkan& backend)
{
    // 与 AutoExposure 相同：已有实例时不重复创建，候选图由调用方持有到事务提交。
    if (hairSimulation) return;
    auto candidate = std::make_shared<VL::Hair::HairSimulationPass>();
    candidate->Initialize(factory, backend);
    hairSimulation = std::move(candidate);
}

void RenderGraph::InitializeSkinDeform(PipelineFactory& factory,
    VL::RendererBackendVulkan& backend)
{
    // 只有 --prerender-skin 打开时调用方才会走到这里；关闭时 pass 不创建，
    // 也不编译 compute pipeline。
    if (skinDeform) return;
    auto candidate = std::make_shared<VL::Skinning::SkinDeformPass>();
    candidate->Initialize(factory, backend);
    skinDeform = std::move(candidate);
}

void RenderGraph::RenderInitialize(
    VL::RendererBackendVulkan& rendererBackend,
    const VL::RendererDescriptorContext& descriptorContext)
{
    for (const std::string& passName : renderpassesOrdered)
    {
        Renderpass& renderpass = renderpasses.at(passName);
        // 专用 compute / 自带 pipeline 的 pass 不经过 Material pass 描述符。
        if (renderpass.type == VL::RenderGraphPassType::AutoExposure ||
            renderpass.type == VL::RenderGraphPassType::ParticleSoftDepth ||
            renderpass.type == VL::RenderGraphPassType::VolumetricFog ||
            renderpass.type == VL::RenderGraphPassType::FogComposite ||
            renderpass.type == VL::RenderGraphPassType::LightCulling)
        {
            continue;
        }
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
    // 体积雾不走 pass material 描述符刷新，但同样依赖每 image 的
    // global/light/particle-light buffer；World 换代后必须一起更新。
    if (volumetricFog)
    {
        volumetricFog->RefreshDescriptors(
            rendererBackend, descriptorContext);
    }
    if (lightCulling)
    {
        lightCulling->RefreshDescriptors(
            rendererBackend, descriptorContext);
    }
    for (auto& [passName, renderpass] : renderpasses)
    {
        if (renderpass.type == VL::RenderGraphPassType::AutoExposure ||
            renderpass.type == VL::RenderGraphPassType::ParticleSoftDepth ||
            renderpass.type == VL::RenderGraphPassType::VolumetricFog ||
            renderpass.type == VL::RenderGraphPassType::FogComposite ||
            renderpass.type == VL::RenderGraphPassType::LightCulling)
        {
            continue;
        }
        // World-local 纹理替换会同时改变缓存的 image view/sampler 与写入计划。
        // 必须先重建 pass-input 快照，避免刷新时把待退休的 world-local 纹理写回活动 descriptor。
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

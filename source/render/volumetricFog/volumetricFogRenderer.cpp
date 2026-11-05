#include "render/volumetricFog/volumetricFogRenderer.h"

#include <algorithm>
#include <stdexcept>

#include "pipeline/graphicsPipelineBuilder.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorContext.h"
#include "renderGraph.h"
#include "shaderVariant.h"
#include "shaderReflect.h"

namespace VL
{

namespace
{
constexpr uint32_t kFogComputeGroupSize = 8;

const RenderResource& RequireImage(
    const RenderGraph& graph,
    const std::string& resourceName,
    uint32_t imageIndex)
{
    const auto& resolve = graph.GetResourcesResolve();
    const auto found = resolve.find(resourceName);
    if (found == resolve.end() || imageIndex >= found->second.size())
    {
        throw std::runtime_error(
            "Volumetric fog requires graph resource '" +
            resourceName + "' for this swapchain image.");
    }
    return found->second[imageIndex];
}

Renderpass* FindFogCompositePass(RenderGraph& graph)
{
    for (auto& [name, pass] : graph.GetRenderpasses())
    {
        if (pass.type == RenderGraphPassType::FogComposite)
        {
            return &pass;
        }
    }
    return nullptr;
}

std::vector<vk::DescriptorPoolSize> CollectPoolSizes(
    const PipelineBase& pipeline)
{
    std::vector<vk::DescriptorPoolSize> sizes;
    for (const ShaderBinding& binding : pipeline.GetShaderBindings())
    {
        auto existing = std::find_if(
            sizes.begin(), sizes.end(),
            [&binding](const vk::DescriptorPoolSize& size)
            {
                return size.type == binding.type;
            });
        if (existing == sizes.end())
        {
            sizes.push_back(vk::DescriptorPoolSize(
                binding.type, binding.descriptorCount));
        }
        else
        {
            existing->descriptorCount += binding.descriptorCount;
        }
    }
    return sizes;
}
} // namespace

VolumetricFogRenderer::~VolumetricFogRenderer()
{
    // 图事务的 Retire 路径会把 owner 交给退休队列，Shutdown 不会被调用；
    // 与 AutoExposureCompute 一样，由析构函数按仍有效的 backend 释放
    // descriptor pool（pipeline 的 shared_ptr 自身即可安全释放）。
    if (backend != nullptr)
    {
        DestroyDescriptorState(*backend);
    }
}

void VolumetricFogRenderer::Initialize(
    PipelineFactory& factory,
    RendererBackendVulkan& rendererBackend,
    RenderGraph& graph,
    const RendererDescriptorContext& descriptorContext)
{
    if (initialized)
    {
        return;
    }
    backend = &rendererBackend;

    const Renderpass& computePass =
        graph.RequireUniquePass(RenderGraphPassType::VolumetricFog);
    if (computePass.outputResources.empty())
    {
        throw std::runtime_error(
            "VolumetricFog pass must declare its fog volume output.");
    }
    fogResourceName = computePass.outputResources.at(0);
    const auto& fogImages =
        graph.GetResourcesResolve().at(fogResourceName);
    if (fogImages.empty())
    {
        throw std::runtime_error(
            "VolumetricFog output resource has no GPU images.");
    }
    width = fogImages.at(0).width;
    height = fogImages.at(0).height;
    layerCount = fogImages.at(0).arrayLayers;
    if (width == 0 || height == 0 || layerCount == 0)
    {
        throw std::runtime_error(
            "VolumetricFog output resource has an invalid extent.");
    }

    computePipeline =
        factory.CreateComputePipelineCandidate(
            "pass/volumetricFog", nullptr);
    if (!computePipeline)
    {
        throw std::runtime_error(
            "VolumetricFog failed to create its compute pipeline.");
    }

    // 合成 pass 是可选的：自定义图可以不包含 fogComposite。
    Renderpass* compositePass = FindFogCompositePass(graph);
    if (compositePass != nullptr)
    {
        ShaderVariantKey compositeVariant;
        compositeVariant.shaderName = "pass/fogComposite";
        compositeVariant.renderMode = RenderMode::Opaque;
        compositeVariant.shadingModelMacro = "SHADING_MODEL_UNLIT";
        const GraphicsShaderVariantArtifact& artifact =
            factory.PrepareGraphicsShaderVariant(compositeVariant);
        // 雾是预乘结果：rgb = in-scatter，a = 1 - transmittance。
        // One / OneMinusSrcAlpha 正好等于 S + sceneColor * T。
        compositePipeline = factory.CreateGraphicsPipeline(
            &compositePass->renderPass,
            compositePass->pipelineContractKey,
            artifact,
            vk::CullModeFlagBits::eNone,
            GraphicsPipelineBlendMode::PremultipliedAlpha);
        if (!compositePipeline)
        {
            throw std::runtime_error(
                "VolumetricFog failed to create its composite pipeline.");
        }
    }

    CreateComputeDescriptors(
        rendererBackend, graph, descriptorContext);
    if (compositePipeline)
    {
        CreateCompositeDescriptors(rendererBackend, graph);
    }
    initialized = true;
}

void VolumetricFogRenderer::CreateComputeDescriptors(
    RendererBackendVulkan& rendererBackend,
    RenderGraph& graph,
    const RendererDescriptorContext& descriptorContext)
{
    const uint32_t imageCount =
        rendererBackend.GetSwapchainImageCount();
    if (descriptorContext.globalUniformBufferInfos == nullptr ||
        descriptorContext.globalUniformBufferInfos->size() < imageCount ||
        descriptorContext.lightBufferInfos == nullptr ||
        descriptorContext.lightBufferInfos->size() < imageCount ||
        descriptorContext.particleLightBufferInfos == nullptr ||
        descriptorContext.particleLightBufferInfos->size() < imageCount)
    {
        throw std::runtime_error(
            "VolumetricFog descriptor context is missing per-image "
            "global/light/particle-light buffers.");
    }
    // 帧资源在 resize 时会重建；这里复制 buffer info，避免持有
    // RenderSystem 内部 vector 的指针跨越一代。
    globalUniformBufferInfos =
        *descriptorContext.globalUniformBufferInfos;

    const auto& layouts =
        computePipeline->GetDescriptorSetLayouts();
    if (layouts.size() < 2)
    {
        throw std::runtime_error(
            "VolumetricFog compute shader must declare set 0 and set 1.");
    }

    const uint32_t setCount = static_cast<uint32_t>(layouts.size());
    std::vector<vk::DescriptorPoolSize> poolSizes =
        CollectPoolSizes(*computePipeline);
    for (vk::DescriptorPoolSize& size : poolSizes)
    {
        // 一个 pipeline 的反射只描述一份 set；池要覆盖
        // imageCount × setCount 份 descriptor。
        size.descriptorCount *= imageCount * setCount;
    }
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount * setCount);
    computePool = rendererBackend.CreateDescriptorPool(
        poolInfo, "VolumetricFog:ComputeDescriptors");

    try
    {
        std::vector<vk::DescriptorSetLayout> setLayouts;
        setLayouts.reserve(imageCount * setCount);
        for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
        {
            for (const vk::DescriptorSetLayout& layout : layouts)
            {
                setLayouts.push_back(layout);
            }
        }
        std::vector<vk::DescriptorSet> allocatedSets(imageCount * setCount);
        vk::DescriptorSetAllocateInfo allocateInfo;
        allocateInfo
            .setDescriptorPool(computePool)
            .setSetLayouts(setLayouts);
        rendererBackend.AllocateDescriptorSets(allocateInfo, allocatedSets);
        computeSets.resize(imageCount);
        for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
        {
            computeSets[imageIndex].assign(
                allocatedSets.begin() + imageIndex * setCount,
                allocatedSets.begin() + (imageIndex + 1) * setCount);
        }

        images.resize(imageCount);
        const Renderpass& computePass =
            graph.RequireUniquePass(RenderGraphPassType::VolumetricFog);
        for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
        {
            ImageResources& resources = images[imageIndex];
            const RenderResource& fogImage = RequireImage(
                graph, fogResourceName, imageIndex);
            const RenderResource& sceneDepthImage = RequireImage(
                graph, "sceneDepth", imageIndex);
            const RenderResource& shadowImage = RequireImage(
                graph, "shadowMap", imageIndex);
            const RenderResource& localShadowImage = RequireImage(
                graph, "localShadowMap", imageIndex);

            resources.sceneDepth = vk::DescriptorImageInfo(
                sceneDepthImage.sampler,
                sceneDepthImage.GetShaderDescriptorView(),
                vk::ImageLayout::eDepthStencilReadOnlyOptimal);
            resources.shadowMap = vk::DescriptorImageInfo(
                shadowImage.sampler,
                shadowImage.GetShaderDescriptorView(),
                vk::ImageLayout::eDepthStencilReadOnlyOptimal);
            resources.localShadowMap = vk::DescriptorImageInfo(
                localShadowImage.sampler,
                localShadowImage.GetShaderDescriptorView(),
                vk::ImageLayout::eDepthStencilReadOnlyOptimal);
            resources.fogStorage = vk::DescriptorImageInfo(
                {},
                fogImage.GetShaderDescriptorView(),
                vk::ImageLayout::eGeneral);
            resources.fogSample = vk::DescriptorImageInfo(
                fogImage.sampler,
                fogImage.GetShaderDescriptorView(),
                vk::ImageLayout::eShaderReadOnlyOptimal);

            std::vector<vk::WriteDescriptorSet> writes;
            for (const ShaderBinding& binding :
                 computePipeline->GetShaderBindings())
            {
                vk::WriteDescriptorSet write;
                write
                    .setDstSet(computeSets.at(imageIndex).at(binding.set))
                    .setDstBinding(binding.binding)
                    .setDstArrayElement(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(binding.type);
                if (binding.set == 0 && binding.binding == 0)
                {
                    write.setBufferInfo(
                        globalUniformBufferInfos.at(imageIndex));
                }
                else if (binding.set == 0 && binding.binding == 1)
                {
                    write.setBufferInfo(
                        descriptorContext.lightBufferInfos->at(
                            imageIndex));
                }
                else if (binding.set == 0 && binding.binding == 4)
                {
                    write.setBufferInfo(
                        descriptorContext.particleLightBufferInfos->at(
                            imageIndex));
                }
                else if (binding.set == 0 && binding.binding == 2)
                {
                    write.setBufferInfo(
                        descriptorContext.lightGridBufferInfos->at(
                            imageIndex));
                }
                else if (binding.set == 0 && binding.binding == 3)
                {
                    write.setBufferInfo(
                        descriptorContext.lightIndexBufferInfos->at(
                            imageIndex));
                }
                else if (binding.set == 0 && binding.binding == 5)
                {
                    write.setBufferInfo(
                        descriptorContext.lightGridParamBufferInfos->at(
                            imageIndex));
                }
                else if (binding.set == 1 && binding.binding == 0)
                {
                    write.setImageInfo(resources.sceneDepth);
                }
                else if (binding.set == 1 && binding.binding == 1)
                {
                    write.setImageInfo(resources.shadowMap);
                }
                else if (binding.set == 1 && binding.binding == 2)
                {
                    write.setImageInfo(resources.localShadowMap);
                }
                else if (binding.set == 1 && binding.binding == 3)
                {
                    write.setImageInfo(resources.fogStorage);
                }
                else
                {
                    throw std::runtime_error(
                        "VolumetricFog compute shader declared an "
                        "unexpected binding.");
                }
                writes.push_back(write);
            }
            rendererBackend.UpdateDescriptorSets(writes);
        }
    }
    catch (...)
    {
        DestroyDescriptorState(rendererBackend);
        throw;
    }
}

void VolumetricFogRenderer::CreateCompositeDescriptors(
    RendererBackendVulkan& rendererBackend,
    RenderGraph& graph)
{
    const auto& layouts =
        compositePipeline->GetDescriptorSetLayouts();
    if (layouts.empty())
    {
        throw std::runtime_error(
            "VolumetricFog composite shader must declare set 0.");
    }
    const uint32_t imageCount = static_cast<uint32_t>(images.size());
    std::vector<vk::DescriptorPoolSize> poolSizes =
        CollectPoolSizes(*compositePipeline);
    for (vk::DescriptorPoolSize& size : poolSizes)
    {
        size.descriptorCount *= imageCount;
    }
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount);
    compositePool = rendererBackend.CreateDescriptorPool(
        poolInfo, "VolumetricFog:CompositeDescriptors");

    try
    {
        std::vector<vk::DescriptorSetLayout> setLayouts(
            imageCount, layouts.at(0));
        compositeSets.resize(imageCount);
        vk::DescriptorSetAllocateInfo allocateInfo;
        allocateInfo
            .setDescriptorPool(compositePool)
            .setSetLayouts(setLayouts);
        rendererBackend.AllocateDescriptorSets(
            allocateInfo, compositeSets);
        for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
        {
            const RenderResource& fogImage = RequireImage(
                graph, fogResourceName, imageIndex);
            const RenderResource& sceneDepthImage = RequireImage(
                graph, "sceneDepth", imageIndex);
            ImageResources& resources = images.at(imageIndex);
            resources.fogSample = vk::DescriptorImageInfo(
                fogImage.sampler,
                fogImage.GetShaderDescriptorView(),
                vk::ImageLayout::eShaderReadOnlyOptimal);
            resources.sceneDepth = vk::DescriptorImageInfo(
                sceneDepthImage.sampler,
                sceneDepthImage.GetShaderDescriptorView(),
                vk::ImageLayout::eDepthStencilReadOnlyOptimal);

            std::vector<vk::WriteDescriptorSet> writes;
            for (const ShaderBinding& binding :
                 compositePipeline->GetShaderBindings())
            {
                if (binding.set != 0)
                {
                    throw std::runtime_error(
                        "VolumetricFog composite shader must use set 0 only.");
                }
                vk::WriteDescriptorSet write;
                write
                    .setDstSet(compositeSets.at(imageIndex))
                    .setDstBinding(binding.binding)
                    .setDstArrayElement(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(binding.type);
                if (binding.binding == 0)
                {
                    write.setBufferInfo(
                        globalUniformBufferInfos.at(imageIndex));
                }
                else if (binding.binding == 1)
                {
                    write.setImageInfo(resources.fogSample);
                }
                else if (binding.binding == 2)
                {
                    write.setImageInfo(resources.sceneDepth);
                }
                else
                {
                    throw std::runtime_error(
                        "VolumetricFog composite shader declared an "
                        "unexpected binding.");
                }
                writes.push_back(write);
            }
            rendererBackend.UpdateDescriptorSets(writes);
        }
    }
    catch (...)
    {
        DestroyDescriptorState(rendererBackend);
        throw;
    }
}

void VolumetricFogRenderer::Shutdown(
    RendererBackendVulkan& rendererBackend)
{
    DestroyDescriptorState(rendererBackend);
    DestroyPipelines();
    width = 0;
    height = 0;
    layerCount = 0;
    initialized = false;
    backend = nullptr;
}

void VolumetricFogRenderer::RefreshDescriptors(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext)
{
    if (!initialized || !computePipeline)
    {
        return;
    }
    const uint32_t imageCount =
        static_cast<uint32_t>(computeSets.size());
    if (descriptorContext.globalUniformBufferInfos == nullptr ||
        descriptorContext.globalUniformBufferInfos->size() < imageCount ||
        descriptorContext.lightBufferInfos == nullptr ||
        descriptorContext.lightBufferInfos->size() < imageCount ||
        descriptorContext.particleLightBufferInfos == nullptr ||
        descriptorContext.particleLightBufferInfos->size() < imageCount)
    {
        throw std::runtime_error(
            "VolumetricFog descriptor refresh is missing per-image "
            "global/light/particle-light buffers.");
    }
    globalUniformBufferInfos = *descriptorContext.globalUniformBufferInfos;

    // 图资源句柄（雾/深度/阴影）在 World 换代时保持不变，只需重写 set 0。
    for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
    {
        std::vector<vk::WriteDescriptorSet> writes;
        for (const ShaderBinding& binding :
             computePipeline->GetShaderBindings())
        {
            if (binding.set != 0)
            {
                continue;
            }
            vk::WriteDescriptorSet write;
            write
                .setDstSet(computeSets.at(imageIndex).at(0))
                .setDstBinding(binding.binding)
                .setDstArrayElement(0)
                .setDescriptorCount(1)
                .setDescriptorType(binding.type);
            if (binding.binding == 0)
            {
                write.setBufferInfo(
                    globalUniformBufferInfos.at(imageIndex));
            }
            else if (binding.binding == 1)
            {
                write.setBufferInfo(
                    descriptorContext.lightBufferInfos->at(imageIndex));
            }
            else if (binding.binding == 4)
            {
                write.setBufferInfo(
                    descriptorContext.particleLightBufferInfos->at(
                        imageIndex));
            }
            else
            {
                continue;
            }
            writes.push_back(write);
        }
        if (!writes.empty())
        {
            rendererBackend.UpdateDescriptorSets(writes);
        }
    }
}

void VolumetricFogRenderer::DestroyDescriptorState(
    RendererBackendVulkan& rendererBackend)
{
    if (computePool)
    {
        rendererBackend.DestroyDescriptorPool(computePool);
        computePool = vk::DescriptorPool{};
    }
    if (compositePool)
    {
        rendererBackend.DestroyDescriptorPool(compositePool);
        compositePool = vk::DescriptorPool{};
    }
    computeSets.clear();
    compositeSets.clear();
    images.clear();
    globalUniformBufferInfos.clear();
}

void VolumetricFogRenderer::DestroyPipelines()
{
    computePipeline.reset();
    compositePipeline.reset();
}

void VolumetricFogRenderer::Record(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    (void)renderPass;
    if (!initialized || !computePipeline ||
        imageIndex >= computeSets.size())
    {
        return;
    }

    // 粒子灯 SSBO 由本帧更早的 compute 写入；图屏障只覆盖图资源，
    // 非图资源需要在这里显式建立 compute -> compute 可见性。
    vk::MemoryBarrier lightBarrier;
    lightBarrier
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        lightBarrier,
        nullptr,
        nullptr);

    commandBuffer.bindPipeline(
        vk::PipelineBindPoint::eCompute,
        computePipeline->GetPipeline());
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        computePipeline->GetPipelineLayout(),
        0,
        computeSets.at(imageIndex),
        nullptr);
    commandBuffer.dispatch(
        (width + kFogComputeGroupSize - 1) / kFogComputeGroupSize,
        (height + kFogComputeGroupSize - 1) / kFogComputeGroupSize,
        1);
}

void VolumetricFogRenderer::RecordComposite(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    (void)renderPass;
    if (!compositePipeline || imageIndex >= compositeSets.size())
    {
        return;
    }
    commandBuffer.bindPipeline(
        vk::PipelineBindPoint::eGraphics,
        compositePipeline->GetPipeline());
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        compositePipeline->GetPipelineLayout(),
        0,
        compositeSets.at(imageIndex),
        nullptr);
    commandBuffer.draw(3, 1, 0, 0);
}

} // namespace VL

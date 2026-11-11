#include "render/lightCulling/lightCullingRenderer.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorContext.h"
#include "renderGraph.h"
#include "shaderReflect.h"

namespace VL
{
namespace
{

std::vector<vk::DescriptorPoolSize> CollectPoolSizes(
    const ComputePipeline& computePipeline)
{
    std::vector<vk::DescriptorPoolSize> sizes;
    for (const ShaderBinding& binding :
         computePipeline.GetShaderBindings())
    {
        auto existing = std::find_if(
            sizes.begin(),
            sizes.end(),
            [&binding](const vk::DescriptorPoolSize& size)
            {
                return size.type == binding.type;
            });
        if (existing == sizes.end())
        {
            sizes.push_back(vk::DescriptorPoolSize(
                binding.type,
                binding.descriptorCount));
        }
        else
        {
            existing->descriptorCount += binding.descriptorCount;
        }
    }
    return sizes;
}

const vk::DescriptorBufferInfo& RequireBufferInfo(
    const std::vector<vk::DescriptorBufferInfo>* infos,
    uint32_t imageIndex,
    const char* debugName)
{
    if (infos == nullptr || imageIndex >= infos->size())
    {
        throw std::runtime_error(
            std::string("LightCulling is missing ") +
            debugName +
            " for this swapchain image.");
    }
    return (*infos)[imageIndex];
}

} // namespace

LightCullingRenderer::~LightCullingRenderer()
{
    // 图事务的 Retire 路径不会调用 Shutdown；由析构函数按仍有效的
    // backend 释放 descriptor pool（pipeline 的 shared_ptr 自行释放）。
    if (backend != nullptr)
    {
        DestroyDescriptorState(*backend);
    }
}

bool LightCullingRenderer::ResolveExtent(RenderGraph& graph)
{
    // 聚类网格必须与光照 shader 的 gl_FragCoord 同尺寸。sceneColor 是
    // geometry/deferred 共用的主颜色目标，因此直接取它的解析尺寸；
    // 找不到时退化为不启用（pass 仍然存在，但参数写 enabled=0）。
    const auto& resolve = graph.GetResourcesResolve();
    const auto found = resolve.find("sceneColor");
    if (found == resolve.end() || found->second.empty())
    {
        return false;
    }
    const RenderResource& resource = found->second.front();
    width = resource.width;
    height = resource.height;
    if (width == 0 || height == 0)
    {
        return false;
    }
    ComputeLightTileGrid(width, height, tilesX, tilesY);
    return tilesX > 0 && tilesY > 0;
}

void LightCullingRenderer::Initialize(
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
    if (!ResolveExtent(graph))
    {
        throw std::runtime_error(
            "LightCulling requires a non-empty sceneColor render target.");
    }

    pipeline = factory.CreateComputePipelineCandidate(
        "pass/lightCulling",
        nullptr);
    if (!pipeline)
    {
        throw std::runtime_error(
            "LightCulling failed to create its compute pipeline.");
    }
    CreateDescriptors(rendererBackend, descriptorContext);
    initialized = true;
}

void LightCullingRenderer::CreateDescriptors(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext)
{
    const uint32_t imageCount =
        rendererBackend.GetSwapchainImageCount();
    if (descriptorContext.globalUniformBufferInfos == nullptr ||
        descriptorContext.globalUniformBufferInfos->size() < imageCount ||
        descriptorContext.lightBufferInfos == nullptr ||
        descriptorContext.lightBufferInfos->size() < imageCount ||
        descriptorContext.lightGridBufferInfos == nullptr ||
        descriptorContext.lightGridBufferInfos->size() < imageCount ||
        descriptorContext.lightIndexBufferInfos == nullptr ||
        descriptorContext.lightIndexBufferInfos->size() < imageCount ||
        descriptorContext.lightGridParamBufferInfos == nullptr ||
        descriptorContext.lightGridParamBufferInfos->size() < imageCount)
    {
        throw std::runtime_error(
            "LightCulling descriptor context is missing per-image "
            "global/light/grid buffers.");
    }

    const auto& layouts = pipeline->GetDescriptorSetLayouts();
    if (layouts.empty())
    {
        throw std::runtime_error(
            "LightCulling compute shader must declare set 0.");
    }
    std::vector<vk::DescriptorPoolSize> poolSizes =
        CollectPoolSizes(*pipeline);
    for (vk::DescriptorPoolSize& size : poolSizes)
    {
        size.descriptorCount *= imageCount;
    }
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(imageCount);
    descriptorPool = rendererBackend.CreateDescriptorPool(
        poolInfo,
        "LightCulling:Descriptors");

    try
    {
        const std::vector<vk::DescriptorSetLayout> setLayouts(
            imageCount,
            layouts.at(0));
        vk::DescriptorSetAllocateInfo allocateInfo;
        allocateInfo
            .setDescriptorPool(descriptorPool)
            .setSetLayouts(setLayouts);
        descriptorSets.resize(imageCount);
        rendererBackend.AllocateDescriptorSets(
            allocateInfo,
            descriptorSets);

        buffers.resize(imageCount);
        for (uint32_t imageIndex = 0;
             imageIndex < imageCount;
             ++imageIndex)
        {
            BufferView& view = buffers[imageIndex];
            view.globalUniformInfo = RequireBufferInfo(
                descriptorContext.globalUniformBufferInfos,
                imageIndex,
                "global uniform buffer");
            view.lightInfo = RequireBufferInfo(
                descriptorContext.lightBufferInfos,
                imageIndex,
                "light storage buffer");
            view.gridInfo = RequireBufferInfo(
                descriptorContext.lightGridBufferInfos,
                imageIndex,
                "light grid buffer");
            view.indexInfo = RequireBufferInfo(
                descriptorContext.lightIndexBufferInfos,
                imageIndex,
                "light index buffer");
            view.paramInfo = RequireBufferInfo(
                descriptorContext.lightGridParamBufferInfos,
                imageIndex,
                "light grid parameter buffer");
            view.grid = view.gridInfo.buffer;
            view.indices = view.indexInfo.buffer;
            view.params = view.paramInfo.buffer;

            std::vector<vk::WriteDescriptorSet> writes;
            for (const ShaderBinding& binding :
                 pipeline->GetShaderBindings())
            {
                if (binding.set != 0)
                {
                    throw std::runtime_error(
                        "LightCulling compute shader must use set 0 only.");
                }
                vk::WriteDescriptorSet write;
                write
                    .setDstSet(descriptorSets.at(imageIndex))
                    .setDstBinding(binding.binding)
                    .setDstArrayElement(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(binding.type);
                if (binding.binding == 0)
                {
                    write.setBufferInfo(view.globalUniformInfo);
                }
                else if (binding.binding == 1)
                {
                    write.setBufferInfo(view.lightInfo);
                }
                else if (binding.binding == 2)
                {
                    // 与 shader 布局一致：2 = std140 参数块（uniform），
                    // 3 = tile 网格，4 = tile 索引。
                    write.setBufferInfo(view.paramInfo);
                }
                else if (binding.binding == 3)
                {
                    write.setBufferInfo(view.gridInfo);
                }
                else if (binding.binding == 4)
                {
                    write.setBufferInfo(view.indexInfo);
                }
                else
                {
                    throw std::runtime_error(
                        "LightCulling compute shader declared an "
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

void LightCullingRenderer::RefreshDescriptors(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext)
{
    if (!initialized || !pipeline)
    {
        return;
    }
    const uint32_t imageCount =
        static_cast<uint32_t>(descriptorSets.size());
    if (descriptorContext.globalUniformBufferInfos == nullptr ||
        descriptorContext.globalUniformBufferInfos->size() < imageCount ||
        descriptorContext.lightBufferInfos == nullptr ||
        descriptorContext.lightBufferInfos->size() < imageCount ||
        descriptorContext.lightGridBufferInfos == nullptr ||
        descriptorContext.lightGridBufferInfos->size() < imageCount ||
        descriptorContext.lightIndexBufferInfos == nullptr ||
        descriptorContext.lightIndexBufferInfos->size() < imageCount ||
        descriptorContext.lightGridParamBufferInfos == nullptr ||
        descriptorContext.lightGridParamBufferInfos->size() < imageCount)
    {
        throw std::runtime_error(
            "LightCulling descriptor refresh is missing per-image "
            "global/light/grid buffers.");
    }
    for (uint32_t imageIndex = 0;
         imageIndex < imageCount;
         ++imageIndex)
    {
        BufferView& view = buffers.at(imageIndex);
        view.globalUniformInfo = RequireBufferInfo(
            descriptorContext.globalUniformBufferInfos,
            imageIndex,
            "global uniform buffer");
        view.lightInfo = RequireBufferInfo(
            descriptorContext.lightBufferInfos,
            imageIndex,
            "light storage buffer");
        view.gridInfo = RequireBufferInfo(
            descriptorContext.lightGridBufferInfos,
            imageIndex,
            "light grid buffer");
        view.indexInfo = RequireBufferInfo(
            descriptorContext.lightIndexBufferInfos,
            imageIndex,
            "light index buffer");
        view.paramInfo = RequireBufferInfo(
            descriptorContext.lightGridParamBufferInfos,
            imageIndex,
            "light grid parameter buffer");
        view.grid = view.gridInfo.buffer;
        view.indices = view.indexInfo.buffer;
        view.params = view.paramInfo.buffer;

        vk::WriteDescriptorSet globalWrite;
        globalWrite
            .setDstSet(descriptorSets.at(imageIndex))
            .setDstBinding(0)
            .setDstArrayElement(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eUniformBuffer)
            .setBufferInfo(view.globalUniformInfo);
        vk::WriteDescriptorSet lightWrite = globalWrite;
        lightWrite
            .setDstBinding(1)
            .setDescriptorType(vk::DescriptorType::eStorageBuffer)
            .setBufferInfo(view.lightInfo);
        // 2 = params (UBO)，3 = grid，4 = indices，与 shader 声明一致。
        vk::WriteDescriptorSet paramWrite = globalWrite;
        paramWrite
            .setDstBinding(2)
            .setBufferInfo(view.paramInfo);
        vk::WriteDescriptorSet gridWrite = lightWrite;
        gridWrite
            .setDstBinding(3)
            .setBufferInfo(view.gridInfo);
        vk::WriteDescriptorSet indexWrite = lightWrite;
        indexWrite
            .setDstBinding(4)
            .setBufferInfo(view.indexInfo);
        rendererBackend.UpdateDescriptorSets(
            {globalWrite, lightWrite, paramWrite, gridWrite, indexWrite});
    }
}

void LightCullingRenderer::Shutdown(RendererBackendVulkan& rendererBackend)
{
    DestroyDescriptorState(rendererBackend);
    pipeline.reset();
    width = 0;
    height = 0;
    tilesX = 0;
    tilesY = 0;
    initialized = false;
    backend = nullptr;
}

void LightCullingRenderer::DestroyDescriptorState(
    RendererBackendVulkan& rendererBackend)
{
    if (descriptorPool)
    {
        rendererBackend.DestroyDescriptorPool(descriptorPool);
        descriptorPool = vk::DescriptorPool{};
    }
    descriptorSets.clear();
    buffers.clear();
}

void LightCullingRenderer::Record(
    vk::CommandBuffer commandBuffer,
    uint32_t imageIndex,
    const Renderpass& renderPass)
{
    (void)renderPass;
    if (!initialized || !pipeline ||
        imageIndex >= descriptorSets.size())
    {
        return;
    }
    const BufferView& view = buffers.at(imageIndex);
    if (!view.grid || !view.indices || !view.params)
    {
        return;
    }

    LightGridParams params{};
    params.tileSize = kLightCullingTileSize;
    params.tilesX = tilesX;
    params.tilesY = tilesY;
    params.maxLightsPerTile = kLightCullingMaxLightsPerTile;
    params.enabled = 1;
    params.renderWidth = width;
    params.renderHeight = height;

    static bool loggedTileGrid = false;
    if (!loggedTileGrid)
    {
        loggedTileGrid = true;
        std::cout
            << "[LightCulling] tiles=" << tilesX << "x" << tilesY
            << " tileSize=" << kLightCullingTileSize
            << " render=" << width << "x" << height
            << " maxLightsPerTile=" << kLightCullingMaxLightsPerTile
            << std::endl;
    }

    // header 每帧清零；tiles 由 compute 全量重写，indices 由 count 门控。
    commandBuffer.fillBuffer(
        view.grid,
        0,
        static_cast<vk::DeviceSize>(LightGridHeaderBytes()),
        0);
    commandBuffer.updateBuffer(
        view.params,
        0,
        sizeof(LightGridParams),
        &params);

    // 1) transfer write（fill/update）-> compute 读；
    // 2) host write（本帧 UploadLightsForPass 的 memcpy）-> compute 读。
    vk::MemoryBarrier transferToCompute;
    transferToCompute
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(
            vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    vk::MemoryBarrier hostToCompute;
    hostToCompute
        .setSrcAccessMask(vk::AccessFlagBits::eHostWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    const vk::MemoryBarrier preDispatchBarriers[] = {
        transferToCompute,
        hostToCompute};
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer |
            vk::PipelineStageFlagBits::eHost,
        vk::PipelineStageFlagBits::eComputeShader,
        {},
        preDispatchBarriers,
        nullptr,
        nullptr);

    commandBuffer.bindPipeline(
        vk::PipelineBindPoint::eCompute,
        pipeline->GetPipeline());
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        pipeline->GetPipelineLayout(),
        0,
        descriptorSets.at(imageIndex),
        nullptr);
    commandBuffer.dispatch(
        (tilesX + kLightCullingGroupSize - 1) / kLightCullingGroupSize,
        (tilesY + kLightCullingGroupSize - 1) / kLightCullingGroupSize,
        1);

    // 聚类结果供 fragment（deferred/forward/粒子）与顶点阶段读取；
    // params 也在这里从 transfer 写入对 shader 可见。
    vk::MemoryBarrier computeToGraphics;
    computeToGraphics
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eFragmentShader |
            vk::PipelineStageFlagBits::eComputeShader,
        {},
        computeToGraphics,
        nullptr,
        nullptr);
}

} // namespace VL

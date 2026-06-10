#include "passBarrier.h"

#include "commonFunction.h"
#include "render/rendergraph/renderGraphCompiler.h"
#include "renderGraph.h"

void PassBarrier::PrepareForPass(
    vk::CommandBuffer& commandBuffer,
    const RenderGraph& renderGraph,
    size_t passIndex,
    uint32_t swapChainImageIndex)
{
    ApplyCompiledBarrierPlan(commandBuffer, renderGraph, passIndex, swapChainImageIndex);
}

void PassBarrier::ApplyCompiledBarrierPlan(
    vk::CommandBuffer& commandBuffer,
    const RenderGraph& renderGraph,
    size_t passIndex,
    uint32_t swapChainImageIndex)
{
    const VL::CompiledRenderGraph& compiledGraph = renderGraph.GetCompiledRenderGraph();
    if (passIndex >= compiledGraph.passes.size())
    {
        return;
    }

    const VL::CompiledRenderGraphPass& compiledPass = compiledGraph.passes[passIndex];
    for (const VL::CompiledRenderGraphBarrier& barrier : compiledPass.barriersBeforePass)
    {
        if (barrier.type == VL::CompiledRenderGraphBarrierType::AttachmentToShaderRead)
        {
            // 当前 pass 会把这个资源当 texture 采样，因此需要在 beginRenderPass 前保证它处于 shader-read layout。
            // 这里转换的是 Vulkan image layout，不是 R16G16B16A16 / D32 这类像素 format。
            TransitionAttachmentToShaderRead(commandBuffer, renderGraph, barrier.resource, swapChainImageIndex);
        }
        else if (barrier.type == VL::CompiledRenderGraphBarrierType::ShaderReadToAttachment)
        {
            // Render pass 的 attachment initialLayout 不能凭空从 shader-read 变回 attachment layout。
            // 如果资源刚被某个 pass 采样过，并且当前 pass 又要 load 它继续写/测，就需要在 beginRenderPass 前显式转回。
            TransitionShaderReadToAttachment(commandBuffer, renderGraph, barrier.resource, swapChainImageIndex);
        }
    }
}

void PassBarrier::TransitionAttachmentToShaderRead(
    vk::CommandBuffer& commandBuffer,
    const RenderGraph& renderGraph,
    const std::string& resourceName,
    uint32_t swapChainImageIndex)
{
    const auto& resolveMap = renderGraph.GetResourcesResolve();
    auto resourceIt = resolveMap.find(resourceName);
    if(resourceIt == resolveMap.end())
    {
        return;
    }

    const auto& resource = resourceIt->second[swapChainImageIndex];
    const bool bIsDepth = CommonFunction::IsDepthFormat(resource.format);

    vk::ImageAspectFlags aspectMask = bIsDepth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    if(bIsDepth && CommonFunction::HasStencilComponent(resource.format))
    {
        aspectMask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageMemoryBarrier imageBarrier;
    imageBarrier
        .setImage(resource.image)
        .setSubresourceRange(vk::ImageSubresourceRange(aspectMask, 0, 1, 0, resource.arrayLayers));

    vk::PipelineStageFlags srcStage;
    vk::PipelineStageFlags dstStage = vk::PipelineStageFlagBits::eFragmentShader;

    if(bIsDepth)
    {
        imageBarrier
            .setSrcAccessMask(vk::AccessFlagBits::eDepthStencilAttachmentWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
            .setOldLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
            .setNewLayout(vk::ImageLayout::eDepthStencilReadOnlyOptimal);

        srcStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    }
    else
    {
        imageBarrier
            .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
            .setDstAccessMask(vk::AccessFlagBits::eShaderRead)
            .setOldLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal);

        srcStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    }

    commandBuffer.pipelineBarrier(
        srcStage,
        dstStage,
        vk::DependencyFlagBits::eByRegion,
        0, nullptr,
        0, nullptr,
        1, &imageBarrier);
}

void PassBarrier::TransitionShaderReadToAttachment(
    vk::CommandBuffer& commandBuffer,
    const RenderGraph& renderGraph,
    const std::string& resourceName,
    uint32_t swapChainImageIndex)
{
    const auto& resolveMap = renderGraph.GetResourcesResolve();
    auto resourceIt = resolveMap.find(resourceName);
    if(resourceIt == resolveMap.end())
    {
        return;
    }

    const auto& resource = resourceIt->second[swapChainImageIndex];
    const bool bIsDepth = CommonFunction::IsDepthFormat(resource.format);

    vk::ImageAspectFlags aspectMask = bIsDepth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    if(bIsDepth && CommonFunction::HasStencilComponent(resource.format))
    {
        aspectMask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageMemoryBarrier imageBarrier;
    imageBarrier
        .setImage(resource.image)
        .setSubresourceRange(vk::ImageSubresourceRange(aspectMask, 0, 1, 0, resource.arrayLayers))
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
        .setOldLayout(bIsDepth ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal)
        .setNewLayout(bIsDepth ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eColorAttachmentOptimal);

    vk::PipelineStageFlags dstStage;
    if(bIsDepth)
    {
        imageBarrier.setDstAccessMask(vk::AccessFlagBits::eDepthStencilAttachmentRead | vk::AccessFlagBits::eDepthStencilAttachmentWrite);
        dstStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    }
    else
    {
        imageBarrier.setDstAccessMask(vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite);
        dstStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    }

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eFragmentShader,
        dstStage,
        vk::DependencyFlagBits::eByRegion,
        0, nullptr,
        0, nullptr,
        1, &imageBarrier);
}

#include "passBarrier.h"

#include <algorithm>
#include "commonFunction.h"
#include "renderGraph.h"

bool PassBarrier::ContainsResource(const std::vector<std::string>& resources, const std::string& resourceName)
{
    return std::find(resources.begin(), resources.end(), resourceName) != resources.end();
}

PassBarrier::ResourcePreviousUse PassBarrier::FindPreviousResourceUse(
    RenderGraph& renderGraph,
    const std::vector<std::string>& renderPassOrdered,
    size_t currentPassIndex,
    const std::string& resourceName)
{
    for(size_t previousIndex = currentPassIndex; previousIndex > 0; --previousIndex)
    {
        const auto& previousPass = renderGraph.GetRenderpasses().at(renderPassOrdered[previousIndex - 1]);
        if(ContainsResource(previousPass.outputResources, resourceName))
        {
            return ResourcePreviousUse::Attachment;
        }
        if(ContainsResource(previousPass.inputResources, resourceName))
        {
            return ResourcePreviousUse::Sampled;
        }
    }
    return ResourcePreviousUse::None;
}

void PassBarrier::PrepareForPass(
    vk::CommandBuffer& commandBuffer,
    RenderGraph& renderGraph,
    size_t passIndex,
    uint32_t swapChainImageIndex)
{
    const auto& renderPassOrdered = renderGraph.GetRenderpassesOrdered();

    PrepareInputResources(commandBuffer, renderGraph, renderPassOrdered, passIndex, swapChainImageIndex);
    PrepareLoadOutputResources(commandBuffer, renderGraph, renderPassOrdered, passIndex, swapChainImageIndex);
}

void PassBarrier::PrepareInputResources(
    vk::CommandBuffer& commandBuffer,
    RenderGraph& renderGraph,
    const std::vector<std::string>& renderPassOrdered,
    size_t passIndex,
    uint32_t swapChainImageIndex)
{
    const auto& renderPass = renderGraph.GetRenderpasses().at(renderPassOrdered[passIndex]);

    for(const auto& resourceName : renderPass.inputResources)
    {
        if(FindPreviousResourceUse(renderGraph, renderPassOrdered, passIndex, resourceName) != ResourcePreviousUse::Attachment)
        {
            continue;
        }

        // 当前 pass 会把这个资源当 texture 采样，因此需要在 beginRenderPass 前保证它处于 shader-read layout。
        // 这里转换的是 Vulkan image layout，不是 R16G16B16A16 / D32 这类像素 format。
        TransitionAttachmentToShaderRead(commandBuffer, renderGraph, resourceName, swapChainImageIndex);
    }
}

void PassBarrier::PrepareLoadOutputResources(
    vk::CommandBuffer& commandBuffer,
    RenderGraph& renderGraph,
    const std::vector<std::string>& renderPassOrdered,
    size_t passIndex,
    uint32_t swapChainImageIndex)
{
    const auto& renderPass = renderGraph.GetRenderpasses().at(renderPassOrdered[passIndex]);

    for(const auto& resourceName : renderPass.outputResources)
    {
        if(resourceName == "swapChain")
        {
            continue;
        }

        auto loadOpIt = renderPass.outputLoadOps.find(resourceName);
        if(loadOpIt == renderPass.outputLoadOps.end() ||
           loadOpIt->second != vk::AttachmentLoadOp::eLoad ||
           FindPreviousResourceUse(renderGraph, renderPassOrdered, passIndex, resourceName) != ResourcePreviousUse::Sampled)
        {
            continue;
        }

        // Render pass 的 attachment initialLayout 不能凭空从 shader-read 变回 attachment layout。
        // 如果资源刚被某个 pass 采样过，并且当前 pass 又要 load 它继续写/测，就需要在 beginRenderPass 前显式转回。
        TransitionShaderReadToAttachment(commandBuffer, renderGraph, resourceName, swapChainImageIndex);
    }
}

void PassBarrier::TransitionAttachmentToShaderRead(
    vk::CommandBuffer& commandBuffer,
    RenderGraph& renderGraph,
    const std::string& resourceName,
    uint32_t swapChainImageIndex)
{
    auto& resolveMap = renderGraph.GetResourcesResolve();
    auto resourceIt = resolveMap.find(resourceName);
    if(resourceIt == resolveMap.end())
    {
        return;
    }

    auto& resource = resourceIt->second[swapChainImageIndex];
    const bool bIsDepth = CommonFunction::IsDepthFormat(resource.format);

    vk::ImageAspectFlags aspectMask = bIsDepth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    if(bIsDepth && CommonFunction::HasStencilComponent(resource.format))
    {
        aspectMask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageMemoryBarrier imageBarrier;
    imageBarrier
        .setImage(resource.image)
        .setSubresourceRange(vk::ImageSubresourceRange(aspectMask, 0, 1, 0, 1));

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
    RenderGraph& renderGraph,
    const std::string& resourceName,
    uint32_t swapChainImageIndex)
{
    auto& resolveMap = renderGraph.GetResourcesResolve();
    auto resourceIt = resolveMap.find(resourceName);
    if(resourceIt == resolveMap.end())
    {
        return;
    }

    auto& resource = resourceIt->second[swapChainImageIndex];
    const bool bIsDepth = CommonFunction::IsDepthFormat(resource.format);

    vk::ImageAspectFlags aspectMask = bIsDepth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    if(bIsDepth && CommonFunction::HasStencilComponent(resource.format))
    {
        aspectMask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageMemoryBarrier imageBarrier;
    imageBarrier
        .setImage(resource.image)
        .setSubresourceRange(vk::ImageSubresourceRange(aspectMask, 0, 1, 0, 1))
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

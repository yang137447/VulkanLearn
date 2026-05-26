#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

class RenderGraph;

class PassBarrier
{
public:
    // PassBarrier 只处理 render graph pass 边界上的 image layout / visibility。
    // RenderSystem 负责“画什么”，RenderGraph 负责“资源声明”，这里负责把 input/output 关系翻译成 Vulkan barrier。
    // 注意：这里转换的是 Vulkan image layout，不是 RenderResource 的像素 format。
    static void PrepareForPass(
        vk::CommandBuffer& commandBuffer,
        RenderGraph& renderGraph,
        size_t passIndex,
        uint32_t swapChainImageIndex);

private:
    enum class ResourcePreviousUse
    {
        None,
        Sampled,
        Attachment
    };

    static bool ContainsResource(const std::vector<std::string>& resources, const std::string& resourceName);
    static ResourcePreviousUse FindPreviousResourceUse(RenderGraph& renderGraph, const std::vector<std::string>& renderPassOrdered, size_t currentPassIndex, const std::string& resourceName);
    static void PrepareInputResources(
        vk::CommandBuffer& commandBuffer,
        RenderGraph& renderGraph,
        const std::vector<std::string>& renderPassOrdered,
        size_t passIndex,
        uint32_t swapChainImageIndex);
    static void PrepareLoadOutputResources(
        vk::CommandBuffer& commandBuffer,
        RenderGraph& renderGraph,
        const std::vector<std::string>& renderPassOrdered,
        size_t passIndex,
        uint32_t swapChainImageIndex);
    static void TransitionAttachmentToShaderRead(
        vk::CommandBuffer& commandBuffer,
        RenderGraph& renderGraph,
        const std::string& resourceName,
        uint32_t swapChainImageIndex);
    static void TransitionShaderReadToAttachment(
        vk::CommandBuffer& commandBuffer,
        RenderGraph& renderGraph,
        const std::string& resourceName,
        uint32_t swapChainImageIndex);
};

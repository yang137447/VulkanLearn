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
    // PassBarrier executes the compiled graph's pass-boundary image layout
    // plan. RenderGraphCompiler decides which resources need transitions;
    // this class only maps those planned transitions to Vulkan barriers.
    static void PrepareForPass(
        vk::CommandBuffer& commandBuffer,
        const RenderGraph& renderGraph,
        size_t passIndex,
        uint32_t swapChainImageIndex);

private:
    static void ApplyCompiledBarrierPlan(
        vk::CommandBuffer& commandBuffer,
        const RenderGraph& renderGraph,
        size_t passIndex,
        uint32_t swapChainImageIndex);
    static void TransitionAttachmentToShaderRead(
        vk::CommandBuffer& commandBuffer,
        const RenderGraph& renderGraph,
        const std::string& resourceName,
        uint32_t swapChainImageIndex);
    static void TransitionShaderReadToAttachment(
        vk::CommandBuffer& commandBuffer,
        const RenderGraph& renderGraph,
        const std::string& resourceName,
        uint32_t swapChainImageIndex);
};

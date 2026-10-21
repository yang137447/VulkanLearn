#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

class RenderGraph;
struct RenderResource;

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
    // 图执行完后的临时只读消费者；归还时恢复图末布局，不改变下一帧编译计划。
    static const RenderResource* BeginSceneDepthRead(vk::CommandBuffer& commandBuffer,
        const RenderGraph& renderGraph, uint32_t imageIndex);
    static void EndSceneDepthRead(vk::CommandBuffer& commandBuffer,
        const RenderGraph& renderGraph, uint32_t imageIndex);

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
    static void TransitionStorage(
        vk::CommandBuffer& commandBuffer, const RenderGraph& renderGraph,
        const std::string& resourceName, uint32_t imageIndex, bool toStorage);
};

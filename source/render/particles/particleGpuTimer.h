#pragma once

// File responsibility: Per-swapchain-image GPU timestamp timing for the particle
// graphics pass. Mirrors EnvironmentGpuTimer's query layout, but boundary errors
// and backend exceptions disable timing instead of escaping into the frame loop.

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/particles/particleGpuTiming.h"

namespace VL
{

class RendererBackendVulkan;

class ParticleGpuTimer
{
public:
    void Initialize(RendererBackendVulkan& rendererBackend);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    // BeginFrame 先回收该 image 上一轮已完成的结果，再 reset 本帧的两个 query。
    // 调用点必须在等待该 image fence 之后、录制 particle pass 之前。
    void BeginFrame(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex);
    void BeginPass(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex);
    void EndPass(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex);

    const ParticleGpuTimingSnapshot& GetSnapshot() const noexcept
    {
        return accumulator.GetSnapshot();
    }
    bool IsSupported() const noexcept
    {
        return accumulator.GetSnapshot().supported;
    }

private:
    static constexpr uint32_t kQueriesPerImage = 2;

    struct FrameQueryState
    {
        bool used = false;
        bool open = false;
    };

    uint32_t GetQueryIndex(
        uint32_t swapchainImageIndex,
        bool endQuery) const;
    void CollectFrameResults(uint32_t swapchainImageIndex);
    void Disable(const std::string& reason);

    RendererBackendVulkan* rendererBackend = nullptr;
    vk::QueryPool queryPool;
    std::vector<FrameQueryState> frameQueryStates;
    ParticleGpuTimingAccumulator accumulator;
    float timestampPeriodNanoseconds = 0.0f;
    uint32_t timestampValidBits = 0;
    bool initialized = false;
    bool disabledLogged = false;
};

} // namespace VL

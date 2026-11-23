#pragma once

// File responsibility: Per-swapchain-image GPU timestamp timing for one particle
// graphics interval (the whole pass or one emitter/global-pool draw). Mirrors
// EnvironmentGpuTimer's query layout, but boundary errors and backend exceptions
// disable timing instead of escaping into the frame loop.

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/backend/gpuTimestampQueryBatch.h"
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

    // 帧命令成功提交 / 被丢弃的显式边界：被丢弃的帧不能在下一次复用 image
    // 时读回（那些 query 从未提交，读取会得到 VK_NOT_READY）。
    void FrameSubmitted() noexcept;
    void AbortFrame() noexcept;

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
        // 每个 image 一对 query；batch 保证只有提交过的完整 pair 才被读回。
        GpuTimestampQueryBatch batch{ 1 };
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
    // 本帧 BeginFrame 选中的 image 只在录制期有效；提交/废弃后清空。
    uint32_t recordingImageIndex = 0;
    bool hasRecordingFrame = false;
};

} // namespace VL

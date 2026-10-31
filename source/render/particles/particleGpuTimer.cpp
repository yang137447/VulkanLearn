#include "render/particles/particleGpuTimer.h"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>

#include "core/logStore.h"
#include "render/backend/rendererBackendVulkan.h"

namespace VL
{

void ParticleGpuTimer::Initialize(RendererBackendVulkan& rendererBackendIn)
{
    if (initialized)
    {
        return;
    }

    rendererBackend = &rendererBackendIn;
    timestampPeriodNanoseconds =
        rendererBackendIn.GetTimestampPeriodNanoseconds();
    timestampValidBits = rendererBackendIn.GetGraphicsTimestampValidBits();
    const uint32_t swapchainImageCount =
        rendererBackendIn.GetSwapchainImageCount();
    frameQueryStates.assign(
        std::max(swapchainImageCount, 1u),
        FrameQueryState{});
    disabledLogged = false;

    const bool supported =
        timestampPeriodNanoseconds > 0.0f && timestampValidBits > 0;
    accumulator.Reset(supported);
    initialized = true;

    if (!supported)
    {
        // 部分后端没有 graphics timestamp；保持 supported=false 即可，
        // 不写 query、不影响渲染。
        Disable("backend does not expose graphics timestamps");
        return;
    }

    try
    {
        queryPool = rendererBackendIn.CreateTimestampQueryPool(
            swapchainImageCount * kQueriesPerImage,
            "ParticleGpuTimer");
    }
    catch (const std::exception& exception)
    {
        Disable(
            std::string("query pool creation failed: ") +
            exception.what());
    }
}

void ParticleGpuTimer::Shutdown(RendererBackendVulkan& rendererBackendIn)
{
    if (!initialized)
    {
        return;
    }

    if (queryPool && rendererBackend != nullptr)
    {
        try
        {
            rendererBackend->DestroyQueryPool(queryPool);
        }
        catch (const std::exception&)
        {
            // Shutdown 不能把后端拆销毁异常带进渲染线程退出路径。
        }
    }
    queryPool = vk::QueryPool();
    frameQueryStates.clear();
    accumulator.Reset(false);
    timestampPeriodNanoseconds = 0.0f;
    timestampValidBits = 0;
    rendererBackend = nullptr;
    initialized = false;
    disabledLogged = false;
    (void)rendererBackendIn;
}

void ParticleGpuTimer::BeginFrame(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex)
{
    if (!initialized || !IsSupported())
    {
        return;
    }
    if (!queryPool || swapchainImageIndex >= frameQueryStates.size())
    {
        Disable("swapchain image index is invalid");
        return;
    }

    CollectFrameResults(swapchainImageIndex);
    if (!IsSupported())
    {
        return;
    }

    FrameQueryState& frameState = frameQueryStates[swapchainImageIndex];
    frameState.used = false;
    frameState.open = false;

    try
    {
        commandBuffer.resetQueryPool(
            queryPool,
            swapchainImageIndex * kQueriesPerImage,
            kQueriesPerImage);
    }
    catch (const std::exception& exception)
    {
        Disable(
            std::string("query pool reset failed: ") +
            exception.what());
    }
}

void ParticleGpuTimer::BeginPass(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex)
{
    if (!initialized || !IsSupported())
    {
        return;
    }
    if (!queryPool || swapchainImageIndex >= frameQueryStates.size())
    {
        Disable("swapchain image index is invalid");
        return;
    }

    FrameQueryState& frameState = frameQueryStates[swapchainImageIndex];
    if (frameState.used)
    {
        // 每帧只计一次 particle pass；重复录制保留首个区间，
        // 避免同一对 query 被后写的 end 覆盖。
        return;
    }

    frameState.used = true;
    frameState.open = true;
    try
    {
        commandBuffer.writeTimestamp(
            vk::PipelineStageFlagBits::eTopOfPipe,
            queryPool,
            GetQueryIndex(swapchainImageIndex, false));
    }
    catch (const std::exception& exception)
    {
        frameState.used = false;
        frameState.open = false;
        Disable(
            std::string("begin timestamp failed: ") +
            exception.what());
    }
}

void ParticleGpuTimer::EndPass(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex)
{
    if (!initialized || !IsSupported())
    {
        return;
    }
    if (!queryPool || swapchainImageIndex >= frameQueryStates.size())
    {
        Disable("swapchain image index is invalid");
        return;
    }

    FrameQueryState& frameState = frameQueryStates[swapchainImageIndex];
    if (!frameState.open)
    {
        return;
    }

    try
    {
        commandBuffer.writeTimestamp(
            vk::PipelineStageFlagBits::eBottomOfPipe,
            queryPool,
            GetQueryIndex(swapchainImageIndex, true));
        frameState.open = false;
    }
    catch (const std::exception& exception)
    {
        frameState.used = false;
        frameState.open = false;
        Disable(
            std::string("end timestamp failed: ") +
            exception.what());
    }
}

uint32_t ParticleGpuTimer::GetQueryIndex(
    uint32_t swapchainImageIndex,
    bool endQuery) const
{
    return swapchainImageIndex * kQueriesPerImage + (endQuery ? 1u : 0u);
}

void ParticleGpuTimer::CollectFrameResults(uint32_t swapchainImageIndex)
{
    if (rendererBackend == nullptr)
    {
        return;
    }

    const FrameQueryState& frameState =
        frameQueryStates[swapchainImageIndex];
    // 只有 begin/end 都写入过的轮次才有完整 timestamp pair。
    if (!frameState.used || frameState.open)
    {
        return;
    }

    std::array<uint64_t, 2> timestamps{};
    try
    {
        rendererBackend->ReadTimestampQueryPair(
            queryPool,
            GetQueryIndex(swapchainImageIndex, false),
            timestamps);
    }
    catch (const std::exception& exception)
    {
        Disable(
            std::string("timestamp readback failed: ") +
            exception.what());
        return;
    }

    accumulator.Record(ParticleGpuTimestampDeltaMilliseconds(
        timestamps[0],
        timestamps[1],
        timestampValidBits,
        timestampPeriodNanoseconds));
}

void ParticleGpuTimer::Disable(const std::string& reason)
{
    accumulator.Disable();
    if (disabledLogged)
    {
        return;
    }
    disabledLogged = true;
    const std::string message =
        "Particle GPU timing disabled: " + reason;
    ProcessLog().Append(
        LogSeverity::Warning,
        "Particles",
        message);
    std::cout << "[Particles][Warning] " << message << std::endl;
}

} // namespace VL

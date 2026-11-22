#include "render/environment/environmentGpuTimer.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

#include "profiler.h"
#include "render/backend/rendererBackendVulkan.h"

namespace VL
{

void EnvironmentGpuTimer::Initialize(RendererBackendVulkan& rendererBackend)
{
    if (initialized)
    {
        return;
    }

    this->rendererBackend = &rendererBackend;
    timestampPeriodNanoseconds = rendererBackend.GetTimestampPeriodNanoseconds();
    timestampValidBits = rendererBackend.GetGraphicsTimestampValidBits();
    const uint32_t swapchainImageCount = rendererBackend.GetSwapchainImageCount();
    frameQueryStates.resize(swapchainImageCount);
    hasRecordingFrame = false;
    recordingImageIndex = 0;

    timingSnapshot = EnvironmentGpuTimingSnapshot{};
    timingSnapshot.supported = timestampPeriodNanoseconds > 0.0f && timestampValidBits > 0;
    if (timingSnapshot.supported)
    {
        queryPool = rendererBackend.CreateTimestampQueryPool(
            swapchainImageCount * kQueriesPerImage,
            "EnvironmentGpuTimer");
    }

    initialized = true;
}

void EnvironmentGpuTimer::Shutdown(RendererBackendVulkan& rendererBackend)
{
    if (!initialized)
    {
        return;
    }

    if (queryPool)
    {
        rendererBackend.DestroyQueryPool(queryPool);
    }
    frameQueryStates.clear();
    timingSnapshot = EnvironmentGpuTimingSnapshot{};
    timestampPeriodNanoseconds = 0.0f;
    timestampValidBits = 0;
    hasRecordingFrame = false;
    recordingImageIndex = 0;
    this->rendererBackend = nullptr;
    initialized = false;
}

void EnvironmentGpuTimer::BeginFrame(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex)
{
    hasRecordingFrame = false;
    if (!initialized || !timingSnapshot.supported)
    {
        return;
    }
    if (swapchainImageIndex >= frameQueryStates.size())
    {
        throw std::runtime_error("Environment GPU timer swapchain image index is invalid.");
    }

    FrameQueryState& frameState = frameQueryStates[swapchainImageIndex];
    // 先读回上一轮真正提交过的完整批次；被废弃帧的批次已在 AbortFrame 里作废。
    if (frameState.batch.HasSubmitted())
    {
        CollectFrameResults(swapchainImageIndex);
        frameState.batch.Consumed();
    }
    frameState.batch.BeginRecording();
    recordingImageIndex = swapchainImageIndex;
    hasRecordingFrame = true;

    const uint32_t firstQuery = swapchainImageIndex * kQueriesPerImage;
    commandBuffer.resetQueryPool(queryPool, firstQuery, kQueriesPerImage);
}

void EnvironmentGpuTimer::BeginProduct(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex,
    EnvironmentGpuProduct product)
{
    if (!initialized || !timingSnapshot.supported)
    {
        return;
    }

    const uint32_t productIndex = static_cast<uint32_t>(product);
    if (swapchainImageIndex >= frameQueryStates.size() || productIndex >= kProductCount)
    {
        throw std::runtime_error("Environment GPU timer product index is invalid.");
    }

    FrameQueryState& frameState = frameQueryStates[swapchainImageIndex];
    // 同一 product 每帧只允许一次 begin；重复会覆盖同一对 query。
    if (!frameState.batch.Begin(productIndex))
    {
        throw std::runtime_error("Environment GPU product can only be timed once per frame.");
    }

    commandBuffer.writeTimestamp(
        vk::PipelineStageFlagBits::eTopOfPipe,
        queryPool,
        GetQueryIndex(swapchainImageIndex, product, false));
}

void EnvironmentGpuTimer::EndProduct(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex,
    EnvironmentGpuProduct product)
{
    if (!initialized || !timingSnapshot.supported)
    {
        return;
    }

    const uint32_t productIndex = static_cast<uint32_t>(product);
    if (swapchainImageIndex >= frameQueryStates.size() ||
        productIndex >= kProductCount ||
        !frameQueryStates[swapchainImageIndex].batch.IsOpen(productIndex))
    {
        throw std::runtime_error("Environment GPU timer product was not opened.");
    }

    commandBuffer.writeTimestamp(
        vk::PipelineStageFlagBits::eBottomOfPipe,
        queryPool,
        GetQueryIndex(swapchainImageIndex, product, true));
    frameQueryStates[swapchainImageIndex].batch.End(productIndex);
}

void EnvironmentGpuTimer::FrameSubmitted() noexcept
{
    if (hasRecordingFrame && recordingImageIndex < frameQueryStates.size())
        frameQueryStates[recordingImageIndex].batch.Submitted();
    hasRecordingFrame = false;
}

void EnvironmentGpuTimer::AbortFrame() noexcept
{
    // 本帧命令已被丢弃：query 从未在 GPU 上执行，禁止下一次复用该 image 时读回。
    if (hasRecordingFrame && recordingImageIndex < frameQueryStates.size())
        frameQueryStates[recordingImageIndex].batch.Aborted();
    hasRecordingFrame = false;
}

uint32_t EnvironmentGpuTimer::GetQueryIndex(
    uint32_t swapchainImageIndex,
    EnvironmentGpuProduct product,
    bool endQuery) const
{
    return swapchainImageIndex * kQueriesPerImage +
        static_cast<uint32_t>(product) * kQueriesPerProduct +
        (endQuery ? 1u : 0u);
}

void EnvironmentGpuTimer::CollectFrameResults(uint32_t swapchainImageIndex)
{
    if (rendererBackend == nullptr)
    {
        return;
    }

    const FrameQueryState& frameState = frameQueryStates[swapchainImageIndex];
    if (!frameState.batch.HasSubmitted())
    {
        return;
    }

    for (uint32_t productIndex = 0; productIndex < kProductCount; ++productIndex)
    {
        if (!frameState.batch.WasRecorded(productIndex))
        {
            continue;
        }

        const EnvironmentGpuProduct product =
            static_cast<EnvironmentGpuProduct>(productIndex);
        std::array<uint64_t, 2> timestamps{};
        rendererBackend->ReadTimestampQueryPair(
            queryPool,
            GetQueryIndex(swapchainImageIndex, product, false),
            timestamps);

        uint64_t timestampMask = std::numeric_limits<uint64_t>::max();
        if (timestampValidBits < 64)
        {
            timestampMask = (uint64_t{ 1 } << timestampValidBits) - 1;
        }
        const uint64_t elapsedTicks =
            (timestamps[1] - timestamps[0]) & timestampMask;
        const double milliseconds =
            static_cast<double>(elapsedTicks) *
            static_cast<double>(timestampPeriodNanoseconds) /
            1000000.0;
        RecordTimingSample(product, milliseconds);
    }
}

void EnvironmentGpuTimer::RecordTimingSample(
    EnvironmentGpuProduct product,
    double milliseconds)
{
    EnvironmentGpuProductTiming& timing =
        timingSnapshot.products[static_cast<size_t>(product)];
    timing.sampleCount++;
    timing.lastMilliseconds = milliseconds;
    timing.maxMilliseconds = std::max(timing.maxMilliseconds, milliseconds);
    timing.averageMilliseconds +=
        (milliseconds - timing.averageMilliseconds) /
        static_cast<double>(timing.sampleCount);

    switch (product)
    {
    case EnvironmentGpuProduct::Cubemap:
        PROFILE_GPU_VALUE("Environment.CubemapGpuMs", milliseconds);
        break;
    case EnvironmentGpuProduct::SphericalHarmonics:
        PROFILE_GPU_VALUE("Environment.ShGpuMs", milliseconds);
        break;
    case EnvironmentGpuProduct::Prefilter:
        PROFILE_GPU_VALUE("Environment.PrefilterGpuMs", milliseconds);
        break;
    case EnvironmentGpuProduct::Commit:
        PROFILE_GPU_VALUE("Environment.CommitGpuMs", milliseconds);
        break;
    case EnvironmentGpuProduct::Count:
        break;
    }
}

const char* ToString(EnvironmentGpuProduct product)
{
    switch (product)
    {
    case EnvironmentGpuProduct::Cubemap:
        return "Cubemap";
    case EnvironmentGpuProduct::SphericalHarmonics:
        return "SH";
    case EnvironmentGpuProduct::Prefilter:
        return "Prefilter";
    case EnvironmentGpuProduct::Commit:
        return "Commit";
    case EnvironmentGpuProduct::Count:
        break;
    }
    return "Unknown";
}

} // namespace VL

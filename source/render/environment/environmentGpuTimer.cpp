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
    this->rendererBackend = nullptr;
    initialized = false;
}

void EnvironmentGpuTimer::BeginFrame(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex)
{
    if (!initialized || !timingSnapshot.supported)
    {
        return;
    }
    if (swapchainImageIndex >= frameQueryStates.size())
    {
        throw std::runtime_error("Environment GPU timer swapchain image index is invalid.");
    }

    CollectFrameResults(swapchainImageIndex);
    FrameQueryState& frameState = frameQueryStates[swapchainImageIndex];
    frameState.used.fill(false);
    frameState.open.fill(false);

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
    if (frameState.used[productIndex] || frameState.open[productIndex])
    {
        throw std::runtime_error("Environment GPU product can only be timed once per frame.");
    }

    frameState.used[productIndex] = true;
    frameState.open[productIndex] = true;
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
        !frameQueryStates[swapchainImageIndex].open[productIndex])
    {
        throw std::runtime_error("Environment GPU timer product was not opened.");
    }

    commandBuffer.writeTimestamp(
        vk::PipelineStageFlagBits::eBottomOfPipe,
        queryPool,
        GetQueryIndex(swapchainImageIndex, product, true));
    frameQueryStates[swapchainImageIndex].open[productIndex] = false;
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
    for (uint32_t productIndex = 0; productIndex < kProductCount; ++productIndex)
    {
        if (!frameState.used[productIndex])
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

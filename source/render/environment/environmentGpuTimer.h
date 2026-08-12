#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.hpp>

namespace VL
{

class RendererBackendVulkan;

enum class EnvironmentGpuProduct : uint32_t
{
    Cubemap,
    SphericalHarmonics,
    Prefilter,
    Commit,
    Count
};

struct EnvironmentGpuProductTiming
{
    double lastMilliseconds = 0.0;
    double averageMilliseconds = 0.0;
    double maxMilliseconds = 0.0;
    uint64_t sampleCount = 0;
};

struct EnvironmentGpuTimingSnapshot
{
    bool supported = false;
    std::array<EnvironmentGpuProductTiming,
        static_cast<size_t>(EnvironmentGpuProduct::Count)> products{};

    const EnvironmentGpuProductTiming& Get(EnvironmentGpuProduct product) const
    {
        return products[static_cast<size_t>(product)];
    }
};

// 每个 swapchain image 独占一组 query。BeginFrame 已等待该 image 的 fence，
// 因此可以无阻塞读取上次结果，再在当前命令缓冲里 reset 并复用同一组槽位。
class EnvironmentGpuTimer
{
public:
    void Initialize(RendererBackendVulkan& rendererBackend);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    void BeginFrame(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex);
    void BeginProduct(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex,
        EnvironmentGpuProduct product);
    void EndProduct(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex,
        EnvironmentGpuProduct product);

    EnvironmentGpuTimingSnapshot GetSnapshot() const { return timingSnapshot; }
    bool IsSupported() const { return timingSnapshot.supported; }

private:
    static constexpr uint32_t kProductCount =
        static_cast<uint32_t>(EnvironmentGpuProduct::Count);
    static constexpr uint32_t kQueriesPerProduct = 2;
    static constexpr uint32_t kQueriesPerImage = kProductCount * kQueriesPerProduct;

    struct FrameQueryState
    {
        std::array<bool, kProductCount> used{};
        std::array<bool, kProductCount> open{};
    };

    uint32_t GetQueryIndex(
        uint32_t swapchainImageIndex,
        EnvironmentGpuProduct product,
        bool endQuery) const;
    void CollectFrameResults(uint32_t swapchainImageIndex);
    void RecordTimingSample(EnvironmentGpuProduct product, double milliseconds);

    RendererBackendVulkan* rendererBackend = nullptr;
    vk::QueryPool queryPool;
    std::vector<FrameQueryState> frameQueryStates;
    EnvironmentGpuTimingSnapshot timingSnapshot;
    float timestampPeriodNanoseconds = 0.0f;
    uint32_t timestampValidBits = 0;
    bool initialized = false;
};

const char* ToString(EnvironmentGpuProduct product);

} // namespace VL

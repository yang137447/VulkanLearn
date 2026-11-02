#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "vertexDataStruct.h"

class PipelineFactory;
class RenderableObject;
class ComputePipeline;

namespace VL
{

class RendererBackendVulkan;

// 一次性把描边 section 的顶点法线焊接平滑：weld -> accumulate -> apply 三次 compute。
// 只处理资产显式声明 smoothNormalsSections 的 section；本体 section 的顶点缓冲
// 不会被触碰，因此硬表面着色保持原样。
//
// 生命周期：世界加载期 Enqueue（GT），渲染线程在 BeginFrame 之后、图形 pass 之前
// 录制 compute。每个 swapchain image 独占 scratch/descriptor pool，BeginFrame 已等待
// 该 image 的 fence，所以复用前不需要全局 waitIdle。
class OutlineNormalSmoother
{
public:
    static OutlineNormalSmoother& GetInstance();

    void Initialize(PipelineFactory& factory, RendererBackendVulkan& backend);
    void Shutdown();

    void Enqueue(const std::shared_ptr<RenderableObject>& renderable);

    // 每帧调用；没有待处理 job 时只回收上一帧该 image 的 GPU 计时。
    void RecordPending(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex);

    bool IsInitialized() const noexcept { return initialized; }

private:
    OutlineNormalSmoother() = default;
    ~OutlineNormalSmoother();

    struct JobStats
    {
        uint32_t vertexCount = 0;
        uint32_t triangleCount = 0;
        uint32_t hashSlots = 0;
        uint32_t uniquePositions = 0;
        uint32_t occupiedSlots = 0;
        float hashOccupancy = 0.0f;
        uint32_t weldGroups = 0;
        uint32_t accumulateGroups = 0;
        uint32_t applyGroups = 0;
    };

    struct DebugSource
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };

    struct Job
    {
        std::weak_ptr<RenderableObject> target;
        vk::Buffer vertexBuffer;
        vk::Buffer indexBuffer;
        vk::DeviceSize vertexBytes = 0;
        vk::DeviceSize indexBytes = 0;
        uint32_t strideFloats = 0;
        JobStats stats;
        std::shared_ptr<const DebugSource> debugSource;
    };

    struct ScratchBuffer
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        void* mapped = nullptr;
        vk::DeviceSize size = 0;
    };

    struct ImageScratch
    {
        ScratchBuffer hash;
        ScratchBuffer rep;
        ScratchBuffer accum;
        ScratchBuffer params;
        vk::DescriptorPool descriptorPool;
        std::array<std::vector<vk::DescriptorSet>, 3> sets;
        bool timingPending = false;
        uint32_t timingJobs = 0;
        ScratchBuffer debugReadback;
        std::shared_ptr<const DebugSource> debugSource;
        bool debugReadbackPending = false;
    };

    static JobStats BuildStats(const RenderableObject& renderable);
    static uint32_t NextPowerOfTwo(uint32_t value);

    bool EnsureScratchCapacity(ImageScratch& scratch, uint32_t hashSize,
        uint32_t vertexCount, uint32_t jobCount, const std::string& label);
    void EnsureBuffer(ScratchBuffer& buffer, vk::DeviceSize requiredSize,
        vk::BufferUsageFlags usage, vk::MemoryPropertyFlags memory,
        const std::string& debugName, bool mapped);
    bool EnsureDescriptorPool(ImageScratch& scratch, uint32_t setCount);
    void UpdateJobDescriptors(ImageScratch& scratch, const std::vector<Job>& jobs);
    void RecordJob(vk::CommandBuffer commandBuffer, const Job& job,
        ImageScratch& scratch, uint32_t jobIndex);
    void CollectDebugReadback(uint32_t swapchainImageIndex);
    static std::vector<Eigen::Vector3f> BuildCpuSmoothedNormals(
        const DebugSource& source);
    void DispatchStage(vk::CommandBuffer commandBuffer, size_t stage,
        uint32_t groups, ImageScratch& scratch, uint32_t jobIndex);
    void CollectTiming(uint32_t swapchainImageIndex);
    void DestroyBuffer(ScratchBuffer& buffer);
    void DestroyScratch(ImageScratch& scratch);

    mutable std::mutex jobMutex;
    std::vector<Job> pendingJobs;
    RendererBackendVulkan* backend = nullptr;
    std::array<std::shared_ptr<ComputePipeline>, 3> pipelines;
    std::vector<ImageScratch> imageScratch;
    vk::QueryPool timestampPool;
    float timestampPeriodNanoseconds = 0.0f;
    uint32_t timestampValidBits = 0;
    bool timestampSupported = false;
    bool initialized = false;
};

} // namespace VL

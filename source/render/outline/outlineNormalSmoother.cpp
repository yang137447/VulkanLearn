#include "render/outline/outlineNormalSmoother.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "renderableObject.h"
#include "vertexDataStruct.h"

namespace VL
{
namespace
{

constexpr uint32_t kLocalSize = 64;
// std140 结构体按 16 字节对齐，20 字节字段实际占 32 字节。
constexpr uint32_t kParamsBlockSize = 32;
// 256 是 Vulkan 规范允许的 minUniformBufferOffsetAlignment 上限，直接按 256 对齐
// 可以避免查询设备 limit；job 数量很少，这点空洞可以接受。
constexpr uint32_t kParamsStride = 256;
constexpr float kQuantScale = 1000.0f;

struct SmoothParams
{
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t strideFloats;
    uint32_t hashSize;
    float quantScale;
};

int32_t QuantizePositionComponent(float value)
{
    const double scaled = static_cast<double>(value) * kQuantScale;
    if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
        return std::numeric_limits<int32_t>::min();
    if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(std::llround(scaled));
}

uint32_t OutlineKeyCpu(int32_t x, int32_t y, int32_t z)
{
    const uint32_t hash = static_cast<uint32_t>(x) * 73856093u ^
        static_cast<uint32_t>(y) * 19349663u ^
        static_cast<uint32_t>(z) * 83492791u;
    return hash == 0u ? 1u : hash;
}

uint32_t DispatchGroups(uint32_t count)
{
    return (count + kLocalSize - 1u) / kLocalSize;
}

} // namespace

OutlineNormalSmoother& OutlineNormalSmoother::GetInstance()
{
    static OutlineNormalSmoother instance;
    return instance;
}

OutlineNormalSmoother::~OutlineNormalSmoother()
{
    Shutdown();
}

void OutlineNormalSmoother::Initialize(PipelineFactory& factory,
    RendererBackendVulkan& renderer)
{
    const uint32_t imageCount = renderer.GetSwapchainImageCount();
    if (initialized && backend == &renderer && imageScratch.size() == imageCount)
        return;

    if (initialized)
        Shutdown();

    backend = &renderer;
    try
    {
        pipelines[0] = factory.CreateComputePipelineCandidate("outline/outlineSmoothWeld");
        pipelines[1] = factory.CreateComputePipelineCandidate("outline/outlineSmoothAccumulate");
        pipelines[2] = factory.CreateComputePipelineCandidate("outline/outlineSmoothApply");

        imageScratch.resize(imageCount);
        timestampPeriodNanoseconds = renderer.GetTimestampPeriodNanoseconds();
        timestampValidBits = renderer.GetGraphicsTimestampValidBits();
        timestampSupported = timestampPeriodNanoseconds > 0.0f && timestampValidBits > 0;
        if (timestampSupported && imageCount > 0)
        {
            timestampPool = renderer.CreateTimestampQueryPool(
                imageCount * 2u, "OutlineNormalSmoother");
        }
        initialized = true;
        std::cout << "[OutlineSmooth] initialized stages=3 imageCount=" << imageCount
                  << " timestamp=" << (timestampSupported ? "on" : "off") << std::endl;
    }
    catch (...)
    {
        Shutdown();
        throw;
    }
}

void OutlineNormalSmoother::Shutdown()
{
    if (backend != nullptr)
    {
        for (ImageScratch& scratch : imageScratch)
            DestroyScratch(scratch);
        if (timestampPool)
            backend->DestroyQueryPool(timestampPool);
    }
    imageScratch.clear();
    timestampPool = nullptr;
    pipelines = {};
    backend = nullptr;
    timestampSupported = false;
    timestampPeriodNanoseconds = 0.0f;
    timestampValidBits = 0;
    initialized = false;
}

void OutlineNormalSmoother::Enqueue(
    const std::shared_ptr<RenderableObject>& renderable)
{
    if (!renderable)
        return;

    const auto& vertexInfo = renderable->GetVertexBufferInfo();
    const auto& indexInfo = renderable->GetIndexBufferInfo();
    if (!vertexInfo.buffer || !indexInfo.buffer)
    {
        std::cout << "[OutlineSmooth][Warning] skipped object without GPU buffers: "
                  << renderable->GetName() << std::endl;
        return;
    }

    Job job;
    job.target = renderable;
    job.vertexBuffer = vertexInfo.buffer;
    job.indexBuffer = indexInfo.buffer;
    job.vertexBytes = vertexInfo.range;
    job.indexBytes = indexInfo.range;
    job.strideFloats = renderable->GetVertexStrideFloats();
    job.stats = BuildStats(*renderable);
    if (std::getenv("VL_OUTLINE_DEBUG_READBACK") != nullptr)
    {
        auto source = std::make_shared<DebugSource>();
        source->vertices = renderable->GetSourceVertices();
        source->indices = renderable->GetSourceIndices();
        job.debugSource = std::move(source);
    }

    std::lock_guard<std::mutex> lock(jobMutex);
    pendingJobs.push_back(std::move(job));

    const Job& queued = pendingJobs.back();
    std::cout << "[OutlineSmooth] enqueue " << renderable->GetName()
              << " vertices=" << queued.stats.vertexCount
              << " triangles=" << queued.stats.triangleCount
              << " hashSlots=" << queued.stats.hashSlots
              << " uniquePositions=" << queued.stats.uniquePositions
              << " occupiedSlots=" << queued.stats.occupiedSlots
              << " occupancy=" << (queued.stats.hashOccupancy * 100.0f) << "%"
              << std::endl;
}

void OutlineNormalSmoother::RecordPending(vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex)
{
    if (!initialized || swapchainImageIndex >= imageScratch.size())
        return;

    CollectTiming(swapchainImageIndex);
    CollectDebugReadback(swapchainImageIndex);

    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lock(jobMutex);
        jobs.swap(pendingJobs);
    }
    if (jobs.empty())
        return;

    // 世界在 job 录制前被替换时 weak_ptr 会失效；已完成的 job 也不再重复。
    std::vector<std::shared_ptr<RenderableObject>> targets;
    targets.reserve(jobs.size());
    for (auto it = jobs.begin(); it != jobs.end();)
    {
        std::shared_ptr<RenderableObject> target = it->target.lock();
        if (!target || target->IsOutlineNormalsReady())
        {
            it = jobs.erase(it);
            continue;
        }

        // 缓冲句柄以实际对象为准，避免缓存的 RenderableObject 在入队后重建。
        const auto& vertexInfo = target->GetVertexBufferInfo();
        const auto& indexInfo = target->GetIndexBufferInfo();
        it->vertexBuffer = vertexInfo.buffer;
        it->indexBuffer = indexInfo.buffer;
        it->vertexBytes = vertexInfo.range;
        it->indexBytes = indexInfo.range;
        it->strideFloats = target->GetVertexStrideFloats();
        targets.push_back(std::move(target));
        ++it;
    }
    if (jobs.empty())
        return;

    uint32_t maxHashSize = 0;
    uint32_t maxVertexCount = 0;
    for (const Job& job : jobs)
    {
        maxHashSize = std::max(maxHashSize, job.stats.hashSlots);
        maxVertexCount = std::max(maxVertexCount, job.stats.vertexCount);
    }

    const auto recordStart = std::chrono::steady_clock::now();
    ImageScratch& scratch = imageScratch[swapchainImageIndex];
    EnsureScratchCapacity(scratch, maxHashSize, maxVertexCount,
        static_cast<uint32_t>(jobs.size()), "OutlineNormalSmoother");
    EnsureDescriptorPool(scratch, static_cast<uint32_t>(jobs.size()) * 3u);
    UpdateJobDescriptors(scratch, jobs);

    if (timestampSupported)
    {
        commandBuffer.resetQueryPool(timestampPool, swapchainImageIndex * 2u, 2u);
        commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe,
            timestampPool, swapchainImageIndex * 2u);
    }

    for (size_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex)
        RecordJob(commandBuffer, jobs[jobIndex], scratch,
            static_cast<uint32_t>(jobIndex));

    if (timestampSupported)
    {
        commandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
            timestampPool, swapchainImageIndex * 2u + 1u);
        scratch.timingPending = true;
        scratch.timingJobs = static_cast<uint32_t>(jobs.size());
    }

    // Debug only: copy the compute-written vertex buffer back once. The next use
    // of this swapchain image happens after BeginFrame waited for its fence.
    if (jobs.front().debugSource)
    {
        EnsureBuffer(scratch.debugReadback, jobs.front().vertexBytes,
            vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "OutlineNormalSmoother:DebugReadback", true);
        vk::BufferCopy copyRegion;
        copyRegion.setSrcOffset(0).setDstOffset(0).setSize(jobs.front().vertexBytes);
        commandBuffer.copyBuffer(jobs.front().vertexBuffer,
            scratch.debugReadback.buffer, copyRegion);
        scratch.debugSource = jobs.front().debugSource;
        scratch.debugReadbackPending = true;
    }

    const auto recordEnd = std::chrono::steady_clock::now();
    const double recordMilliseconds =
        std::chrono::duration<double, std::milli>(recordEnd - recordStart).count();

    uint32_t totalVertices = 0;
    uint32_t totalTriangles = 0;
    uint32_t totalWeldGroups = 0;
    uint32_t totalAccumulateGroups = 0;
    uint32_t totalApplyGroups = 0;
    uint64_t totalHashSlots = 0;
    uint64_t totalUniquePositions = 0;
    uint64_t totalOccupiedSlots = 0;
    for (const Job& job : jobs)
    {
        totalVertices += job.stats.vertexCount;
        totalTriangles += job.stats.triangleCount;
        totalWeldGroups += job.stats.weldGroups;
        totalAccumulateGroups += job.stats.accumulateGroups;
        totalApplyGroups += job.stats.applyGroups;
        totalHashSlots += job.stats.hashSlots;
        totalUniquePositions += job.stats.uniquePositions;
        totalOccupiedSlots += job.stats.occupiedSlots;
    }
    const double occupancy = totalHashSlots > 0
        ? static_cast<double>(totalOccupiedSlots) / static_cast<double>(totalHashSlots)
        : 0.0;

    std::cout << "[OutlineSmooth] record image=" << swapchainImageIndex
              << " jobs=" << jobs.size()
              << " dispatches=" << (jobs.size() * 3u)
              << " threads=[weld=" << totalVertices
              << ",accumulate=" << totalTriangles
              << ",apply=" << totalVertices << "]"
              << " groups=[weld=" << totalWeldGroups
              << ",accumulate=" << totalAccumulateGroups
              << ",apply=" << totalApplyGroups << "]"
              << " hashSlots=" << totalHashSlots
              << " uniquePositions=" << totalUniquePositions
              << " occupiedSlots=" << totalOccupiedSlots
              << " occupancy=" << (occupancy * 100.0) << "%"
              << " cpuRecordMs=" << recordMilliseconds
              << std::endl;

    for (const auto& target : targets)
        target->MarkOutlineNormalsReady();
}

OutlineNormalSmoother::JobStats OutlineNormalSmoother::BuildStats(
    const RenderableObject& renderable)
{
    JobStats stats;
    const auto& vertices = renderable.GetSourceVertices();
    stats.vertexCount = static_cast<uint32_t>(vertices.size());
    stats.triangleCount = static_cast<uint32_t>(renderable.GetIndexBufferInfo().range /
        (sizeof(uint32_t) * 3u));
    const uint64_t targetSlots = std::max<uint64_t>(
        1024ull, static_cast<uint64_t>(stats.vertexCount) * 2ull);
    stats.hashSlots = NextPowerOfTwo(static_cast<uint32_t>(std::min<uint64_t>(
        targetSlots, 1ull << 30)));
    stats.weldGroups = DispatchGroups(stats.vertexCount);
    stats.accumulateGroups = DispatchGroups(stats.triangleCount);
    stats.applyGroups = DispatchGroups(stats.vertexCount);

    std::set<std::tuple<int32_t, int32_t, int32_t>> uniquePositions;
    std::unordered_set<uint32_t> occupiedKeys;
    for (const Vertex& vertex : vertices)
    {
        const int32_t x = QuantizePositionComponent(vertex.position.x());
        const int32_t y = QuantizePositionComponent(vertex.position.y());
        const int32_t z = QuantizePositionComponent(vertex.position.z());
        uniquePositions.emplace(x, y, z);
        occupiedKeys.insert(OutlineKeyCpu(x, y, z));
    }
    stats.uniquePositions = static_cast<uint32_t>(uniquePositions.size());
    stats.occupiedSlots = static_cast<uint32_t>(occupiedKeys.size());
    stats.hashOccupancy = stats.hashSlots > 0
        ? static_cast<float>(stats.occupiedSlots) /
            static_cast<float>(stats.hashSlots)
        : 0.0f;
    return stats;
}

uint32_t OutlineNormalSmoother::NextPowerOfTwo(uint32_t value)
{
    uint32_t result = 1;
    while (result < value && result < (1u << 30))
        result <<= 1;
    return result;
}

void OutlineNormalSmoother::EnsureBuffer(ScratchBuffer& buffer,
    vk::DeviceSize requiredSize, vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memory, const std::string& debugName, bool mapped)
{
    if (buffer.size >= requiredSize && buffer.buffer)
        return;

    DestroyBuffer(buffer);
    const auto allocation = backend->CreateBuffer(
        requiredSize, usage, memory, debugName);
    buffer.buffer = allocation.first;
    buffer.memory = allocation.second;
    buffer.size = requiredSize;
    if (mapped)
        buffer.mapped = backend->MapMemory(buffer.memory, requiredSize);
}

bool OutlineNormalSmoother::EnsureScratchCapacity(ImageScratch& scratch,
    uint32_t hashSize, uint32_t vertexCount, uint32_t jobCount,
    const std::string& label)
{
    const vk::DeviceSize hashBytes =
        static_cast<vk::DeviceSize>(hashSize) * sizeof(uint32_t);
    const vk::DeviceSize repBytes =
        static_cast<vk::DeviceSize>(vertexCount) * sizeof(uint32_t);
    const vk::DeviceSize accumBytes =
        static_cast<vk::DeviceSize>(hashSize) * 3u * sizeof(int32_t);
    const vk::DeviceSize paramsBytes =
        static_cast<vk::DeviceSize>(jobCount) * kParamsStride;

    EnsureBuffer(scratch.hash, hashBytes,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        label + ":Hash", false);
    EnsureBuffer(scratch.rep, repBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        label + ":Rep", false);
    EnsureBuffer(scratch.accum, accumBytes,
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        label + ":Accum", false);
    EnsureBuffer(scratch.params, paramsBytes,
        vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        label + ":Params", true);
    return true;
}

bool OutlineNormalSmoother::EnsureDescriptorPool(ImageScratch& scratch,
    uint32_t setCount)
{
    // 每次录制都重新分配 set；同一 image 的上一帧已在 BeginFrame 等到 fence，
    // 因此可以安全销毁旧池。这样多次 world reload/多个平滑对象不会耗尽 maxSets。
    if (scratch.descriptorPool)
        backend->DestroyDescriptorPool(scratch.descriptorPool);
    for (auto& stageSets : scratch.sets)
        stageSets.clear();

    std::vector<vk::DescriptorPoolSize> poolSizes{
        {vk::DescriptorType::eStorageBuffer, setCount * 4u},
        {vk::DescriptorType::eUniformBuffer, setCount}};
    vk::DescriptorPoolCreateInfo poolInfo;
    poolInfo.setPoolSizes(poolSizes).setMaxSets(setCount);
    scratch.descriptorPool = backend->CreateDescriptorPool(
        poolInfo, "OutlineNormalSmoother:Descriptors");
    return true;
}

void OutlineNormalSmoother::UpdateJobDescriptors(ImageScratch& scratch,
    const std::vector<Job>& jobs)
{
    const uint32_t jobCount = static_cast<uint32_t>(jobs.size());
    for (size_t stage = 0; stage < pipelines.size(); ++stage)
    {
        scratch.sets[stage].clear();
        scratch.sets[stage].resize(jobCount);
        std::vector<vk::DescriptorSetLayout> layouts(
            jobCount, pipelines[stage]->GetDescriptorSetLayouts().at(0));
        vk::DescriptorSetAllocateInfo allocateInfo;
        allocateInfo.setDescriptorPool(scratch.descriptorPool)
            .setSetLayouts(layouts);
        backend->AllocateDescriptorSets(allocateInfo, scratch.sets[stage]);
    }

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(jobCount * 14u);
    for (uint32_t jobIndex = 0; jobIndex < jobCount; ++jobIndex)
    {
        const Job& job = jobs[jobIndex];
        const uint32_t indexCount = static_cast<uint32_t>(
            job.indexBytes / sizeof(uint32_t));
        SmoothParams params{};
        params.vertexCount = job.stats.vertexCount;
        params.indexCount = indexCount;
        params.strideFloats = job.strideFloats;
        params.hashSize = job.stats.hashSlots;
        params.quantScale = kQuantScale;
        std::memcpy(static_cast<uint8_t*>(scratch.params.mapped) +
                static_cast<size_t>(jobIndex) * kParamsStride,
            &params, sizeof(params));

        const vk::DescriptorBufferInfo vertexInfo(
            job.vertexBuffer, 0, job.vertexBytes);
        const vk::DescriptorBufferInfo indexInfo(
            job.indexBuffer, 0, job.indexBytes);
        const vk::DescriptorBufferInfo hashInfo(
            scratch.hash.buffer, 0,
            static_cast<vk::DeviceSize>(job.stats.hashSlots) * sizeof(uint32_t));
        const vk::DescriptorBufferInfo repInfo(
            scratch.rep.buffer, 0,
            static_cast<vk::DeviceSize>(job.stats.vertexCount) * sizeof(uint32_t));
        const vk::DescriptorBufferInfo accumInfo(
            scratch.accum.buffer, 0,
            static_cast<vk::DeviceSize>(job.stats.hashSlots) * 3u * sizeof(int32_t));
        const vk::DescriptorBufferInfo paramsInfo(
            scratch.params.buffer,
            static_cast<vk::DeviceSize>(jobIndex) * kParamsStride,
            kParamsBlockSize);

        // weld: 0 vertex, 1 hash, 2 rep, 3 params
        {
            const vk::DescriptorSet set = scratch.sets[0][jobIndex];
            vk::WriteDescriptorSet write;
            write.setDstSet(set).setDstBinding(0).setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(vertexInfo);
            writes.push_back(write);
            write.setDstBinding(1).setBufferInfo(hashInfo);
            writes.push_back(write);
            write.setDstBinding(2).setBufferInfo(repInfo);
            writes.push_back(write);
            write.setDstBinding(3)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(paramsInfo);
            writes.push_back(write);
        }
        // accumulate: 0 vertex, 1 index, 2 rep, 3 accum, 4 params
        {
            const vk::DescriptorSet set = scratch.sets[1][jobIndex];
            vk::WriteDescriptorSet write;
            write.setDstSet(set).setDstBinding(0).setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(vertexInfo);
            writes.push_back(write);
            write.setDstBinding(1).setBufferInfo(indexInfo);
            writes.push_back(write);
            write.setDstBinding(2).setBufferInfo(repInfo);
            writes.push_back(write);
            write.setDstBinding(3).setBufferInfo(accumInfo);
            writes.push_back(write);
            write.setDstBinding(4)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(paramsInfo);
            writes.push_back(write);
        }
        // apply: 0 vertex, 1 rep, 2 accum, 3 params
        {
            const vk::DescriptorSet set = scratch.sets[2][jobIndex];
            vk::WriteDescriptorSet write;
            write.setDstSet(set).setDstBinding(0).setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setBufferInfo(vertexInfo);
            writes.push_back(write);
            write.setDstBinding(1).setBufferInfo(repInfo);
            writes.push_back(write);
            write.setDstBinding(2).setBufferInfo(accumInfo);
            writes.push_back(write);
            write.setDstBinding(3)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(paramsInfo);
            writes.push_back(write);
        }
    }
    backend->UpdateDescriptorSets(writes);
}

void OutlineNormalSmoother::RecordJob(vk::CommandBuffer commandBuffer,
    const Job& job, ImageScratch& scratch, uint32_t jobIndex)
{
    const vk::DeviceSize hashBytes =
        static_cast<vk::DeviceSize>(job.stats.hashSlots) * sizeof(uint32_t);
    const vk::DeviceSize accumBytes =
        static_cast<vk::DeviceSize>(job.stats.hashSlots) * 3u * sizeof(int32_t);

    // 每个 job 复用同一份 scratch，但命令按顺序执行；先等上一个 job 的
    // compute 写完，再 clear 本 job 需要的槽位。
    commandBuffer.fillBuffer(scratch.hash.buffer, 0, hashBytes, 0u);
    commandBuffer.fillBuffer(scratch.accum.buffer, 0, accumBytes, 0u);

    std::vector<vk::BufferMemoryBarrier> toCompute;
    toCompute.reserve(5);
    vk::BufferMemoryBarrier hashBarrier;
    hashBarrier.setBuffer(scratch.hash.buffer).setOffset(0).setSize(hashBytes)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    toCompute.push_back(hashBarrier);
    vk::BufferMemoryBarrier accumBarrier;
    accumBarrier.setBuffer(scratch.accum.buffer).setOffset(0).setSize(accumBytes)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    toCompute.push_back(accumBarrier);
    vk::BufferMemoryBarrier repBarrier;
    repBarrier.setBuffer(scratch.rep.buffer).setOffset(0)
        .setSize(static_cast<vk::DeviceSize>(job.stats.vertexCount) * sizeof(uint32_t))
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    toCompute.push_back(repBarrier);
    vk::BufferMemoryBarrier vertexBarrier;
    vertexBarrier.setBuffer(job.vertexBuffer).setOffset(0).setSize(job.vertexBytes)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    toCompute.push_back(vertexBarrier);
    vk::BufferMemoryBarrier indexBarrier;
    indexBarrier.setBuffer(job.indexBuffer).setOffset(0).setSize(job.indexBytes)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    toCompute.push_back(indexBarrier);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer |
            vk::PipelineStageFlagBits::eVertexInput |
            vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags{}, nullptr, toCompute, nullptr);

    DispatchStage(commandBuffer, 0, job.stats.weldGroups, scratch, jobIndex);
    vk::MemoryBarrier computeBarrier;
    computeBarrier.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader, {},
        computeBarrier, nullptr, nullptr);

    DispatchStage(commandBuffer, 1, job.stats.accumulateGroups, scratch, jobIndex);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader, {},
        computeBarrier, nullptr, nullptr);

    DispatchStage(commandBuffer, 2, job.stats.applyGroups, scratch, jobIndex);

    // 法线写回顶点缓冲后，本帧后续 shadow/gbuffer 的顶点输入必须看到新值；
    // 同时让下一个 job 的 fillBuffer 排在本 job 的 compute 之后。
    vk::MemoryBarrier toGraphics;
    toGraphics.setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead |
            vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eVertexInput |
            vk::PipelineStageFlagBits::eVertexShader |
            vk::PipelineStageFlagBits::eTransfer,
        {}, toGraphics, nullptr, nullptr);
}

void OutlineNormalSmoother::DispatchStage(vk::CommandBuffer commandBuffer,
    size_t stage, uint32_t groups, ImageScratch& scratch, uint32_t jobIndex)
{
    pipelines[stage]->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
        pipelines[stage]->GetPipelineLayout(), 0,
        scratch.sets[stage][jobIndex], nullptr);
    pipelines[stage]->Dispatch(commandBuffer, groups, 1, 1);
}

std::vector<Eigen::Vector3f> OutlineNormalSmoother::BuildCpuSmoothedNormals(
    const DebugSource& source)
{
    std::vector<Eigen::Vector3f> result(source.vertices.size());
    for (size_t i = 0; i < source.vertices.size(); ++i)
        result[i] = source.vertices[i].normal;

    std::map<std::tuple<int32_t, int32_t, int32_t>, uint32_t> welded;
    std::vector<Eigen::Vector3f> accumulated;
    std::vector<uint32_t> weldedIndex(source.vertices.size(), 0);
    for (size_t i = 0; i < source.vertices.size(); ++i)
    {
        const Eigen::Vector3f& p = source.vertices[i].position;
        const std::tuple<int32_t, int32_t, int32_t> key(
            QuantizePositionComponent(p.x()),
            QuantizePositionComponent(p.y()),
            QuantizePositionComponent(p.z()));
        const auto found = welded.find(key);
        if (found == welded.end())
        {
            weldedIndex[i] = static_cast<uint32_t>(accumulated.size());
            welded.emplace(key, weldedIndex[i]);
            accumulated.emplace_back(Eigen::Vector3f::Zero());
        }
        else
        {
            weldedIndex[i] = found->second;
        }
    }

    for (size_t i = 0; i + 2 < source.indices.size(); i += 3)
    {
        const uint32_t i0 = source.indices[i];
        const uint32_t i1 = source.indices[i + 1];
        const uint32_t i2 = source.indices[i + 2];
        if (i0 >= source.vertices.size() || i1 >= source.vertices.size() ||
            i2 >= source.vertices.size())
            continue;
        const Eigen::Vector3f& p0 = source.vertices[i0].position;
        const Eigen::Vector3f& p1 = source.vertices[i1].position;
        const Eigen::Vector3f& p2 = source.vertices[i2].position;
        Eigen::Vector3f n = (p1 - p0).cross(p2 - p0);
        const float doubleArea = n.norm();
        if (doubleArea <= 1e-12f)
            continue;
        const Eigen::Vector3f sourceReference =
            source.vertices[i0].normal + source.vertices[i1].normal +
            source.vertices[i2].normal;
        if (sourceReference.squaredNorm() > 1e-12f &&
            n.dot(sourceReference) < 0.0f)
            n = -n;
        n /= doubleArea;
        const Eigen::Vector3f weighted = n * (0.5f * doubleArea);
        accumulated[weldedIndex[i0]] += weighted;
        accumulated[weldedIndex[i1]] += weighted;
        accumulated[weldedIndex[i2]] += weighted;
    }

    for (size_t i = 0; i < result.size(); ++i)
    {
        const Eigen::Vector3f& sum = accumulated[weldedIndex[i]];
        const float length = sum.norm();
        if (length > 1e-6f)
            result[i] = sum / length;
    }
    return result;
}

void OutlineNormalSmoother::CollectDebugReadback(uint32_t swapchainImageIndex)
{
    if (swapchainImageIndex >= imageScratch.size())
        return;
    ImageScratch& scratch = imageScratch[swapchainImageIndex];
    if (!scratch.debugReadbackPending || !scratch.debugSource ||
        !scratch.debugReadback.mapped)
        return;

    const DebugSource& source = *scratch.debugSource;
    const std::vector<Eigen::Vector3f> expected =
        BuildCpuSmoothedNormals(source);
    const Vertex* gpuVertices =
        static_cast<const Vertex*>(scratch.debugReadback.mapped);
    const size_t count = source.vertices.size();
    size_t negative = 0;
    size_t belowDot9 = 0;
    size_t nonUnit = 0;
    float minDot = 1.0f;
    float maxAngle = 0.0f;
    size_t worstIndex = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const Eigen::Vector3f gpuN = gpuVertices[i].normal;
        const float l = gpuN.norm();
        if (std::abs(l - 1.0f) > 1e-3f)
            ++nonUnit;
        if (l <= 1e-6f)
            continue;
        const float dot = gpuN.normalized().dot(expected[i].normalized());
        minDot = std::min(minDot, dot);
        maxAngle = std::max(maxAngle,
            std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.2957795f);
        if (dot < 0.0f)
            ++negative;
        if (dot < 0.9f)
            ++belowDot9;
        if (dot < minDot + 1e-6f)
            worstIndex = i;
    }
    std::cout << "[OutlineSmooth][Debug] gpuVsCpu vertices=" << count
              << " negativeDot=" << negative
              << " dotBelow0.9=" << belowDot9
              << " nonUnit=" << nonUnit
              << " minDot=" << minDot
              << " maxAngleDeg=" << maxAngle
              << " worst=" << worstIndex
              << " worstPos=(" << source.vertices[worstIndex].position.x()
              << "," << source.vertices[worstIndex].position.y()
              << "," << source.vertices[worstIndex].position.z() << ")"
              << std::endl;
    scratch.debugReadbackPending = false;
}

void OutlineNormalSmoother::CollectTiming(uint32_t swapchainImageIndex)
{
    if (!timestampSupported || swapchainImageIndex >= imageScratch.size())
        return;
    ImageScratch& scratch = imageScratch[swapchainImageIndex];
    if (!scratch.timingPending)
        return;

    std::array<uint64_t, 2> timestamps{};
    try
    {
        backend->ReadTimestampQueryPair(timestampPool,
            swapchainImageIndex * 2u, timestamps);
    }
    catch (const std::exception& error)
    {
        std::cout << "[OutlineSmooth][Warning] timestamp read failed: "
                  << error.what() << std::endl;
        scratch.timingPending = false;
        return;
    }

    uint64_t timestampMask = std::numeric_limits<uint64_t>::max();
    if (timestampValidBits < 64)
        timestampMask = (uint64_t{1} << timestampValidBits) - 1u;
    const uint64_t elapsedTicks =
        (timestamps[1] - timestamps[0]) & timestampMask;
    const double milliseconds =
        static_cast<double>(elapsedTicks) *
        static_cast<double>(timestampPeriodNanoseconds) / 1000000.0;
    std::cout << "[OutlineSmooth] gpuMs=" << milliseconds
              << " image=" << swapchainImageIndex
              << " jobs=" << scratch.timingJobs << std::endl;
    scratch.timingPending = false;
    scratch.timingJobs = 0;
}

void OutlineNormalSmoother::DestroyBuffer(ScratchBuffer& buffer)
{
    if (backend != nullptr)
    {
        if (buffer.mapped)
            backend->UnmapMemory(buffer.memory);
        if (buffer.buffer)
            backend->DestroyBuffer(buffer.buffer, buffer.memory);
    }
    buffer = {};
}

void OutlineNormalSmoother::DestroyScratch(ImageScratch& scratch)
{
    DestroyBuffer(scratch.hash);
    DestroyBuffer(scratch.rep);
    DestroyBuffer(scratch.accum);
    DestroyBuffer(scratch.params);
    DestroyBuffer(scratch.debugReadback);
    if (backend != nullptr && scratch.descriptorPool)
        backend->DestroyDescriptorPool(scratch.descriptorPool);
    scratch.descriptorPool = nullptr;
    for (auto& stageSets : scratch.sets)
        stageSets.clear();
    scratch.timingPending = false;
    scratch.timingJobs = 0;
    scratch.debugSource.reset();
    scratch.debugReadbackPending = false;
}

} // namespace VL

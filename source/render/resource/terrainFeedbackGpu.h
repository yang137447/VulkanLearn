#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "terrain/terrainFeedback.h"

namespace VL
{
class RendererBackendVulkan;

// World-local feedback 回读 ring：每帧一个 host-visible/coherent buffer，
// 帧末从 terrainFeedback RT 拷贝，等该 frameIndex 的 fence 完成后才允许解析。
// 槽位数必须等于 backend 的 frame fence 数（当前 2）。
class TerrainFeedbackGpuRing
{
public:
    explicit TerrainFeedbackGpuRing(RendererBackendVulkan& rendererBackend);
    ~TerrainFeedbackGpuRing();

    TerrainFeedbackGpuRing(const TerrainFeedbackGpuRing&) = delete;
    TerrainFeedbackGpuRing& operator=(const TerrainFeedbackGpuRing&) = delete;

    uint32_t SlotCount() const noexcept;
    uint32_t Width() const noexcept { return width; }
    uint32_t Height() const noexcept { return height; }

    // 在 scene pass 之后录制：ColorAttachment -> TransferSrc -> copy -> ColorAttachment。
    // 调用方保证该 frameIndex 的 fence 已在本帧 BeginFrame 等待过，因此可以安全
    // 覆盖同槽旧内容；CPU 仍只会通过 TryConsume 的 epoch 比较读取已完成数据。
    void RecordCopy(
        vk::CommandBuffer commandBuffer,
        vk::Image feedbackImage,
        uint32_t frameIndex,
        uint64_t submittedEpoch,
        uint8_t frameTag);

    // 只在 copiedEpoch <= completedEpoch 时解析并清除 pending，保证只消费一次。
    std::optional<TerrainFeedbackFrameSummary> TryConsume(
        uint32_t frameIndex,
        uint64_t completedEpoch);

    bool IsPending(uint32_t frameIndex) const noexcept;
    bool PendingTagRange(uint8_t& oldest, uint8_t& newest) const noexcept;

private:
    RendererBackendVulkan* rendererBackend = nullptr;
    uint32_t width = TerrainFeedbackResolution;
    uint32_t height = TerrainFeedbackResolution;
    std::vector<vk::Buffer> buffers;
    std::vector<vk::DeviceMemory> memories;
    std::vector<void*> mapped;
    TerrainFeedbackRingState ringState;
};

} // namespace VL

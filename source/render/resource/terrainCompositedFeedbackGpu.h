#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "terrain/terrainFeedback.h"
#include "terrain/terrainCompositedFeedback.h"
#include "terrain/terrainRvtBudget.h"

namespace VL
{
class RendererBackendVulkan;
class TerrainCompositedService;

// 合成材质页 feedback 回读 ring：每帧把 4 张 128×128 RGBA8 attachment 依次
// 拷贝进同一个 host-visible/coherent buffer（4×16 KiB），等该 frameIndex 的
// fence 完成后才允许解析。epoch/frameTag 规则与覆盖页 ring 完全一致，
// 复用 TerrainFeedbackRingState。
class TerrainCompositedFeedbackGpuRing
{
public:
    explicit TerrainCompositedFeedbackGpuRing(
        RendererBackendVulkan& rendererBackend,
        std::shared_ptr<TerrainRvtBudgetOwner> budget = {});
    ~TerrainCompositedFeedbackGpuRing();

    TerrainCompositedFeedbackGpuRing(const TerrainCompositedFeedbackGpuRing&) = delete;
    TerrainCompositedFeedbackGpuRing& operator=(
        const TerrainCompositedFeedbackGpuRing&) = delete;

    uint32_t SlotCount() const noexcept;
    uint32_t Width() const noexcept { return TerrainCompositedFeedbackResolution; }
    uint32_t Height() const noexcept { return TerrainCompositedFeedbackResolution; }

    // 在 scene pass 之后录制：每张图 ColorAttachment -> TransferSrc -> copy ->
    // ColorAttachment，4 张共用一次录制、一次提交。调用方保证该 frameIndex
    // 的 fence 已在本帧 BeginFrame 等待过。
    void RecordCopy(
        vk::CommandBuffer commandBuffer,
        const std::array<vk::Image, TerrainCompositedFeedbackSlotCount>& images,
        uint32_t frameIndex,
        uint64_t submittedEpoch,
        uint8_t frameTag);

    std::optional<std::vector<TerrainCompositedFeedbackRecord>> TryConsumeComposited(
        uint32_t frameIndex,
        uint64_t completedEpoch, TerrainCompositedService* explicitObserver = nullptr);

    bool IsPending(uint32_t frameIndex) const noexcept;

private:
    RendererBackendVulkan* rendererBackend = nullptr;
    std::vector<vk::Buffer> buffers;
    std::vector<vk::DeviceMemory> memories;
    std::vector<void*> mapped;
    TerrainFeedbackRingState ringState;
    std::optional<TerrainRvtBudgetLease> lease;
};

} // namespace VL

#include "render/resource/terrainLayerFeedbackGpu.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "commonFunction.h"
#include "render/backend/rendererBackendVulkan.h"

namespace VL
{

TerrainLayerFeedbackGpuRing::TerrainLayerFeedbackGpuRing(
    RendererBackendVulkan& backend)
    : rendererBackend(&backend)
{
    const uint32_t slotCount = backend.GetFrameFenceCount();
    if (slotCount == 0)
    {
        throw std::runtime_error(
            "TerrainLayerFeedbackGpuRing requires at least one frame fence");
    }

    const vk::DeviceSize slotBytes =
        static_cast<vk::DeviceSize>(TerrainLayerFeedbackBufferBytes);
    buffers.reserve(slotCount);
    memories.reserve(slotCount);
    mapped.reserve(slotCount);
    try
    {
        for (uint32_t slot = 0; slot < slotCount; ++slot)
        {
            const std::pair<vk::Buffer, vk::DeviceMemory> created =
                backend.CreateBuffer(
                    slotBytes,
                    vk::BufferUsageFlagBits::eTransferDst,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent,
                    "TerrainLayerFeedbackRing" + std::to_string(slot));
            buffers.push_back(created.first);
            memories.push_back(created.second);
            // 常驻映射：回读完成后 CPU 直接解析，不再每帧 map/unmap。
            mapped.push_back(backend.MapMemory(created.second, slotBytes));
        }
    }
    catch (...)
    {
        // 已创建的槽位必须逐个销毁，避免 World 候选失败时泄漏 device memory。
        for (size_t slot = 0; slot < buffers.size(); ++slot)
        {
            if (slot < memories.size() && slot < mapped.size() &&
                mapped[slot] != nullptr)
            {
                backend.UnmapMemory(memories[slot]);
            }
            backend.DestroyBuffer(
                buffers[slot],
                slot < memories.size() ? memories[slot]
                                       : vk::DeviceMemory{});
        }
        buffers.clear();
        memories.clear();
        mapped.clear();
        throw;
    }

    ringState.Reset(slotCount);
}

TerrainLayerFeedbackGpuRing::~TerrainLayerFeedbackGpuRing()
{
    if (rendererBackend == nullptr)
        return;
    for (size_t slot = 0; slot < buffers.size(); ++slot)
    {
        if (mapped[slot] != nullptr)
            rendererBackend->UnmapMemory(memories[slot]);
        rendererBackend->DestroyBuffer(buffers[slot], memories[slot]);
    }
}

uint32_t TerrainLayerFeedbackGpuRing::SlotCount() const noexcept
{
    return ringState.SlotCount();
}

void TerrainLayerFeedbackGpuRing::RecordCopy(
    vk::CommandBuffer commandBuffer,
    const std::array<vk::Image, TerrainLayerFeedbackSlotCount>& images,
    uint32_t frameIndex,
    uint64_t submittedEpoch,
    uint8_t frameTag)
{
    if (frameIndex >= buffers.size())
    {
        throw std::out_of_range(
            "Terrain layer feedback copy frame index is out of range");
    }

    for (uint32_t slot = 0; slot < TerrainLayerFeedbackSlotCount; ++slot)
    {
        if (!images[slot])
        {
            throw std::invalid_argument(
                "Terrain layer feedback copy requires 4 valid images");
        }
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            images[slot],
            1,
            vk::Format::eR8G8B8A8Unorm,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eTransferSrcOptimal);
        CommonFunction::RecordCopyImageToBuffer(
            commandBuffer,
            images[slot],
            buffers[frameIndex],
            TerrainLayerFeedbackResolution,
            TerrainLayerFeedbackResolution,
            false,
            0,
            static_cast<vk::DeviceSize>(slot) *
                TerrainLayerFeedbackImageBytes);
        CommonFunction::RecordTransitionImageLayout(
            commandBuffer,
            images[slot],
            1,
            vk::Format::eR8G8B8A8Unorm,
            vk::ImageLayout::eTransferSrcOptimal,
            vk::ImageLayout::eColorAttachmentOptimal);
    }

    ringState.Record(frameIndex, submittedEpoch, frameTag);
}

std::optional<TerrainLayerFeedbackFrameSummary>
TerrainLayerFeedbackGpuRing::TryConsume(
    uint32_t frameIndex,
    uint64_t completedEpoch)
{
    // 先确认该槽真的有可读映射，再消费 ring 的 pending 状态：否则映射缺失时
    // TryConsume 已经把 pending 清掉，这一帧的 feedback 会被静默丢弃。
    if (frameIndex >= mapped.size() || mapped[frameIndex] == nullptr)
        return std::nullopt;
    const std::optional<TerrainFeedbackRingState::CompletedFrame> completed =
        ringState.TryConsume(frameIndex, completedEpoch);
    if (!completed.has_value())
        return std::nullopt;

    TerrainLayerFeedbackFrameSummary summary =
        SummarizeTerrainLayerFeedback(
            static_cast<const uint8_t*>(mapped[frameIndex]),
            TerrainLayerFeedbackBufferBytes,
            completed->frameTag);
    summary.copiedEpoch = completed->copiedEpoch;
    return summary;
}

bool TerrainLayerFeedbackGpuRing::IsPending(
    uint32_t frameIndex) const noexcept
{
    return ringState.IsPending(frameIndex);
}

} // namespace VL

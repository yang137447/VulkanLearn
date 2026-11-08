#include "render/resource/terrainFeedbackGpu.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "commonFunction.h"
#include "render/backend/rendererBackendVulkan.h"

namespace VL
{

TerrainFeedbackGpuRing::TerrainFeedbackGpuRing(
    RendererBackendVulkan& backend)
    : rendererBackend(&backend)
{
    const uint32_t slotCount = backend.GetFrameFenceCount();
    if (slotCount == 0)
    {
        throw std::runtime_error(
            "TerrainFeedbackGpuRing requires at least one frame fence");
    }

    const vk::DeviceSize slotBytes =
        static_cast<vk::DeviceSize>(width) * height *
        TerrainFeedbackTexelBytes;
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
                        vk::MemoryPropertyFlagBits::eHostCoherent |
                        vk::MemoryPropertyFlagBits::eHostCached,
                    "TerrainFeedbackRing" + std::to_string(slot));
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
            if (slot < memories.size() &&
                slot < mapped.size() &&
                mapped[slot] != nullptr)
                backend.UnmapMemory(memories[slot]);
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

TerrainFeedbackGpuRing::~TerrainFeedbackGpuRing()
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

uint32_t TerrainFeedbackGpuRing::SlotCount() const noexcept
{
    return ringState.SlotCount();
}

void TerrainFeedbackGpuRing::RecordCopy(
    vk::CommandBuffer commandBuffer,
    vk::Image feedbackImage,
    uint32_t frameIndex,
    uint64_t submittedEpoch,
    uint8_t frameTag)
{
    if (frameIndex >= buffers.size())
    {
        throw std::out_of_range(
            "Terrain feedback copy frame index is out of range");
    }
    if (!feedbackImage)
    {
        throw std::invalid_argument(
            "Terrain feedback copy requires a valid image");
    }

    // terrainFeedback pass 的 render pass finalLayout 是 ColorAttachmentOptimal。
    // 回读前后显式往返，下一帧 render pass 仍以 Undefined/clear 开始，不依赖旧内容。
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        feedbackImage,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eTransferSrcOptimal);
    CommonFunction::RecordCopyImageToBuffer(
        commandBuffer,
        feedbackImage,
        buffers[frameIndex],
        width,
        height);
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        feedbackImage,
        1,
        vk::Format::eR8G8B8A8Unorm,
        vk::ImageLayout::eTransferSrcOptimal,
        vk::ImageLayout::eColorAttachmentOptimal);

    // 这里只登记 CPU 侧状态；真正可读性由该槽 fence 完成 + TryConsume 的
    // copiedEpoch <= completedEpoch 双重条件保证。
    ringState.Record(frameIndex, submittedEpoch, frameTag);
}

std::optional<TerrainFeedbackFrameSummary>
TerrainFeedbackGpuRing::TryConsume(
    uint32_t frameIndex,
    uint64_t completedEpoch)
{
    const std::optional<TerrainFeedbackRingState::CompletedFrame> completed =
        ringState.TryConsume(frameIndex, completedEpoch);
    if (!completed.has_value())
        return std::nullopt;
    if (frameIndex >= mapped.size() || mapped[frameIndex] == nullptr)
        return std::nullopt;

    const size_t byteCount =
        static_cast<size_t>(width) * height * TerrainFeedbackTexelBytes;
    TerrainFeedbackFrameSummary summary = SummarizeTerrainFeedback(
        static_cast<const uint8_t*>(mapped[frameIndex]),
        byteCount,
        completed->frameTag);
    summary.copiedEpoch = completed->copiedEpoch;
    return summary;
}

bool TerrainFeedbackGpuRing::IsPending(
    uint32_t frameIndex) const noexcept
{
    return ringState.IsPending(frameIndex);
}

bool TerrainFeedbackGpuRing::PendingTagRange(
    uint8_t& oldest,
    uint8_t& newest) const noexcept
{
    return ringState.PendingTagRange(oldest, newest);
}

} // namespace VL

#include "render/resource/terrainCompositedFeedbackGpu.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "commonFunction.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/terrainCompositedService.h"

namespace VL
{

TerrainCompositedFeedbackGpuRing::TerrainCompositedFeedbackGpuRing(
    RendererBackendVulkan& backend, std::shared_ptr<TerrainRvtBudgetOwner> budget)
    : rendererBackend(&backend)
{
    const uint32_t slotCount = backend.GetFrameFenceCount();
    if (slotCount == 0)
    {
        throw std::runtime_error(
            "TerrainCompositedFeedbackGpuRing requires at least one frame fence");
    }

    const vk::DeviceSize slotBytes =
        static_cast<vk::DeviceSize>(TerrainCompositedFeedbackBufferBytes);
    if (budget)
    {
        const auto allocation = backend.QueryBufferMemoryRequirements(
            slotBytes, vk::BufferUsageFlagBits::eTransferDst).size;
        lease = budget->TryReserve({allocation * slotCount, allocation * slotCount +
            slotCount * (sizeof(vk::Buffer) + sizeof(vk::DeviceMemory) + sizeof(void*)), 0, 0},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Candidate);
        if (!lease)
            throw std::runtime_error("Composited feedback ring exceeds frozen actual allocation budget");
    }
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
                    "TerrainCompositedFeedbackRing" + std::to_string(slot));
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

TerrainCompositedFeedbackGpuRing::~TerrainCompositedFeedbackGpuRing()
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

uint32_t TerrainCompositedFeedbackGpuRing::SlotCount() const noexcept
{
    return ringState.SlotCount();
}

void TerrainCompositedFeedbackGpuRing::RecordCopy(
    vk::CommandBuffer commandBuffer,
    const std::array<vk::Image, TerrainCompositedFeedbackSlotCount>& images,
    uint32_t frameIndex,
    uint64_t submittedEpoch,
    uint8_t frameTag)
{
    if (frameIndex >= buffers.size())
    {
        throw std::out_of_range(
            "Terrain layer feedback copy frame index is out of range");
    }

    for (uint32_t slot = 0; slot < TerrainCompositedFeedbackSlotCount; ++slot)
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
            TerrainCompositedFeedbackResolution,
            TerrainCompositedFeedbackResolution,
            false,
            0,
            static_cast<vk::DeviceSize>(slot) *
                TerrainCompositedFeedbackImageBytes);
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

std::optional<std::vector<TerrainCompositedFeedbackRecord>>
TerrainCompositedFeedbackGpuRing::TryConsumeComposited(
    uint32_t frameIndex,
    uint64_t completedEpoch, TerrainCompositedService* explicitObserver)
{
    if (frameIndex >= mapped.size() || mapped[frameIndex] == nullptr)
        return std::nullopt;
    const auto completed = ringState.TryConsume(frameIndex, completedEpoch);
    if (!completed.has_value())
        return std::nullopt;
    if (explicitObserver)
        explicitObserver->ObserveFeedbackReadback(
            static_cast<const uint8_t*>(mapped[frameIndex]), TerrainCompositedFeedbackBufferBytes,
            completed->frameTag, completed->copiedEpoch, completedEpoch);
    return DecodeTerrainCompositedFeedback(
        static_cast<const uint8_t*>(mapped[frameIndex]),
        TerrainCompositedFeedbackBufferBytes,
        completed->frameTag,
        completed->copiedEpoch);
}

bool TerrainCompositedFeedbackGpuRing::IsPending(
    uint32_t frameIndex) const noexcept
{
    return ringState.IsPending(frameIndex);
}

} // namespace VL

#pragma once

#include <cstdint>
#include <memory>
#include <array>
#include <limits>
#include <stdexcept>
#include "terrain/terrainRvtBudget.h"
#include <vulkan/vulkan.hpp>

namespace VL
{
class RendererBackendVulkan;

struct TerrainCompositedLoadingTarget
{
    vk::ImageView view;
    vk::Format format = vk::Format::eUndefined;
    vk::Extent2D extent;
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    std::shared_ptr<const void> owner;
    uint64_t ownerGeneration = 0;
};

class TerrainCompositedLoading
{
public:
    static constexpr size_t Capacity = 8;

    static TerrainRvtCost ReservationCost(TerrainRvtCost metadataPerEntry)
    {
        if (!metadataPerEntry.gpuBytes || !metadataPerEntry.cpuBytes ||
            metadataPerEntry.physicalSlots || metadataPerEntry.productionTasks ||
            metadataPerEntry.gpuBytes > std::numeric_limits<uint64_t>::max() / Capacity ||
            metadataPerEntry.cpuBytes >
                (std::numeric_limits<uint64_t>::max() - sizeof(TerrainCompositedLoading)) / Capacity)
            throw std::invalid_argument("Loading cache requires an explicit nonzero metadata reserve");
        return {metadataPerEntry.gpuBytes * Capacity,
            metadataPerEntry.cpuBytes * Capacity + sizeof(TerrainCompositedLoading), 0, 0};
    }

    TerrainCompositedLoading(
        const std::shared_ptr<TerrainRvtBudgetOwner>& budget,
        TerrainRvtCost metadataPerEntry);
    ~TerrainCompositedLoading();
    TerrainCompositedLoading(const TerrainCompositedLoading&) = delete;
    TerrainCompositedLoading& operator=(const TerrainCompositedLoading&) = delete;

    static constexpr bool Required(bool terrainWorld, bool qualityReady) noexcept
    {
        return terrainWorld && !qualityReady;
    }

    [[nodiscard]] std::shared_ptr<void> Record(
        RendererBackendVulkan& backend,
        vk::CommandBuffer command,
        const TerrainCompositedLoadingTarget& target,
        uint64_t submissionEpoch,
        bool terrainWorld,
        bool qualityReady);

private:
    struct Entry;
    std::shared_ptr<TerrainRvtBudgetLease> reservation;
    std::array<std::shared_ptr<Entry>, Capacity> entries;
};
}

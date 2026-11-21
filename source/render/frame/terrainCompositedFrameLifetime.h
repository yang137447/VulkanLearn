#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace VL
{
class TerrainCompositedFrameLifetime
{
public:
    static constexpr size_t Capacity = 8;

    void Complete(uint64_t completedEpoch)
    {
        for (auto& slot : slots)
            if (slot.epoch && slot.epoch <= completedEpoch)
                slot = {};
    }

    void Prepare(size_t frameIndex) const
    {
        if (frameIndex >= slots.size() || slots[frameIndex].epoch)
            throw std::runtime_error("Terrain quality/loading frame slot still awaits its submission fence");
    }

    void Submitted(size_t frameIndex, uint64_t epoch,
        std::shared_ptr<const void> quality, std::shared_ptr<void> loading)
    {
        Prepare(frameIndex);
        if (!epoch)
            throw std::invalid_argument("Terrain frame lifetime requires its actual submission epoch");
        slots[frameIndex] = {epoch, std::move(quality), std::move(loading)};
    }

private:
    struct Slot
    {
        uint64_t epoch = 0;
        std::shared_ptr<const void> quality;
        std::shared_ptr<void> loading;
    };
    std::array<Slot, Capacity> slots;
};
}

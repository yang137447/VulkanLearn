#pragma once

#include <optional>
#include <unordered_map>

#include <Eigen/Dense>

#include "core/runtimeResult.h"
#include "world/world.h"
#include "world/worldSnapshot.h"

namespace VL
{

struct WorldSnapshotBuildDesc
{
    uint64_t worldGeneration = 0;
    uint64_t frameIndex = 0;
    int debugViewMode = 0;
    float environmentIntensity = 1.0f;
    bool hasSelectedDraw = false;
    bool selectedAllMaterialSlots = false;
    RuntimeId selectedObjectId = 0;
    uint32_t selectedMaterialSlotIndex = 0;
};

// Exports the immutable render-facing data for one World frame. This builder
// reads World only; renderer cache to World assembly belongs to WorldBuilder.
class WorldSnapshotBuilder
{
public:
    RuntimeResult<WorldSnapshot> Build(
        const World& world,
        const WorldSnapshotBuildDesc& desc);
    void Reset();
    void Swap(WorldSnapshotBuilder& other) noexcept;

private:
    std::optional<uint64_t> previousWorldGeneration;
    std::optional<Eigen::Matrix4f> previousViewProjection;
    // Previous object transforms are renderer input history, so the builder
    // caches them by stable object id instead of asking RenderSystem to write
    // back into mutable World objects after command submission.
    std::unordered_map<RuntimeId, Eigen::Matrix4f> previousObjectModels;
};

} // namespace VL

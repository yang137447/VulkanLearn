#pragma once

#include <memory>

#include "core/runtimeResult.h"
#include "world/world.h"

namespace VL
{

struct WorldBuildPlan;

// Builds the active World from the validated WorldBuildPlan. Camera, light,
// environment, mesh object runtime data and terrain collision placements are
// created here from pure build-plan data; renderer resources stay in the
// renderer's own loaders.
class WorldBuilder
{
public:
    RuntimeResult<std::shared_ptr<World>> BuildFromLoadedScene(
        uint64_t generation,
        const WorldBuildPlan& worldBuildPlan,
        uint32_t shadowCascadeCount) const;
};

} // namespace VL

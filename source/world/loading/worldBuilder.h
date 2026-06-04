#pragma once

#include <memory>

#include "core/runtimeResult.h"
#include "world/world.h"

namespace VL
{

class RendererResourceCache;
struct WorldBuildPlan;

// Builds the active World from the validated WorldBuildPlan. Camera, light,
// environment, and mesh object runtime data are created here. The renderer
// resource cache is consulted only for global resources that affect World
// metadata, such as BRDF LUT availability.
class WorldBuilder
{
public:
    RuntimeResult<std::shared_ptr<World>> BuildFromLoadedScene(
        uint64_t generation,
        const WorldBuildPlan& worldBuildPlan,
        const RendererResourceCache& resourceCache) const;
};

} // namespace VL

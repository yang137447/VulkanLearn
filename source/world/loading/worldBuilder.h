#pragma once

#include <memory>

#include "core/runtimeResult.h"
#include "world/world.h"

namespace VL
{

class RendererResourceCache;
struct WorldBuildPlan;

// Builds the active World from the validated WorldBuildPlan. Camera, light, and
// environment metadata are created here; the renderer resource cache is only
// consulted for mesh resources that still own descriptors and GPU buffers.
class WorldBuilder
{
public:
    RuntimeResult<std::shared_ptr<World>> BuildFromLoadedScene(
        uint64_t generation,
        const WorldBuildPlan& worldBuildPlan,
        const RendererResourceCache& resourceCache) const;
};

} // namespace VL

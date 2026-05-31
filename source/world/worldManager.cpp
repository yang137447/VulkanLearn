#include "world/worldManager.h"

#include <algorithm>
#include <utility>

#include "world/world.h"

namespace VL
{

WorldHandle WorldManager::ActivateLoadedWorld(std::shared_ptr<World> world)
{
    activeWorld = std::move(world);
    if (!activeWorld)
    {
        activeWorldHandle = WorldHandle{};
        return activeWorldHandle;
    }

    activeWorldHandle = BuildHandle(*activeWorld);
    nextWorldGeneration = std::max(nextWorldGeneration, activeWorldHandle.generation + 1);
    return activeWorldHandle;
}

void WorldManager::ClearActiveWorld()
{
    activeWorld.reset();
    activeWorldHandle = WorldHandle{};
}

WorldHandle WorldManager::BuildHandle(const World& world) const
{
    WorldHandle handle;
    handle.generation = world.GetGeneration();
    handle.scenePath = world.GetScenePath();
    handle.viewTarget = world.GetViewTarget();
    return handle;
}

} // namespace VL

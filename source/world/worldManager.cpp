#include "world/worldManager.h"

#include <algorithm>
#include <utility>

#include "world/world.h"

namespace VL
{

PreparedWorldActivation WorldManager::PrepareActivation(
    std::shared_ptr<World> world) const
{
    PreparedWorldActivation prepared;
    prepared.world = std::move(world);
    if (!prepared.world)
    {
        return prepared;
    }
    prepared.handle = BuildHandle(*prepared.world);
    prepared.nextWorldGeneration = std::max(
        nextWorldGeneration,
        prepared.handle.generation + 1);
    return prepared;
}

std::shared_ptr<World> WorldManager::CommitPreparedActivation(
    PreparedWorldActivation prepared) noexcept
{
    std::shared_ptr<World> retiredWorld;
    retiredWorld.swap(activeWorld);
    activeWorld.swap(prepared.world);
    using std::swap;
    swap(activeWorldHandle, prepared.handle);
    nextWorldGeneration = prepared.nextWorldGeneration;
    return retiredWorld;
}

WorldHandle WorldManager::ActivateLoadedWorld(std::shared_ptr<World> world)
{
    PreparedWorldActivation prepared =
        PrepareActivation(std::move(world));
    (void)CommitPreparedActivation(std::move(prepared));
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

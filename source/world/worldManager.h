#pragma once

#include <cstdint>
#include <memory>
#include <string>

class SceneNode;

namespace VL
{

class World;

struct WorldHandle
{
    uint64_t generation = 0;
    std::string scenePath;
    // GT-only control target for the current world. The renderer must continue
    // to consume WorldSnapshot/RenderScene instead of this mutable object.
    std::weak_ptr<SceneNode> viewTarget;

    bool IsValid() const { return generation != 0 && !scenePath.empty(); }
};

struct PreparedWorldActivation
{
    std::shared_ptr<World> world;
    WorldHandle handle;
    uint64_t nextWorldGeneration = 1;
};

// Metadata owner for the active World. Renderer resource loaders own GPU-side
// mesh resources, while WorldManager owns the stable world identity that
// snapshots and renderer data validate against.
class WorldManager
{
public:
    uint64_t GetNextWorldGeneration() const { return nextWorldGeneration; }
    PreparedWorldActivation PrepareActivation(
        std::shared_ptr<World> world) const;
    std::shared_ptr<World> CommitPreparedActivation(
        PreparedWorldActivation prepared) noexcept;
    WorldHandle ActivateLoadedWorld(std::shared_ptr<World> world);
    void ClearActiveWorld();

    bool HasActiveWorld() const { return activeWorld != nullptr; }
    const WorldHandle& GetActiveWorldHandle() const { return activeWorldHandle; }
    const std::shared_ptr<World>& GetActiveWorld() const { return activeWorld; }

private:
    WorldHandle BuildHandle(const World& world) const;

    uint64_t nextWorldGeneration = 1;
    WorldHandle activeWorldHandle;
    std::shared_ptr<World> activeWorld;
};

} // namespace VL

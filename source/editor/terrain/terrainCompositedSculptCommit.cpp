#include "editor/terrain/terrainEditApply.h"
#include "editor/terrain/terrainCompositedSculptApply.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "world/world.h"

namespace VL
{
namespace
{

struct WorldChunkSnapshot
{
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    uint32_t lodLevel = 0;
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
};

void RollbackWorldChunks(
    World& world,
    const std::string& placementId,
    const std::vector<WorldChunkSnapshot>& snapshots,
    size_t appliedCount) noexcept
{
    const size_t count = std::min(appliedCount, snapshots.size());
    for (size_t index = count; index > 0; --index)
    {
        const WorldChunkSnapshot& snapshot = snapshots[index - 1];
        try
        {
            static_cast<void>(world.UpdateTerrainChunkBounds(
                placementId, snapshot.chunkX, snapshot.chunkZ,
                snapshot.lodLevel, snapshot.boundsMin, snapshot.boundsMax));
        }
        catch (...)
        {
        }
    }
}

}

bool ApplyCompositedTerrainSculptUpdate(
    World& world,
    Editor::TerrainCompositedSculptApply& sculpt,
    const Editor::TerrainMeshUpdate& update,
    const std::function<void(const TerrainBuildPlan&, const TerrainCompositedDirtyChange&, uint64_t)>& refreshPlacement,
    TerrainEditApplyStats& stats,
    std::string& error)
{
    stats = {};
    return sculpt.Apply(world.GetGeneration(), update,
        [&](const Editor::TerrainMeshUpdate& frozen, std::string& failure)
        {
            const TerrainCollisionInstance* placement = nullptr;
            for (const auto& candidate : world.GetTerrainCollisionInstances())
                if (candidate.placementId == frozen.placementId)
                {
                    placement = &candidate;
                    break;
                }
            if (!placement || !refreshPlacement)
            {
                failure = "Terrain sculpt requires a World placement and queued refresh service";
                return false;
            }
            TerrainBuildPlan plan = placement->plan;
            const auto oldHeightmap = plan.heightmap;
            std::vector<WorldChunkSnapshot> snapshots;
            for (const auto& chunk : frozen.chunks)
            {
                const auto found = world.GetMeshObjects().find(chunk.objectName);
                if (found == world.GetMeshObjects().end() ||
                    !found->second.isTerrain ||
                    found->second.terrainPlacementId != frozen.placementId ||
                    found->second.meshKey != chunk.resourceKey ||
                    found->second.lodLevel != chunk.lodLevel)
                {
                    failure = "Terrain sculpt World chunk identity mismatch";
                    return false;
                }
                snapshots.push_back({chunk.chunkX, chunk.chunkZ, chunk.lodLevel,
                    found->second.localBoundsMin, found->second.localBoundsMax});
            }
            size_t applied = 0;
            try
            {
                for (const auto& chunk : frozen.chunks)
                {
                    if (!world.UpdateTerrainChunkBounds(frozen.placementId,
                            chunk.chunkX, chunk.chunkZ, chunk.lodLevel,
                            chunk.boundsMin, chunk.boundsMax))
                        throw std::runtime_error("Terrain sculpt World bounds update failed");
                    ++applied;
                }
                if (!world.UpdateTerrainPlacementHeightmap(frozen.placementId, frozen.heightmap))
                    throw std::runtime_error("Terrain sculpt World height update failed");
                plan.heightmap = frozen.heightmap;
                const TerrainCompositedDirtyChange change{
                    frozen.placementId, TerrainCompositedDependencyChange::Sculpt,
                    frozen.compositedDirty.valid ? frozen.compositedDirty.rect : TerrainCompositedVTUVRect{}};
                refreshPlacement(plan, change, frozen.sourceRevision);
            }
            catch (...)
            {
                RollbackWorldChunks(world, frozen.placementId, snapshots, applied);
                world.UpdateTerrainPlacementHeightmap(frozen.placementId, oldHeightmap);
                throw;
            }
            stats.worldChunks = applied;
            return true;
        }, error);
}

bool ApplyCompositedTerrainSculptUpdate(
    World& world,
    Editor::TerrainCompositedSculptApply& sculpt,
    const Editor::TerrainMeshUpdate& update,
    const std::function<void(const TerrainBuildPlan&)>& refreshPlacement,
    TerrainEditApplyStats& stats,
    std::string& error)
{
    std::function<void(const TerrainBuildPlan&, const TerrainCompositedDirtyChange&, uint64_t)> typed;
    if (refreshPlacement)
        typed = [&refreshPlacement](const TerrainBuildPlan& plan, const TerrainCompositedDirtyChange&, uint64_t) {
            refreshPlacement(plan);
        };
    return ApplyCompositedTerrainSculptUpdate(world, sculpt, update, typed, stats, error);
}

}

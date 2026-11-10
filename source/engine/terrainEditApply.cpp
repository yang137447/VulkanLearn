#include "engine/terrainEditApply.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderableObject.h"
#include "world/world.h"

namespace VL
{
namespace
{

struct CacheSnapshot
{
    std::string resourceKey;
    bool existed = false;
    std::shared_ptr<RenderableObject> object;
};

struct WorldChunkSnapshot
{
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    uint32_t lodLevel = 0;
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
};

struct StagedObject
{
    std::string resourceKey;
    std::shared_ptr<RenderableObject> object;
};

void RollbackCacheBindings(
    RendererResourceCache& cache,
    const std::vector<CacheSnapshot>& snapshots,
    size_t boundCount) noexcept
{
    const size_t count = std::min(boundCount, snapshots.size());
    for (size_t index = count; index > 0; --index)
    {
        const CacheSnapshot& snapshot = snapshots[index - 1];
        try
        {
            cache.BindRenderableObject(
                snapshot.resourceKey,
                snapshot.existed ? snapshot.object : nullptr);
        }
        catch (...)
        {
            // 回滚本身失败时已经没有可安全推进的恢复动作；保留原错误返回。
        }
    }
}

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
                placementId,
                snapshot.chunkX,
                snapshot.chunkZ,
                snapshot.lodLevel,
                snapshot.boundsMin,
                snapshot.boundsMax));
        }
        catch (...)
        {
        }
    }
}

bool HasCollisionPlacement(
    const World& world,
    const std::string& placementId,
    std::shared_ptr<const HostImage>& oldHeightmap)
{
    for (const TerrainCollisionInstance& instance :
         world.GetTerrainCollisionInstances())
    {
        if (instance.placementId != placementId)
            continue;
        oldHeightmap = instance.plan.heightmap;
        return true;
    }
    return false;
}

} // namespace

bool ApplyTerrainMeshUpdate(
    World& world,
    RendererBackendVulkan& backend,
    const Editor::TerrainMeshUpdate& update,
    TerrainEditApplyStats& stats,
    std::string& error)
{
    error.clear();
    stats = TerrainEditApplyStats();
    if (update.placementId.empty())
    {
        error = "Terrain edit apply requires a placement id";
        return false;
    }
    if (!update.heightmap)
    {
        error = "Terrain edit apply requires an edited heightmap snapshot";
        return false;
    }

    std::shared_ptr<const HostImage> oldHeightmap;
    if (!HasCollisionPlacement(world, update.placementId, oldHeightmap))
    {
        error = "Terrain edit apply has no matching collision placement: " +
            update.placementId;
        return false;
    }

    const auto& meshObjects = world.GetMeshObjects();
    std::unordered_set<std::string> seenObjectNames;
    std::vector<WorldChunkSnapshot> oldWorldChunks;
    oldWorldChunks.reserve(update.chunks.size());
    for (const Editor::TerrainChunkMeshUpdate& chunk : update.chunks)
    {
        if (chunk.resourceKey.empty() || chunk.objectName.empty())
        {
            error = "Terrain edit apply received an empty resource/object name";
            return false;
        }
        if (!seenObjectNames.insert(chunk.objectName).second)
        {
            error = "Terrain edit apply received a duplicate chunk object: " +
                chunk.objectName;
            return false;
        }
        const auto objectIt = meshObjects.find(chunk.objectName);
        if (objectIt == meshObjects.end())
        {
            error = "Terrain edit apply cannot find World chunk: " +
                chunk.objectName;
            return false;
        }
        const WorldMeshObject& object = objectIt->second;
        if (!object.isTerrain ||
            object.terrainPlacementId != update.placementId ||
            object.lodLevel != chunk.lodLevel)
        {
            error = "Terrain edit apply World chunk identity mismatch: " +
                chunk.objectName;
            return false;
        }
        WorldChunkSnapshot snapshot;
        snapshot.chunkX = chunk.chunkX;
        snapshot.chunkZ = chunk.chunkZ;
        snapshot.lodLevel = chunk.lodLevel;
        snapshot.boundsMin = object.localBoundsMin;
        snapshot.boundsMax = object.localBoundsMax;
        oldWorldChunks.push_back(snapshot);
    }

    RendererResourceCache& cache = RendererResourceCache::GetInstance();
    std::vector<CacheSnapshot> oldCacheBindings;
    oldCacheBindings.reserve(update.chunks.size());
    for (const Editor::TerrainChunkMeshUpdate& chunk : update.chunks)
    {
        CacheSnapshot snapshot;
        snapshot.resourceKey = chunk.resourceKey;
        const std::shared_ptr<RenderableObject> existing =
            cache.GetRenderableObject(chunk.resourceKey);
        if (existing != nullptr)
        {
            snapshot.existed = true;
            snapshot.object = existing;
        }
        oldCacheBindings.push_back(std::move(snapshot));
    }

    std::vector<StagedObject> staged;
    staged.reserve(update.chunks.size());
    try
    {
        // 先完成全部 GPU 对象构造再发布缓存，避免中途失败留下半批新网格。
        for (const Editor::TerrainChunkMeshUpdate& chunk : update.chunks)
        {
            StagedObject object;
            object.resourceKey = chunk.resourceKey;
            object.object = std::make_shared<RenderableObject>(
                chunk.vertices,
                chunk.indices,
                backend,
                chunk.resourceKey);
            staged.push_back(std::move(object));
        }
    }
    catch (const std::exception& exception)
    {
        error = std::string("Terrain edit apply failed to build renderable: ") +
            exception.what();
        return false;
    }
    catch (...)
    {
        error = "Terrain edit apply failed to build renderable";
        return false;
    }

    size_t boundCount = 0;
    try
    {
        for (const StagedObject& object : staged)
        {
            cache.BindRenderableObject(object.resourceKey, object.object);
            ++boundCount;
        }
    }
    catch (const std::exception& exception)
    {
        RollbackCacheBindings(cache, oldCacheBindings, boundCount);
        error = std::string("Terrain edit apply failed to bind renderable: ") +
            exception.what();
        return false;
    }
    catch (...)
    {
        RollbackCacheBindings(cache, oldCacheBindings, boundCount);
        error = "Terrain edit apply failed to bind renderable";
        return false;
    }

    size_t updatedWorldChunks = 0;
    try
    {
        for (const Editor::TerrainChunkMeshUpdate& chunk : update.chunks)
        {
            if (!world.UpdateTerrainChunkBounds(
                    update.placementId,
                    chunk.chunkX,
                    chunk.chunkZ,
                    chunk.lodLevel,
                    chunk.boundsMin,
                    chunk.boundsMax))
            {
                RollbackWorldChunks(
                    world,
                    update.placementId,
                    oldWorldChunks,
                    updatedWorldChunks);
                RollbackCacheBindings(cache, oldCacheBindings, boundCount);
                error = "Terrain edit apply did not find matching World chunk bounds";
                return false;
            }
            ++updatedWorldChunks;
        }

        if (!world.UpdateTerrainPlacementHeightmap(
                update.placementId,
                update.heightmap))
        {
            RollbackWorldChunks(
                world,
                update.placementId,
                oldWorldChunks,
                updatedWorldChunks);
            RollbackCacheBindings(cache, oldCacheBindings, boundCount);
            if (oldHeightmap)
            {
                static_cast<void>(world.UpdateTerrainPlacementHeightmap(
                    update.placementId,
                    std::move(oldHeightmap)));
            }
            error = "Terrain edit apply did not find matching collision heightmap";
            return false;
        }
    }
    catch (const std::exception& exception)
    {
        RollbackWorldChunks(
            world,
            update.placementId,
            oldWorldChunks,
            updatedWorldChunks);
        RollbackCacheBindings(cache, oldCacheBindings, boundCount);
        if (oldHeightmap)
        {
            static_cast<void>(world.UpdateTerrainPlacementHeightmap(
                update.placementId,
                std::move(oldHeightmap)));
        }
        error = std::string("Terrain edit apply failed to update World: ") +
            exception.what();
        return false;
    }
    catch (...)
    {
        RollbackWorldChunks(
            world,
            update.placementId,
            oldWorldChunks,
            updatedWorldChunks);
        RollbackCacheBindings(cache, oldCacheBindings, boundCount);
        if (oldHeightmap)
        {
            static_cast<void>(world.UpdateTerrainPlacementHeightmap(
                update.placementId,
                std::move(oldHeightmap)));
        }
        error = "Terrain edit apply failed to update World";
        return false;
    }

    stats.renderableObjects = staged.size();
    stats.worldChunks = updatedWorldChunks;
    // 新对象已经发布，旧 chunk buffer 仍可能被 in-flight 帧引用；不能靠
    // render thread idle（只保证 CPU 录制结束）直接析构。交给 GPU epoch 退休队列。
    ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
    const uint64_t lastUsedEpoch = retireQueue.GetLastSubmittedEpoch();
    for (const CacheSnapshot& snapshot : oldCacheBindings)
        if (snapshot.existed && snapshot.object)
            retireQueue.RetireShared(
                "TerrainChunk:" + snapshot.resourceKey,
                world.GetGeneration(),
                lastUsedEpoch,
                snapshot.object);
    return true;
}

} // namespace VL

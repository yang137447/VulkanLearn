#include "engine/terrainChunkStreamer.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "material.h"
#include "materialInstance.h"
#include "material/materialAssetUtils.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainCompositedService.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderableObject.h"
#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainCollisionTypes.h"
#include "terrain/terrainVertexUpdateQueue.h"
#include "world/world.h"

namespace VL
{
void TerrainChunkStreamer::Update(
    World& world,
    RendererBackendVulkan& backend,
    RendererResourceCache& cache,
    const Eigen::Vector3f& cameraPositionCm,
    uint32_t maxUploadsPerFrame,
    uint32_t maxNewRequestsPerFrame)
{
    if (world.GetGeneration() != worldGeneration)
    {
        ++epoch;
        worldGeneration = world.GetGeneration();
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.clear();
            completed.clear();
        }
        readyUploads.clear();
        RebuildPlacements(world);
    }

    PollCompletedBuilds();

    TerrainChunkResidencyDesc residency;
    residency.cameraKnown = true;
    residency.cameraPositionCm = cameraPositionCm;

    for (size_t placementIndex = 0;
         placementIndex < placements.size();
         ++placementIndex)
    {
        Placement& placement = placements[placementIndex];
        if (!RefreshPlacement(world, placement))
            continue;

        const std::vector<TerrainChunkStreamingCandidate> candidates =
            ComputeTerrainChunkStreamingSet(
                residency,
                placement.residencyDistancesCm,
                placement.model,
                placement.chunkBounds,
                placement.lodLevelCount);
        std::set<TerrainChunkId> desired;
        for (const TerrainChunkStreamingCandidate& candidate : candidates)
            desired.insert(candidate.id);

        size_t inFlight = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            inFlight = requests.size() + completed.size() +
                readyUploads.size();
        }
        uint32_t requested = 0;
        for (const TerrainChunkStreamingCandidate& candidate : candidates)
        {
            if (requested >= maxNewRequestsPerFrame || inFlight >= 16)
                break;
            if (placement.resident.count(candidate.id) != 0 ||
                placement.building.count(candidate.id) != 0)
            {
                continue;
            }
            RequestBuild(placementIndex, candidate.id);
            ++requested;
            ++inFlight;
            ++stats.requestedBuilds;
        }
    }

    uint32_t uploads = 0;
    while (!readyUploads.empty() && uploads < maxUploadsPerFrame)
    {
        BuildResult result = std::move(readyUploads.front());
        readyUploads.pop_front();
        if (result.epoch != epoch)
            continue;
        if (result.placementIndex >= placements.size())
            continue;
        Placement& placement = placements[result.placementIndex];
        if (!FinishBuild(placement, result))
        {
            ++stats.staleBuilds;
            continue;
        }
        if (!result.error.empty() || result.chunks.size() != 1)
        {
            ++stats.failedBuilds;
            continue;
        }
        try
        {
            if (!UploadChunk(world, backend, cache, placement, result))
            {
                ++stats.failedBuilds;
                continue;
            }
        }
        catch (const std::exception& exception)
        {
            std::cerr << "[TerrainStreaming] upload failed: "
                      << exception.what() << std::endl;
            ++stats.failedBuilds;
            continue;
        }
        placement.resident.insert(result.id);
        placement.absentFrames.erase(result.id);
        ++uploads;
        ++stats.uploadedChunks;
    }

    const uint32_t maxRemovals = 4;
    uint32_t removals = 0;
    for (Placement& placement : placements)
    {
        const std::vector<TerrainChunkStreamingCandidate> candidates =
            ComputeTerrainChunkStreamingSet(
                residency,
                placement.residencyDistancesCm,
                placement.model,
                placement.chunkBounds,
                placement.lodLevelCount);
        std::set<TerrainChunkId> desired;
        for (const TerrainChunkStreamingCandidate& candidate : candidates)
            desired.insert(candidate.id);

        for (auto it = placement.resident.begin();
             it != placement.resident.end();)
        {
            const TerrainChunkId id = *it;
            if (desired.count(id) != 0)
            {
                placement.absentFrames.erase(id);
                ++it;
                continue;
            }
            uint32_t& absent = placement.absentFrames[id];
            ++absent;
            if (absent < 2 || removals >= maxRemovals)
            {
                ++it;
                continue;
            }
            RemoveChunk(world, cache, placement, id);
            placement.absentFrames.erase(id);
            it = placement.resident.erase(it);
            ++removals;
            ++stats.evictedChunks;
        }
    }

    stats.placements = static_cast<uint32_t>(placements.size());
    stats.residentChunks = 0;
    for (const Placement& placement : placements)
        stats.residentChunks += static_cast<uint32_t>(placement.resident.size());
    {
        std::lock_guard<std::mutex> lock(mutex);
        stats.pendingBuilds = static_cast<uint32_t>(requests.size());
        stats.readyUploads = static_cast<uint32_t>(readyUploads.size());
    }
}

bool TerrainChunkStreamer::UploadChunk(
    World& world,
    RendererBackendVulkan& backend,
    RendererResourceCache& cache,
    Placement& placement,
    const BuildResult& result)
{
    const TerrainMeshChunk& chunk = result.chunks.front();
    const WorldMeshObject* source =
        FindTemplate(world, placement.placementId);
    if (source == nullptr || placement.sceneObjectIdentity.empty())
        return false;

    const std::string objectName = TerrainChunkObjectName(
        placement.sceneObjectIdentity,
        chunk.chunkX,
        chunk.chunkZ,
        result.id.lodLevel,
        true);
    if (world.GetMeshObjects().find(objectName) !=
        world.GetMeshObjects().end())
    {
        return true;
    }

    const std::string resourceKey = TerrainChunkResourceKey(
        placement.plan,
        chunk.chunkX,
        chunk.chunkZ,
        result.id.lodLevel);
    const std::shared_ptr<RenderableObject> cached =
        cache.GetRenderableObject(resourceKey);
    const bool dynamic = UsesDynamicVertices(
        placement.plan, TerrainCompositedService::IsAuthoringMode());
    const auto streaming = cache.GetTerrainStreaming();
    if (dynamic && (!streaming || !streaming->terrainVertices))
        throw std::runtime_error("Dynamic terrain streaming requires a vertex update queue");

    WorldMeshObject object = *source;
    object.debugName = objectName;
    object.sceneObjectIdentity = placement.sceneObjectIdentity;
    // 流式上传按目标 LOD 重新解析材质身份。LOD0 用 detail MI，LOD>0 用基础
    // MI；不能沿用 LOD0 模板的材质，否则粗 LOD 也会进入 PATCH_LIST 管线。
    const std::string materialInstancePath =
        TerrainMaterialInstancePathForLod(
            placement.plan.desc,
            result.id.lodLevel);
    const std::string materialInstanceKey =
        MaterialAssetUtils::NormalizeAssetPath(materialInstancePath);
    const std::shared_ptr<MaterialInstance> materialInstance =
        cache.GetMaterialInstance(materialInstanceKey);
    if (!materialInstance)
    {
        throw std::runtime_error(
            "Terrain streamed chunk material instance is not loaded: " +
            materialInstanceKey);
    }
    const std::shared_ptr<Material> baseMaterial =
        materialInstance->GetBaseMaterial().lock();
    if (!baseMaterial)
    {
        throw std::runtime_error(
            "Terrain streamed chunk material instance has no base material: " +
            materialInstanceKey);
    }
    object.materialInstanceKey = materialInstance->GetName();
    object.materialKey = baseMaterial->GetMaterialKey();
    object.meshKey = resourceKey;
    object.localBoundsMin = chunk.boundsMin;
    object.localBoundsMax = chunk.boundsMax;
    object.lodGroupId = placement.placementId + "|" +
        std::to_string(chunk.chunkX) + "|" +
        std::to_string(chunk.chunkZ);
    object.lodLevel = result.id.lodLevel;
    object.lodLevelCount = placement.lodLevelCount;
    object.lodDistancesCm = placement.lodDistancesCm;
    const size_t boundsIndex =
        static_cast<size_t>(chunk.chunkZ) * placement.chunksPerSide +
        chunk.chunkX;
    if (boundsIndex >= placement.chunkBounds.size())
        return false;
    object.lodGroupBoundsMin =
        placement.chunkBounds[boundsIndex].boundsMin;
    object.lodGroupBoundsMax =
        placement.chunkBounds[boundsIndex].boundsMax;
    object.isTerrain = true;
    object.terrainAssetPath = placement.plan.desc.assetPath;
    object.terrainPlacementId = placement.placementId;
    std::shared_ptr<RenderableObject> replacement;
    if (!cached || dynamic)
        replacement = std::make_shared<RenderableObject>(
            chunk.vertices, chunk.indices, backend, resourceKey, dynamic, chunk.patchIndices);
    if (replacement)
    {
        if (streaming && streaming->terrainVertices)
            streaming->terrainVertices->UnregisterDynamicChunk(resourceKey);
        cache.BindRenderableObject(resourceKey, replacement);
        if (cached)
            ResourceRetireQueue::GetInstance().RetireShared(
                resourceKey, world.GetGeneration(),
                ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(), cached);
    }
    if (dynamic)
        streaming->terrainVertices->RegisterDynamicChunk(resourceKey);
    try
    {
        world.AddMeshObject(objectName, std::move(object));
    }
    catch (...)
    {
        if (dynamic)
            streaming->terrainVertices->UnregisterDynamicChunk(resourceKey);
        throw;
    }
    return true;
}

void TerrainChunkStreamer::RemoveChunk(
    World& world,
    RendererResourceCache& cache,
    Placement& placement,
    const TerrainChunkId& id)
{
    const std::string objectName = TerrainChunkObjectName(
        placement.sceneObjectIdentity,
        id.chunkX,
        id.chunkZ,
        id.lodLevel,
        true);
    const std::string resourceKey = TerrainChunkResourceKey(
        placement.plan, id.chunkX, id.chunkZ, id.lodLevel);
    const auto streaming = cache.GetTerrainStreaming();
    if (streaming && streaming->terrainVertices)
        streaming->terrainVertices->UnregisterDynamicChunk(resourceKey);
    world.RemoveMeshObject(objectName);
    std::shared_ptr<RenderableObject> object =
        cache.TakeRenderableObject(resourceKey);
    if (object != nullptr)
    {
        ResourceRetireQueue::GetInstance().RetireShared(
            resourceKey,
            world.GetGeneration(),
            ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(),
            std::move(object));
    }
    std::shared_ptr<RendererObjectResourceEntry> objectResource =
        cache.TakeObjectResource(objectName);
    if (objectResource != nullptr)
    {
        ResourceRetireQueue::GetInstance().RetireShared(
            objectName + "|objectResource",
            world.GetGeneration(),
            ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch(),
            std::move(objectResource));
    }
}

} // namespace VL

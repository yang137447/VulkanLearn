#include "engine/terrainChunkStreamer.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <utility>

#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderableObject.h"
#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainCollisionTypes.h"
#include "world/world.h"

namespace VL
{
namespace
{

uint32_t ChunksPerSide(const TerrainAssetDesc& desc)
{
    if (desc.chunkQuads == 0 || desc.gridResolution % desc.chunkQuads != 0)
        return 0;
    return desc.gridResolution / desc.chunkQuads;
}

} // namespace

TerrainChunkStreamer::TerrainChunkStreamer()
{
    worker = std::thread([this]() { WorkerMain(); });
}

TerrainChunkStreamer::~TerrainChunkStreamer()
{
    StopWorker();
}

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
            placement.building.insert(candidate.id);
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
        placement.building.erase(result.id);
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

void TerrainChunkStreamer::RebuildPlacements(World& world)
{
    placements.clear();
    for (const TerrainCollisionInstance& instance :
         world.GetTerrainCollisionInstances())
    {
        if (instance.plan.desc.lodLevels <= 1 || !instance.plan.heightmap)
            continue;
        const uint32_t chunksPerSide = ChunksPerSide(instance.plan.desc);
        if (chunksPerSide == 0)
            continue;

        Placement placement;
        placement.plan = instance.plan;
        placement.placementId = instance.placementId.empty()
            ? TerrainPlacementIdentity(instance.plan)
            : instance.placementId;
        placement.chunksPerSide = chunksPerSide;
        placement.lodLevelCount = std::max(1u, instance.plan.desc.lodLevels);
        placement.lodDistancesCm =
            TerrainLodDistancesCm(instance.plan.desc);
        placement.residencyDistancesCm =
            TerrainChunkResidencyDistancesCm(instance.plan.desc);
        placement.chunkBounds =
            TerrainMeshBuilder::ComputeChunkBounds(placement.plan);

        for (const auto& [name, object] : world.GetMeshObjects())
        {
            if (!object.isTerrain ||
                object.terrainPlacementId != placement.placementId)
            {
                continue;
            }
            std::string groupPlacement;
            uint32_t chunkX = 0;
            uint32_t chunkZ = 0;
            if (!ParseTerrainChunkLodGroupId(
                    object.lodGroupId,
                    groupPlacement,
                    chunkX,
                    chunkZ))
            {
                continue;
            }
            if (groupPlacement != placement.placementId)
                continue;
            placement.resident.insert(
                TerrainChunkId{chunkX, chunkZ, object.lodLevel});
        }
        placements.push_back(std::move(placement));
    }
    std::cout << "[TerrainStreaming] world=" << worldGeneration
              << " placements=" << placements.size() << std::endl;
}

bool TerrainChunkStreamer::RefreshPlacement(
    World& world, Placement& placement)
{
    const TerrainCollisionInstance* current = nullptr;
    for (const TerrainCollisionInstance& instance :
         world.GetTerrainCollisionInstances())
    {
        const std::string currentId = instance.placementId.empty()
            ? TerrainPlacementIdentity(instance.plan)
            : instance.placementId;
        if (currentId == placement.placementId)
        {
            current = &instance;
            break;
        }
    }
    if (current == nullptr)
        return false;
    if (placement.plan.heightmap != current->plan.heightmap)
    {
        placement.plan = current->plan;
        placement.chunkBounds =
            TerrainMeshBuilder::ComputeChunkBounds(placement.plan);
    }

    const WorldMeshObject* object =
        FindTemplate(world, placement.placementId);
    if (object == nullptr)
        return false;
    placement.sceneObjectIdentity = object->sceneObjectIdentity;
    placement.model = object->model;
    return true;
}

const WorldMeshObject* TerrainChunkStreamer::FindTemplate(
    const World& world, const std::string& placementId) const
{
    const WorldMeshObject* best = nullptr;
    for (const auto& [name, object] : world.GetMeshObjects())
    {
        if (!object.isTerrain ||
            object.terrainPlacementId != placementId)
        {
            continue;
        }
        if (best == nullptr || object.lodLevel < best->lodLevel)
            best = &object;
    }
    return best;
}

void TerrainChunkStreamer::RequestBuild(
    size_t placementIndex, const TerrainChunkId& id)
{
    BuildRequest request;
    request.epoch = epoch;
    request.placementIndex = placementIndex;
    request.id = id;
    request.plan = placements[placementIndex].plan;
    {
        std::lock_guard<std::mutex> lock(mutex);
        requests.push_back(std::move(request));
    }
    wake.notify_one();
}

void TerrainChunkStreamer::PollCompletedBuilds()
{
    std::vector<BuildResult> completedNow;
    {
        std::lock_guard<std::mutex> lock(mutex);
        completedNow.swap(completed);
    }
    for (BuildResult& result : completedNow)
        readyUploads.push_back(std::move(result));
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
    if (cached == nullptr)
    {
        cache.BindRenderableObject(
            resourceKey,
            std::make_shared<RenderableObject>(
                chunk.vertices,
                chunk.indices,
                backend,
                resourceKey));
    }

    WorldMeshObject object = *source;
    object.debugName = objectName;
    object.sceneObjectIdentity = placement.sceneObjectIdentity;
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
    world.AddMeshObject(objectName, std::move(object));
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
    world.RemoveMeshObject(objectName);

    const std::string resourceKey = TerrainChunkResourceKey(
        placement.plan, id.chunkX, id.chunkZ, id.lodLevel);
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

void TerrainChunkStreamer::WorkerMain()
{
    for (;;)
    {
        BuildRequest request;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [this]() {
                return stopping || !requests.empty();
            });
            if (stopping && requests.empty())
                return;
            request = std::move(requests.front());
            requests.pop_front();
        }

        BuildResult result;
        result.epoch = request.epoch;
        result.placementIndex = request.placementIndex;
        result.id = request.id;
        try
        {
            const uint32_t chunksPerSide =
                ChunksPerSide(request.plan.desc);
            const size_t maskIndex =
                static_cast<size_t>(request.id.chunkZ) * chunksPerSide +
                request.id.chunkX;
            TerrainChunkBuildFilter filter;
            filter.chunkMask.assign(
                static_cast<size_t>(chunksPerSide) * chunksPerSide, 0);
            if (chunksPerSide == 0 ||
                maskIndex >= filter.chunkMask.size())
            {
                result.error = "requested terrain chunk is out of range";
            }
            else
            {
                filter.chunkMask[maskIndex] = 1;
                result.chunks = TerrainMeshBuilder::BuildChunks(
                    request.plan, request.id.lodLevel, &filter);
            }
        }
        catch (const std::exception& exception)
        {
            result.error = exception.what();
        }
        catch (...)
        {
            result.error = "unknown terrain chunk build failure";
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            completed.push_back(std::move(result));
        }
    }
}

void TerrainChunkStreamer::StopWorker()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping)
            return;
        stopping = true;
        requests.clear();
        completed.clear();
    }
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}

} // namespace VL

#include "engine/terrainChunkStreamer.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <utility>

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

}

TerrainChunkStreamer::TerrainChunkStreamer()
{
    worker = std::thread([this]() { WorkerMain(); });
}

TerrainChunkStreamer::~TerrainChunkStreamer()
{
    StopWorker();
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

void TerrainChunkStreamer::RefreshHeightSnapshot(
    Placement& placement, const TerrainBuildPlan& plan)
{
    if (placement.plan.heightmap == plan.heightmap)
        return;
    auto bounds = TerrainMeshBuilder::ComputeChunkBounds(plan);
    placement.plan = plan;
    placement.chunkBounds = std::move(bounds);
    ++placement.heightRevision;
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
    RefreshHeightSnapshot(placement, current->plan);

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
    request.heightRevision = placements[placementIndex].heightRevision;
    {
        std::lock_guard<std::mutex> lock(mutex);
        requests.push_back(std::move(request));
    }
    placements[placementIndex].building[id] = placements[placementIndex].heightRevision;
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

bool TerrainChunkStreamer::FinishBuild(Placement& placement, const BuildResult& result)
{
    const auto building = placement.building.find(result.id);
    if (building == placement.building.end() || building->second != result.heightRevision)
        return false;
    placement.building.erase(building);
    // result 强持有原始高度快照；指针身份不会因释放/地址重用误判为当前高度。
    return result.heightmap && result.heightmap == placement.plan.heightmap &&
        result.heightRevision == placement.heightRevision;
}

bool TerrainChunkStreamer::UsesDynamicVertices(const TerrainBuildPlan& plan, bool authoring)
{
    return plan.desc.compositedVT.enabled && authoring;
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
        result.heightmap = request.plan.heightmap;
        result.heightRevision = request.heightRevision;
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

}

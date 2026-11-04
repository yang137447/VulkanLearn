#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <Eigen/Dense>

#include "terrain/terrainChunkResidency.h"
#include "terrain/terrainMeshBuilder.h"

namespace VL
{

class RendererBackendVulkan;
class RendererResourceCache;
class World;
struct WorldMeshObject;

struct TerrainChunkStreamingStats
{
    uint32_t placements = 0;
    uint32_t residentChunks = 0;
    uint32_t pendingBuilds = 0;
    uint32_t readyUploads = 0;
    uint64_t requestedBuilds = 0;
    uint64_t uploadedChunks = 0;
    uint64_t evictedChunks = 0;
    uint64_t failedBuilds = 0;
};

// 运行时几何 chunk streaming 的第一版：
//   - CPU 网格生成在独立 worker 线程，按距离/预算排队；
//   - GPU RenderableObject 创建与 World 挂载仍回到 GT（渲染线程空闲时）；
//   - 离开期望集合的 chunk 延迟两帧后卸载，RenderableObject 走统一退休队列。
// 它只消费 World 的 terrain collision instance（plan+TRS）与现有 chunk
// WorldMeshObject 模板，不引入第二套地形类型。
class TerrainChunkStreamer
{
public:
    TerrainChunkStreamer();
    ~TerrainChunkStreamer();

    TerrainChunkStreamer(const TerrainChunkStreamer&) = delete;
    TerrainChunkStreamer& operator=(const TerrainChunkStreamer&) = delete;

    void Update(
        World& world,
        RendererBackendVulkan& backend,
        RendererResourceCache& cache,
        const Eigen::Vector3f& cameraPositionCm,
        uint32_t maxUploadsPerFrame = 2,
        uint32_t maxNewRequestsPerFrame = 4);

    const TerrainChunkStreamingStats& Stats() const noexcept { return stats; }

private:
    struct Placement
    {
        TerrainBuildPlan plan;
        std::string placementId;
        std::string sceneObjectIdentity;
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        uint32_t chunksPerSide = 0;
        uint32_t lodLevelCount = 1;
        std::vector<float> lodDistancesCm;
        std::vector<float> residencyDistancesCm;
        std::vector<TerrainChunkBounds> chunkBounds;
        std::set<TerrainChunkId> resident;
        std::set<TerrainChunkId> building;
        std::map<TerrainChunkId, uint32_t> absentFrames;
    };

    struct BuildRequest
    {
        uint64_t epoch = 0;
        size_t placementIndex = 0;
        TerrainChunkId id;
        TerrainBuildPlan plan;
    };

    struct BuildResult
    {
        uint64_t epoch = 0;
        size_t placementIndex = 0;
        TerrainChunkId id;
        std::vector<TerrainMeshChunk> chunks;
        std::string error;
    };

    void RebuildPlacements(World& world);
    bool RefreshPlacement(World& world, Placement& placement);
    const WorldMeshObject* FindTemplate(
        const World& world,
        const std::string& placementId) const;
    void RequestBuild(size_t placementIndex, const TerrainChunkId& id);
    void PollCompletedBuilds();
    bool UploadChunk(
        World& world,
        RendererBackendVulkan& backend,
        RendererResourceCache& cache,
        Placement& placement,
        const BuildResult& result);
    void RemoveChunk(
        World& world,
        RendererResourceCache& cache,
        Placement& placement,
        const TerrainChunkId& id);
    void WorkerMain();
    void StopWorker();

    std::vector<Placement> placements;
    uint64_t worldGeneration = 0;
    uint64_t epoch = 0;
    TerrainChunkStreamingStats stats;

    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::deque<BuildRequest> requests;
    std::vector<BuildResult> completed;
    std::deque<BuildResult> readyUploads;
};

} // namespace VL

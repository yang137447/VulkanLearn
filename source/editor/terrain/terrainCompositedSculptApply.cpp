#include "editor/terrain/terrainCompositedSculptApply.h"

#include <exception>
#include <limits>
#include <set>

namespace VL::Editor
{

TerrainCompositedSculptApply::TerrainCompositedSculptApply(
    size_t maxChunks, size_t maxBytes)
    : maxChunks_(maxChunks), maxBytes_(maxBytes)
{
}

bool TerrainCompositedSculptApply::Register(
    uint64_t worldGeneration, const TerrainMeshUpdate& baseline,
    bool loadedWithDynamicVertices, std::string& error)
{
    error.clear();
    if (!loadedWithDynamicVertices)
    {
        error = "Terrain sculpt requires dynamic vertices at load time; static "
            "conversion requires a structural replacement safe point";
        return false;
    }
    if (!baseline.desc.compositedVT.enabled || baseline.placementId.empty() ||
        baseline.chunks.empty() || baseline.chunks.size() > maxChunks_)
    {
        error = "Terrain sculpt baseline has invalid identity, mode or chunk budget";
        return false;
    }
    auto next = std::make_shared<State>();
    next->generation = worldGeneration;
    next->placementId = baseline.placementId;
    next->assetPath = baseline.assetPath;
    std::set<std::string> names;
    for (const auto& source : baseline.chunks)
    {
        if (source.resourceKey.empty() || source.objectName.empty() ||
            source.vertices.empty() || !names.insert(source.objectName).second ||
            next->chunks.count(source.resourceKey))
        {
            error = "Terrain sculpt baseline has invalid or duplicate chunk identity";
            return false;
        }
        const size_t remainingIndices = (maxBytes_ - next->bytes) / sizeof(uint32_t);
        if (source.indices.size() > remainingIndices ||
            source.patchIndices.size() > remainingIndices - source.indices.size())
        {
            error = "Terrain sculpt topology exceeds byte budget";
            return false;
        }
        for (uint32_t index : source.indices)
            if (index >= source.vertices.size())
            {
                error = "Terrain sculpt baseline index is outside vertex range";
                return false;
            }
        for (uint32_t index : source.patchIndices)
            if (index >= source.vertices.size())
            {
                error = "Terrain sculpt baseline patch index is outside vertex range";
                return false;
            }
        Chunk chunk;
        chunk.chunkX = source.chunkX;
        chunk.chunkZ = source.chunkZ;
        chunk.lodLevel = source.lodLevel;
        chunk.objectName = source.objectName;
        chunk.vertexCount = source.vertices.size();
        chunk.indices = std::make_shared<const std::vector<uint32_t>>(source.indices);
        chunk.patchIndices = std::make_shared<const std::vector<uint32_t>>(source.patchIndices);
        next->bytes += (source.indices.size() + source.patchIndices.size()) * sizeof(uint32_t);
        next->chunks.emplace(source.resourceKey, std::move(chunk));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = std::move(next);
    return true;
}

bool TerrainCompositedSculptApply::Apply(
    uint64_t worldGeneration, const TerrainMeshUpdate& update,
    const Commit& commit, std::string& error)
{
    error.clear();
    std::shared_ptr<const State> previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previous = state_;
    }
    if (!previous || previous->generation != worldGeneration ||
        previous->placementId != update.placementId ||
        previous->assetPath != update.assetPath ||
        !update.desc.compositedVT.enabled || !update.heightmap || !commit)
    {
        error = "Terrain sculpt update requires the registered World, placement and snapshot";
        return false;
    }
    auto next = std::make_shared<State>(*previous);
    std::set<std::string> keys;
    for (const auto& source : update.chunks)
    {
        const auto found = next->chunks.find(source.resourceKey);
        if (found == next->chunks.end() || !keys.insert(source.resourceKey).second)
        {
            error = "Terrain sculpt update has unregistered or duplicate chunk key";
            return false;
        }
        Chunk& chunk = found->second;
        if (chunk.objectName != source.objectName ||
            chunk.chunkX != source.chunkX || chunk.chunkZ != source.chunkZ ||
            chunk.lodLevel != source.lodLevel ||
            chunk.vertexCount != source.vertices.size() ||
            *chunk.indices != source.indices || *chunk.patchIndices != source.patchIndices)
        {
            error = "Terrain sculpt topology changed; structural replacement requires a safe point";
            return false;
        }
        if (!source.boundsMin.allFinite() || !source.boundsMax.allFinite() ||
            (source.boundsMin.array() > source.boundsMax.array()).any())
        {
            error = "Terrain sculpt bounds are invalid";
            return false;
        }
        for (const Vertex& vertex : source.vertices)
            if (!vertex.position.allFinite() || !vertex.normal.allFinite() ||
                !vertex.tangent.allFinite() ||
                (vertex.position.array() < source.boundsMin.array()).any() ||
                (vertex.position.array() > source.boundsMax.array()).any())
            {
                error = "Terrain sculpt vertices are invalid or outside bounds";
                return false;
            }
        if (!chunk.latest)
        {
            if (chunk.vertexCount > (maxBytes_ - next->bytes) / sizeof(Vertex))
            {
                error = "Terrain sculpt retained vertices exceed byte budget";
                return false;
            }
            next->bytes += chunk.vertexCount * sizeof(Vertex);
        }
        chunk.latest = std::make_shared<const std::vector<Vertex>>(source.vertices);
        if (chunk.version == std::numeric_limits<uint64_t>::max())
        {
            error = "Terrain sculpt chunk version exhausted";
            return false;
        }
        ++chunk.version;
        chunk.acknowledged = std::make_shared<std::array<uint64_t, FrameSlotCount>>();
    }
    // 全部验证和分配先完成；GT 回调只更新 World 值并排队材质刷新。
    // Register/Apply/Reset 必须由同一个 GT 串行调用，RT 只调用回放接口。
    try
    {
        if (!commit(update, error))
            return false;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
    catch (...)
    {
        error = "Terrain sculpt commit failed";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = std::move(next);
    return true;
}

void TerrainCompositedSculptApply::Replay(
    uint64_t worldGeneration, const QueueVertices& queue) const
{
    std::shared_ptr<const State> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = state_;
    }
    if (!snapshot || snapshot->generation != worldGeneration || !queue)
        return;
    // 不能在一次 drain 后清除：其它 image slot、隐藏和非选中 LOD 还没收到。
    // 每个实际 RT frame、动态队列 drain 前重放；不可仅在 stamp 或 GT tick 调用。
    for (const auto& [key, chunk] : snapshot->chunks)
        if (chunk.latest)
            queue(key, *chunk.latest);
}

void TerrainCompositedSculptApply::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.reset();
}

void TerrainCompositedSculptApply::ReplayForFrame(
    uint64_t worldGeneration, uint32_t frameSlot,
    const FrameQueueVertices& queue) const
{
    if (frameSlot >= FrameSlotCount || !queue)
        return;
    std::shared_ptr<const State> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = state_;
    }
    if (!snapshot || snapshot->generation != worldGeneration)
        return;
    for (const auto& [key, chunk] : snapshot->chunks)
    {
        if (!chunk.latest)
            continue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ((*chunk.acknowledged)[frameSlot] == chunk.version)
                continue;
        }
        if (!queue(key, *chunk.latest))
            continue;
        // 未修改 chunk 在 GT 新快照中共享 ACK；修改 chunk 独立分配 ACK，
        // 因此旧回放的确认既不会丢失，也不能确认并发发布的新版本。
        std::lock_guard<std::mutex> lock(mutex_);
        (*chunk.acknowledged)[frameSlot] = chunk.version;
    }
}

}

#include "terrain/terrainVertexUpdateQueue.h"

#include <stdexcept>
#include <utility>
#include "vertexDataStruct.h"

namespace VL
{
void TerrainVertexUpdateQueue::RegisterDynamicChunk(std::string key)
{
    if (key.empty())
        throw std::invalid_argument("Dynamic terrain chunk requires a resource key");
    std::lock_guard<std::mutex> lock(mutex);
    if (dynamicChunks.size() >= 4096 && !dynamicChunks.count(key))
        throw std::invalid_argument("Dynamic terrain chunk registry exceeds capacity");
    if (dynamicChunks.insert(std::move(key)).second)
        ++residencyRevision;
}

void TerrainVertexUpdateQueue::UnregisterDynamicChunk(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!dynamicChunks.count(key))
        return;
    std::shared_ptr<State> candidate;
    if (state && state->chunks.count(key))
    {
        candidate = std::make_shared<State>(*state);
        const auto found = candidate->chunks.find(key);
        candidate->bytes -= found->second.vertices->size() * sizeof(Vertex);
        candidate->chunks.erase(found);
    }
    dynamicChunks.erase(key);
    if (candidate)
        state = std::move(candidate);
    ++residencyRevision;
    ++publicationRevision;
}

uint64_t TerrainVertexUpdateQueue::ResidencyRevision() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return residencyRevision;
}

bool TerrainVertexUpdateQueue::HasDynamicChunk(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return dynamicChunks.count(key) != 0;
}

TerrainVertexUpdateQueue::Prepared TerrainVertexUpdateQueue::Prepare(
    uint64_t generation, const std::vector<Update>& updates) const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!generation || (state && state->generation != generation))
        throw std::invalid_argument("Terrain vertex update has a stale World generation");
    auto candidate = state ? std::make_shared<State>(*state) : std::make_shared<State>();
    candidate->generation = generation;
    std::set<std::string> unique;
    constexpr size_t MaxBytes = 256ull * 1024 * 1024;
    for (const auto& update : updates)
    {
        if (!dynamicChunks.count(update.key) || !update.vertices || update.vertices->empty() ||
            !unique.insert(update.key).second)
            throw std::invalid_argument("Terrain vertex update has invalid chunk data");
        const auto existing = candidate->chunks.find(update.key);
        if (existing != candidate->chunks.end())
            candidate->bytes -= existing->second.vertices->size() * sizeof(Vertex);
        if (update.vertices->size() > (MaxBytes - candidate->bytes) / sizeof(Vertex))
            throw std::invalid_argument("Terrain vertex updates exceed byte budget");
        candidate->bytes += update.vertices->size() * sizeof(Vertex);
        candidate->chunks[update.key] = {
            update.vertices, std::make_shared<std::array<bool, 3>>(std::array<bool, 3>{})};
    }
    Prepared prepared;
    prepared.state = std::move(candidate);
    prepared.owner = identity;
    prepared.residencyRevision = residencyRevision;
    prepared.publicationRevision = publicationRevision;
    return prepared;
}

void TerrainVertexUpdateQueue::Commit(Prepared prepared)
{
    if (!prepared.state)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    // 防止卸载前或旧 publication 准备的批次复活已注销的 key/旧 ACK。
    if (prepared.owner != identity || prepared.residencyRevision != residencyRevision ||
        prepared.publicationRevision != publicationRevision)
        throw std::invalid_argument("Terrain vertex preparation is stale");
    state = std::move(prepared.state);
    ++publicationRevision;
}

void TerrainVertexUpdateQueue::Replay(
    uint64_t generation, uint32_t frameSlot, const QueueVertices& queue) const
{
    if (frameSlot >= 3 || !queue)
        return;
    std::shared_ptr<const State> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = state;
    }
    if (!snapshot || snapshot->generation != generation)
        return;
    for (const auto& entry : snapshot->chunks)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!state || state->generation != generation ||
                !dynamicChunks.count(entry.first))
                continue;
            const auto current = state->chunks.find(entry.first);
            if (current == state->chunks.end() ||
                current->second.consumed != entry.second.consumed)
                continue;
            if ((*entry.second.consumed)[frameSlot])
                continue;
        }
        if (queue(entry.first, *entry.second.vertices))
        {
            std::lock_guard<std::mutex> lock(mutex);
            (*entry.second.consumed)[frameSlot] = true;
        }
    }
}
}

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct Vertex;

namespace VL
{
// World-local 冻结顶点交接；结构变更由 GT 安全点负责，RT 不持有编辑器回调。
class TerrainVertexUpdateQueue
{
    struct Chunk
    {
        std::shared_ptr<const std::vector<Vertex>> vertices;
        std::shared_ptr<std::array<bool, 3>> consumed;
    };
    struct State
    {
        uint64_t generation = 0;
        size_t bytes = 0;
        std::map<std::string, Chunk> chunks;
    };

public:
    struct Update
    {
        std::string key;
        std::shared_ptr<const std::vector<Vertex>> vertices;
    };
    class Prepared
    {
        friend class TerrainVertexUpdateQueue;
        std::shared_ptr<const State> state;
        std::shared_ptr<const uint8_t> owner;
        uint64_t residencyRevision = 0;
        uint64_t publicationRevision = 0;
    };
    using QueueVertices = std::function<bool(
        const std::string&, const std::vector<Vertex>&)>;

    void RegisterDynamicChunk(std::string key);
    void UnregisterDynamicChunk(const std::string& key);
    uint64_t ResidencyRevision() const;
    bool HasDynamicChunk(const std::string& key) const;
    Prepared Prepare(uint64_t generation, const std::vector<Update>& updates) const;
    void Commit(Prepared prepared);
    void Replay(uint64_t generation, uint32_t frameSlot, const QueueVertices& queue) const;

private:
    mutable std::mutex mutex;
    std::set<std::string> dynamicChunks;
    std::shared_ptr<const State> state;
    uint64_t residencyRevision = 0;
    uint64_t publicationRevision = 0;
    const std::shared_ptr<const uint8_t> identity = std::make_shared<const uint8_t>(0);
};
}

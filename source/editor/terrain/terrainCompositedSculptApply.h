#pragma once

#include <array>
#include <functional>
#include <map>
#include <mutex>

#include "editor/terrain/terrainEditSession.h"

namespace VL::Editor
{

// GT 从动态初载网格登记拓扑并提交冻结编辑；RT 每个实际 frame 重放最新顶点。
// 每个 placement 一个实例，不创建 GPU 对象，不把 World 指针交给 RT。
class TerrainCompositedSculptApply
{
public:
    using Commit = std::function<bool(const TerrainMeshUpdate&, std::string&)>;
    using QueueVertices = std::function<void(
        const std::string&, const std::vector<Vertex>&)>;
    using FrameQueueVertices = std::function<bool(
        const std::string&, const std::vector<Vertex>&)>;
    static constexpr uint32_t FrameSlotCount = 3;

    explicit TerrainCompositedSculptApply(
        size_t maxChunks = 4096, size_t maxBytes = 256 * 1024 * 1024);

    bool Register(
        uint64_t worldGeneration, const TerrainMeshUpdate& baseline,
        bool loadedWithDynamicVertices, std::string& error);
    bool Apply(
        uint64_t worldGeneration, const TerrainMeshUpdate& update,
        const Commit& commit, std::string& error);
    // 兼容无确认测试回放；生产路径使用 ReplayForFrame。
    void Replay(uint64_t worldGeneration, const QueueVertices& queue) const;
    // RT 当前槽成功消费才返回 true；隐藏/未解析/仅入队但未保证消费返回 false。
    // frameSlot 必须与实际动态 buffer 槽一致，集成方校验 runtime 的槽数等于 3。
    void ReplayForFrame(
        uint64_t worldGeneration, uint32_t frameSlot,
        const FrameQueueVertices& queue) const;
    void Reset();

private:
    struct Chunk
    {
        uint32_t chunkX = 0;
        uint32_t chunkZ = 0;
        uint32_t lodLevel = 0;
        std::string objectName;
        size_t vertexCount = 0;
        std::shared_ptr<const std::vector<uint32_t>> indices;
        std::shared_ptr<const std::vector<uint32_t>> patchIndices;
        std::shared_ptr<const std::vector<Vertex>> latest;
        uint64_t version = 0;
        std::shared_ptr<std::array<uint64_t, FrameSlotCount>> acknowledged;
    };
    struct State
    {
        uint64_t generation = 0;
        std::string placementId;
        std::string assetPath;
        std::map<std::string, Chunk> chunks;
        size_t bytes = 0;
    };

    const size_t maxChunks_;
    const size_t maxBytes_;
    mutable std::mutex mutex_;
    std::shared_ptr<const State> state_;
};

}

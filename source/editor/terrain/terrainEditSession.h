#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainCollisionTypes.h"
#include "terrain/terrainHeightmapEditor.h"
#include "vertexDataStruct.h"

namespace VL
{
namespace Editor
{

struct TerrainRaycastHit
{
    bool valid = false;
    Eigen::Vector2f uv = Eigen::Vector2f::Zero();
    Eigen::Vector3f world = Eigen::Vector3f::Zero();
    float distanceCm = 0.0f;
};

struct TerrainChunkMeshUpdate
{
    uint32_t chunkX = 0;
    uint32_t chunkZ = 0;
    uint32_t lodLevel = 0;
    std::string resourceKey;
    std::string objectName;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
};

struct TerrainMeshUpdate
{
    std::string placementId;
    std::string actorIdentity;
    std::string assetPath;
    TerrainAssetDesc desc;
    std::shared_ptr<const HostImage> heightmap;
    std::vector<TerrainChunkMeshUpdate> chunks;
};

// 一次 Landscape 雕刻会话。纯 CPU，不持有 World/Vulkan；UI 在 preview 阶段
// 调用 ApplyBrush，结束时只提交一条 undo。历史与 dirty 都是会话自己的状态。
class TerrainEditSession
{
public:
    TerrainEditSession() = default;

    static bool Create(
        const TerrainCollisionInstance& instance,
        TerrainEditSession& out,
        std::string& error);

    const std::string& PlacementId() const noexcept { return placementId_; }
    const std::string& ActorIdentity() const noexcept { return actorIdentity_; }
    const TerrainAssetDesc& Desc() const noexcept { return desc_; }
    const TerrainCollisionPlacement& Placement() const noexcept
    {
        return placement_;
    }
    bool IsStrokeActive() const noexcept { return strokeActive_; }
    uint64_t Revision() const noexcept;

    // BeginStroke 固定“本笔开始前”的高度快照；ApplyBrush 只改 preview，
    // EndStroke 有变化才 push 一条历史。一次 stroke 最多一条 undo。
    bool BeginStroke();
    bool ApplyBrush(const TerrainSculptBrush& brush);
    bool EndStroke();
    void CancelStroke();

    bool CanUndo() const noexcept;
    bool CanRedo() const noexcept;
    bool Undo();
    bool Redo();

    // 世界射线 → 地形局部（position/rotation/scale，YXZ 旋转），对高度场做
    // 步进 + 二分求交。返回 UV、世界命中点与世界距离；退化/未命中 invalid。
    TerrainRaycastHit Raycast(
        const Eigen::Vector3f& origin,
        const Eigen::Vector3f& worldDirection) const;

    Eigen::Vector3f WorldPoint(float u, float v) const;

    // 自上次 ResetDirty 以来 dirty chunks 的全部 LOD 网格。heightmap 是当前
    // 编辑后的完整 R32_FLOAT 快照，resourceKey/objectName 与渲染加载期一致。
    TerrainMeshUpdate BuildUpdate() const;
    void ResetDirty();
    bool SaveHeightmap(const std::filesystem::path& path, std::string& error) const;

private:
    void EnsureHeightBounds() const;
    void MarkAllDirty();

    std::unique_ptr<TerrainHeightmapEditor> editor_;
    TerrainAssetDesc desc_;
    TerrainCollisionPlacement placement_;
    std::string placementId_;
    std::string actorIdentity_;

    bool strokeActive_ = false;
    bool strokeChanged_ = false;
    std::vector<float> strokeStartSamples_;
    std::deque<std::vector<float>> undoStack_;
    std::deque<std::vector<float>> redoStack_;

    // Raycast 的 AABB 只用于限定参数范围，实际求交仍按高度场进行。
    mutable bool heightBoundsDirty_ = true;
    mutable float heightMinCm_ = 0.0f;
    mutable float heightMaxCm_ = 0.0f;
};

} // namespace Editor
} // namespace VL

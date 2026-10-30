#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Eigen/Dense>

#include "core/runtimeResult.h"
#include "world/world.h"
#include "world/worldSnapshot.h"

namespace VL
{

struct WorldSnapshotBuildDesc
{
    uint64_t worldGeneration = 0;
    uint64_t frameIndex = 0;
    int debugViewMode = 0;
    float environmentIntensity = 1.0f;
    bool hasSelectedDraw = false;
    bool selectedAllMaterialSlots = false;
    RuntimeId selectedObjectId = 0;
    uint32_t selectedMaterialSlotIndex = 0;
};

// 地形 LOD 组只读统计：控制台命令读取，渲染路径不消费。
struct TerrainLodGroupStats
{
    std::string groupId;
    uint32_t level = 0;
    uint32_t levelCount = 1;
    float distanceCm = 0.0f;
    uint64_t switchCount = 0;
};

struct TerrainLodStatsSnapshot
{
    bool valid = false;
    uint64_t worldGeneration = 0;
    uint64_t frameIndex = 0;
    uint32_t groupCount = 0;
    uint32_t switchedLastFrame = 0;
    std::vector<TerrainLodGroupStats> groups;
};

// 稳定 RuntimeId 的派生规则由快照构建器拥有：编辑器大纲必须复用同一个对象身份，
// 否则大纲选择无法命中 RenderScene 里的同一个 draw packet，选择描边会静默失效。
RuntimeId WorldObjectRuntimeId(const World& world, const std::string& objectName);
// 地形 LOD 组的所有级共享同一个 RuntimeId（由 lodGroupId 派生），编辑器
// section 与快照 draw packet 必须用这一个入口，否则级切换会让描边失配。
RuntimeId WorldMeshObjectRuntimeId(
    const World& world,
    const std::string& objectName,
    const WorldMeshObject& meshObject);

// 编辑器大纲只读展示灯光参数，必须与渲染快照完全相同。这四个入口复用
// WorldSnapshotBuilder 的同一份 World→LightSnapshot 映射，避免出现第二份规则。
LightSnapshot BuildWorldDirectionalLightSnapshot(
    const World& world, const std::string& lightName, const DirectionalLight& light);
LightSnapshot BuildWorldPointLightSnapshot(
    const World& world, const std::string& lightName, const PointLight& light);
LightSnapshot BuildWorldSpotLightSnapshot(
    const World& world, const std::string& lightName, const SpotLight& light);
LightSnapshot BuildWorldSquareAreaLightSnapshot(
    const World& world, const std::string& lightName, const WorldSquareAreaLight& light);

// Exports the immutable render-facing data for one World frame. This builder
// reads World only; renderer cache to World assembly belongs to WorldBuilder.
//
// 逐对象可见性的唯一过滤点：不可见的网格与灯光在这里就被丢弃，不进入
// meshDraws / lights。渲染侧消费者（出图、拾取、MaterialInstance 上下文、阴影 pass）
// 全部以这份快照为唯一来源，因此不要在渲染器里再加第二处 visible 判断——
// 那会写出"画不出来但仍能拾取"这类不一致。新增可隐藏的 World 内容时，
// 必须同时补 World 的写入端、RuntimeCommandExecutor 的路由和这里的过滤。
class WorldSnapshotBuilder
{
public:
    RuntimeResult<WorldSnapshot> Build(
        const World& world,
        const WorldSnapshotBuildDesc& desc);
    void Reset();
    void Swap(WorldSnapshotBuilder& other) noexcept;
    const TerrainLodStatsSnapshot& GetTerrainLodStats() const
    { return lodStats; }

private:
    // 每个 LOD 组的当前级与切换次数属于渲染历史，和 previousObjectModels
    // 一样只存在于 builder，不写回 World 的运行时状态。
    struct LodGroupState
    {
        uint32_t level = 0;
        uint32_t levelCount = 1;
        uint64_t switchCount = 0;
        float lastDistanceCm = 0.0f;
        bool initialized = false;
    };

    std::optional<uint64_t> previousWorldGeneration;
    std::optional<Eigen::Matrix4f> previousViewProjection;
    // Previous object transforms are renderer input history, so the builder
    // caches them by stable object id instead of asking RenderSystem to write
    // back into mutable World objects after command submission.
    std::unordered_map<RuntimeId, Eigen::Matrix4f> previousObjectModels;
    std::unordered_map<std::string, LodGroupState> lodGroupStates;
    TerrainLodStatsSnapshot lodStats;
};

} // namespace VL

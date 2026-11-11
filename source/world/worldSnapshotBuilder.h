#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Eigen/Dense>

#include "core/runtimeResult.h"
#include "terrain/terrainAssetTypes.h"
#include "world/world.h"
#include "world/worldSnapshot.h"

namespace VL
{

struct WorldSnapshotBuildDesc
{
    uint64_t worldGeneration = 0;
    uint64_t frameIndex = 0;
    float simulationDeltaSeconds = 0.0f;
    float gameSimulationDeltaSeconds = 0.0f;
    float hairSimulationDeltaSeconds = 0.0f;
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
    // 本组实际驻留（World 中存在网格）的级位图；加载期几何驻留会把窗口外
    // 的细 LOD 直接省掉，因此它可能不含低位的级。
    uint32_t residentLevelMask = 0;
    // 本帧真正输出的级。目标级不驻留时会回退到 resident 的更粗一级。
    uint32_t drawnLevel = 0;
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
    // 本帧实际出图的 chunk 数（每组一个）与按级分布。
    uint32_t drawnChunks = 0;
    // 本帧全部 mesh draw 数（含非地形、无 LOD 组的地形 chunk）。单级地形
    // 没有 LOD 组，drawnChunks 会是 0，这一项用于对照“确实画了”。
    uint32_t totalMeshDraws = 0;
    std::array<uint32_t, TerrainMaxLodLevels> drawnByLevel{};
    // World 里各级 mesh 对象数（= 加载期实际创建的 chunk 数），与“全部创建”
    // 的差值由 terrainchunk stats 的加载期统计给出。
    std::array<uint32_t, TerrainMaxLodLevels> residentObjectsByLevel{};
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
        // 本组实际驻留的级位图与本帧输出的级（目标级缺失时为更粗一级）。
        uint32_t residentLevelMask = 0;
        uint32_t drawnLevel = 0;
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
    // 同一 World 内 namespace 字符串不会变化；缓存 objectName/lodGroupId ->
    // RuntimeId，避免每帧对每个 LOD draw 重建字符串并做 FNV。
    std::unordered_map<std::string, RuntimeId> objectRuntimeIds;
    std::unordered_map<std::string, LodGroupState> lodGroupStates;
    TerrainLodStatsSnapshot lodStats;
};

} // namespace VL

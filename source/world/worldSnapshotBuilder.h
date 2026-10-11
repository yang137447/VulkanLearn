#pragma once

#include <optional>
#include <unordered_map>

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

// 稳定 RuntimeId 的派生规则由快照构建器拥有：编辑器大纲必须复用同一个对象身份，
// 否则大纲选择无法命中 RenderScene 里的同一个 draw packet，选择描边会静默失效。
RuntimeId WorldObjectRuntimeId(const World& world, const std::string& objectName);

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
class WorldSnapshotBuilder
{
public:
    RuntimeResult<WorldSnapshot> Build(
        const World& world,
        const WorldSnapshotBuildDesc& desc);
    void Reset();
    void Swap(WorldSnapshotBuilder& other) noexcept;

private:
    std::optional<uint64_t> previousWorldGeneration;
    std::optional<Eigen::Matrix4f> previousViewProjection;
    // Previous object transforms are renderer input history, so the builder
    // caches them by stable object id instead of asking RenderSystem to write
    // back into mutable World objects after command submission.
    std::unordered_map<RuntimeId, Eigen::Matrix4f> previousObjectModels;
};

} // namespace VL

#include "world/worldSnapshotBuilder.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "commonFunction.h"
#include "sceneNode.h"
#include "terrain/terrainAssetTypes.h"

namespace VL
{
namespace
{

RuntimeId StableRuntimeId(const std::string& key)
{
    constexpr RuntimeId fnvOffset = 14695981039346656037ull;
    constexpr RuntimeId fnvPrime = 1099511628211ull;

    RuntimeId hash = fnvOffset;
    for (unsigned char value : key)
    {
        hash ^= value;
        hash *= fnvPrime;
    }
    return hash;
}

ResourceHandle MakeResourceHandle(const std::string& key, uint64_t generation = 0)
{
    ResourceHandle handle;
    handle.key = key;
    handle.generation = generation;
    return handle;
}

std::array<Eigen::Vector3f, 8> BuildWorldCorners(
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    const Eigen::Matrix4f& model)
{
    std::array<Eigen::Vector3f, 8> corners;
    int index = 0;

    for (float x : {localMin.x(), localMax.x()})
    {
        for (float y : {localMin.y(), localMax.y()})
        {
            for (float z : {localMin.z(), localMax.z()})
            {
                Eigen::Vector4f localCorner(x, y, z, 1.0f);
                corners[index++] = (model * localCorner).head<3>();
            }
        }
    }

    return corners;
}

void ComputeWorldBounds(
    const std::array<Eigen::Vector3f, 8>& corners,
    Eigen::Vector3f& outMin,
    Eigen::Vector3f& outMax)
{
    outMin = Eigen::Vector3f(
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    outMax = Eigen::Vector3f(
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest());

    for (const Eigen::Vector3f& corner : corners)
    {
        outMin = outMin.cwiseMin(corner);
        outMax = outMax.cwiseMax(corner);
    }
}

bool TerrainLodGroupStatsLess(
    const TerrainLodGroupStats& left,
    const TerrainLodGroupStats& right)
{
    return left.groupId < right.groupId;
}

std::string BuildObjectRuntimeKey(const World& world, const std::string& objectName)
{
    return world.GetScenePath() + "|object|" + objectName;
}

std::string BuildLightRuntimeKey(const World& world, const std::string& lightName)
{
    return world.GetScenePath() + "|light|" + lightName;
}

// 取位图里最高的置位级；空位图返回 TerrainMaxLodLevels（表示“没有任何级”）。
uint32_t HighestResidentLevel(uint32_t mask)
{
    for (uint32_t level = TerrainMaxLodLevels; level > 0; --level)
    {
        if ((mask & (1u << (level - 1))) != 0)
            return level - 1;
    }
    return TerrainMaxLodLevels;
}

MeshDrawSnapshot BuildMeshDrawSnapshot(
    const std::string& objectName,
    RuntimeId objectId,
    const WorldMeshObject& meshObject,
    const Eigen::Matrix4f& previousModel,
    bool selected)
{
    MeshDrawSnapshot snapshot;
    snapshot.skinPose = meshObject.skinPose;
    snapshot.objectId = objectId;
    snapshot.selected = selected;
    snapshot.debugName = meshObject.debugName.empty() ? objectName : meshObject.debugName;
    snapshot.sceneObjectIdentity = meshObject.sceneObjectIdentity.empty()
        ? objectName
        : meshObject.sceneObjectIdentity;
    snapshot.materialSlotIndex = meshObject.materialSlotIndex;
    snapshot.materialSlotName = meshObject.materialSlotName;
    snapshot.model = meshObject.model;
    snapshot.previousModel = previousModel;
    snapshot.mesh = MakeResourceHandle(meshObject.meshKey);
    snapshot.material = MakeResourceHandle(meshObject.materialKey);
    snapshot.materialInstance = MakeResourceHandle(meshObject.materialInstanceKey);
    snapshot.speedTreeWindProfileKey = meshObject.speedTreeWindProfileKey;
    snapshot.terrainFeedbackEligible =
        meshObject.terrainFeedbackEligible;
    snapshot.lodGroupId = meshObject.lodGroupId;
    snapshot.lodLevel = meshObject.lodLevel;

    const std::array<Eigen::Vector3f, 8> worldCorners = BuildWorldCorners(
        meshObject.localBoundsMin,
        meshObject.localBoundsMax,
        snapshot.model);
    ComputeWorldBounds(worldCorners, snapshot.worldBoundsMin, snapshot.worldBoundsMax);

    return snapshot;
}

LightSnapshot BuildDirectionalLightSnapshot(
    const World& world,
    const std::string& lightName,
    const DirectionalLight& light)
{
    LightSnapshot snapshot;
    snapshot.lightId = StableRuntimeId(BuildLightRuntimeKey(world, lightName));
    snapshot.type = LightSnapshotType::Directional;
    snapshot.color = light.GetColor();
    snapshot.intensity = light.GetIntensity();
    snapshot.position = light.GetPosition();
    snapshot.direction = light.GetForwardVector();
    snapshot.worldToLight = CommonFunction::RotationToMatrix(light.GetRotation()).block<3, 3>(0, 0).transpose();
    return snapshot;
}

LightSnapshot BuildPointLightSnapshot(
    const World& world,
    const std::string& lightName,
    const PointLight& light)
{
    LightSnapshot snapshot;
    snapshot.lightId = StableRuntimeId(BuildLightRuntimeKey(world, lightName));
    snapshot.type = LightSnapshotType::Point;
    snapshot.color = light.GetColor();
    snapshot.intensity = light.GetIntensity();
    snapshot.position = light.GetPosition();
    snapshot.radius = light.GetRadius();
    return snapshot;
}

LightSnapshot BuildSpotLightSnapshot(
    const World& world,
    const std::string& lightName,
    const SpotLight& light)
{
    LightSnapshot snapshot;
    snapshot.lightId = StableRuntimeId(BuildLightRuntimeKey(world, lightName));
    snapshot.type = LightSnapshotType::Spot;
    snapshot.color = light.GetColor();
    snapshot.intensity = light.GetIntensity();
    snapshot.position = light.GetPosition();
    snapshot.radius = light.GetRadius();
    snapshot.direction = light.GetForwardVector();
    snapshot.worldToLight = CommonFunction::RotationToMatrix(light.GetRotation()).block<3, 3>(0, 0).transpose();
    snapshot.coneAngleOuter = light.GetConeAngleOuter();
    snapshot.coneAngleInner = light.GetConeAngleInner();
    return snapshot;
}

LightSnapshot BuildSquareAreaLightSnapshot(
    const World& world,
    const std::string& lightName,
    const WorldSquareAreaLight& light)
{
    LightSnapshot snapshot;
    snapshot.lightId = StableRuntimeId(BuildLightRuntimeKey(world, lightName));
    snapshot.type = LightSnapshotType::SquareArea;
    snapshot.color = light.color;
    snapshot.intensity = light.intensity;
    snapshot.position = light.position;
    snapshot.direction = light.normal;
    snapshot.squareSize = light.size;
    return snapshot;
}

EnvironmentSnapshot BuildEnvironmentSnapshot(
    const World& world,
    const WorldSnapshotBuildDesc& desc)
{
    EnvironmentSnapshot snapshot;
    const WorldEnvironment& environment = world.GetEnvironment();

    snapshot.type = environment.type;
    snapshot.intensity = environment.type == EnvironmentType::None
        ? 0.0f : environment.intensity * desc.environmentIntensity;
    snapshot.skyParameters = environment.skyParameters;

    if (environment.type == EnvironmentType::Hdri && !environment.hdrPath.empty())
    {
        const std::string cubeSize = std::to_string(environment.cubeSize);
        snapshot.cube = MakeResourceHandle("environment/cube|" + environment.hdrPath + "|" + cubeSize);
        snapshot.prefilteredCube = MakeResourceHandle("environment/prefilter|" + environment.hdrPath + "|" + cubeSize);
    }

    snapshot.brdfLut = MakeResourceHandle("global/brdfLut");

    return snapshot;
}

} // namespace

RuntimeId WorldObjectRuntimeId(const World& world, const std::string& objectName)
{
    return StableRuntimeId(BuildObjectRuntimeKey(world, objectName));
}

RuntimeId WorldMeshObjectRuntimeId(
    const World& world,
    const std::string& objectName,
    const WorldMeshObject& meshObject)
{
    return StableRuntimeId(BuildObjectRuntimeKey(
        world,
        meshObject.lodGroupId.empty()
            ? objectName
            : meshObject.lodGroupId));
}

LightSnapshot BuildWorldDirectionalLightSnapshot(
    const World& world, const std::string& lightName, const DirectionalLight& light)
{
    return BuildDirectionalLightSnapshot(world, lightName, light);
}

LightSnapshot BuildWorldPointLightSnapshot(
    const World& world, const std::string& lightName, const PointLight& light)
{
    return BuildPointLightSnapshot(world, lightName, light);
}

LightSnapshot BuildWorldSpotLightSnapshot(
    const World& world, const std::string& lightName, const SpotLight& light)
{
    return BuildSpotLightSnapshot(world, lightName, light);
}

LightSnapshot BuildWorldSquareAreaLightSnapshot(
    const World& world, const std::string& lightName, const WorldSquareAreaLight& light)
{
    return BuildSquareAreaLightSnapshot(world, lightName, light);
}

RuntimeResult<WorldSnapshot> WorldSnapshotBuilder::Build(
    const World& world,
    const WorldSnapshotBuildDesc& desc)
{
    if (desc.worldGeneration != 0 && desc.worldGeneration != world.GetGeneration())
    {
        return RuntimeResult<WorldSnapshot>::Failure(MakeRuntimeError(
            "WorldSnapshot.GenerationMismatch",
            "Cannot build a world snapshot because the requested generation does not match the active World.",
            world.GetScenePath()));
    }

    if (!world.GetCamera())
    {
        return RuntimeResult<WorldSnapshot>::Failure(MakeRuntimeError(
            "WorldSnapshot.NoCamera",
            "Cannot build a world snapshot because the active scene has no camera.",
            world.GetScenePath()));
    }

    if (!previousWorldGeneration.has_value() || *previousWorldGeneration != world.GetGeneration())
    {
        previousViewProjection.reset();
        previousObjectModels.clear();
        lodGroupStates.clear();
        lodStats = TerrainLodStatsSnapshot{};
        previousWorldGeneration = world.GetGeneration();
    }

    WorldSnapshot snapshot;
    snapshot.worldGeneration = world.GetGeneration();
    snapshot.frameIndex = desc.frameIndex;
    snapshot.debugViewMode = desc.debugViewMode;

    Camera& camera = *world.GetRenderCamera();
    snapshot.camera.exposure = camera.GetExposure();
    snapshot.camera.view = camera.GetViewMatrix();
    snapshot.camera.projection = camera.GetProjectionMatrix();
    snapshot.camera.viewProjection = snapshot.camera.projection * snapshot.camera.view;
    snapshot.camera.previousViewProjection =
        previousViewProjection.value_or(snapshot.camera.viewProjection);
    snapshot.camera.position = camera.GetPosition();
    snapshot.camera.forward = camera.GetForwardVector();
    snapshot.camera.right = camera.GetRightVector();
    snapshot.camera.up = camera.GetUpVector();
    snapshot.camera.horizontalFovDegrees = camera.GetHFOV();
    snapshot.camera.clipNear = camera.GetClipNear();
    snapshot.camera.clipFar = camera.GetClipFar();

    // 逐对象可见性在这里过滤，不在渲染器里再判一次：WorldSnapshot 是渲染侧
    // 唯一的对象集合（meshDraws 既出图也供编辑器拾取、MaterialInstance 上下文和
    // 阴影 pass 复用），在构造处丢弃不可见对象，渲染路径就不存在"忘了判断"的分支，
    // 也不会出现"画出来了但仍可拾取"这类不一致。不可见对象同时不进入
    // currentObjectModels，重新显示时该帧退回自身模型（没有速度向量），是刻意的。
    snapshot.meshDraws.reserve(world.GetMeshObjects().size());
    std::unordered_map<RuntimeId, Eigen::Matrix4f> currentObjectModels;
    currentObjectModels.reserve(world.GetMeshObjects().size());
    std::vector<TerrainLodGroupStats> lodGroupStats;
    std::unordered_set<std::string> emittedLodGroups;
    std::unordered_set<std::string> visitedLodGroups;
    // 选择级缺失时的兜底代表（优先 LOD0）；正常情况下不会用到。
    std::unordered_map<std::string, const WorldMeshObject*> lodGroupRepresentatives;
    // 加载期几何驻留会让窗口外的细 LOD 根本不存在。先扫一遍收集每个组
    // 实际存在的级位图：它是“resident”的唯一事实来源（World 里没有的级
    // 就是没驻留），既能决定兜底级，也能给 stats 报各级对象数。
    std::unordered_map<std::string, uint32_t> lodGroupLevelMasks;
    std::array<uint32_t, TerrainMaxLodLevels> residentObjectsByLevel{};
    for (const auto& [objectName, meshObject] : world.GetMeshObjects())
    {
        if (!meshObject.visible || meshObject.lodGroupId.empty())
            continue;
        if (meshObject.lodLevel >= TerrainMaxLodLevels)
            continue;
        lodGroupLevelMasks[meshObject.lodGroupId] |=
            1u << meshObject.lodLevel;
        ++residentObjectsByLevel[meshObject.lodLevel];
    }
    uint32_t drawnChunks = 0;
    std::array<uint32_t, TerrainMaxLodLevels> drawnByLevel{};
    uint32_t lodSwitchedLastFrame = 0;
    for (const auto& [objectName, meshObject] : world.GetMeshObjects())
    {
        if (!meshObject.visible) continue;

        // 同一 LOD 组的各级共用 RuntimeId：级切换不会改变编辑器选择/描边，
        // 也避免 previousObjectModels 把同一 chunk 的多级当成不同对象。
        const bool hasLodGroup = !meshObject.lodGroupId.empty();
        const RuntimeId objectId =
            WorldMeshObjectRuntimeId(world, objectName, meshObject);

        const auto previousModelIt = previousObjectModels.find(objectId);
        const Eigen::Matrix4f previousModel =
            previousModelIt != previousObjectModels.end()
                ? previousModelIt->second
                : meshObject.model;

        bool selected = desc.hasSelectedDraw &&
            desc.selectedObjectId == objectId &&
            (desc.selectedAllMaterialSlots ||
             desc.selectedMaterialSlotIndex == meshObject.materialSlotIndex);

        if (!hasLodGroup)
        {
            snapshot.meshDraws.push_back(BuildMeshDrawSnapshot(
                objectName,
                objectId,
                meshObject,
                previousModel,
                selected));
            currentObjectModels[objectId] = meshObject.model;
            continue;
        }

        auto stateIt = lodGroupStates.find(meshObject.lodGroupId);
        if (stateIt == lodGroupStates.end())
        {
            LodGroupState state;
            state.levelCount = meshObject.lodLevelCount > 0
                ? meshObject.lodLevelCount
                : 1;
            stateIt = lodGroupStates.emplace(
                meshObject.lodGroupId,
                state).first;
        }
        LodGroupState& state = stateIt->second;
        visitedLodGroups.insert(meshObject.lodGroupId);

        // 组 AABB 来自 LOD0，与当前级无关；每帧重新投影，因此 World 内
        // 的对象变换（若有）不需要额外通知。
        const std::array<Eigen::Vector3f, 8> groupCorners = BuildWorldCorners(
            meshObject.lodGroupBoundsMin,
            meshObject.lodGroupBoundsMax,
            meshObject.model);
        Eigen::Vector3f groupWorldMin;
        Eigen::Vector3f groupWorldMax;
        ComputeWorldBounds(groupCorners, groupWorldMin, groupWorldMax);
        const float distanceCm = TerrainLodDistanceToAabbCm(
            snapshot.camera.position,
            groupWorldMin,
            groupWorldMax);
        const uint32_t previousLevel =
            state.initialized ? state.level : 0;
        const uint32_t nextLevel = TerrainSelectLodLevelHysteresis(
            distanceCm,
            meshObject.lodDistancesCm,
            state.initialized,
            previousLevel);
        if (state.initialized && nextLevel != state.level)
        {
            ++state.switchCount;
            ++lodSwitchedLastFrame;
        }
        state.level = nextLevel;
        state.lastDistanceCm = distanceCm;
        state.initialized = true;

        // 兜底代表优先 LOD0：遍历顺序不确定，所以只要见到 LOD0 就覆盖。
        const auto representativeIt =
            lodGroupRepresentatives.find(meshObject.lodGroupId);
        if (representativeIt == lodGroupRepresentatives.end() ||
            meshObject.lodLevel == 0)
        {
            lodGroupRepresentatives[meshObject.lodGroupId] = &meshObject;
        }

        // 目标级没驻留时不能留空洞：沿更粗方向找最近的 resident 级。
        // 加载期驻留保证最粗一级一定存在，因此这个循环最坏也会停在
        // coarsest；若 World 因资产缺陷一级都没有，才走下面的代表兜底。
        const uint32_t residentMask =
            lodGroupLevelMasks[meshObject.lodGroupId];
        uint32_t drawnLevel = std::min(state.level, TerrainMaxLodLevels);
        while (drawnLevel < TerrainMaxLodLevels &&
            (residentMask & (1u << drawnLevel)) == 0)
        {
            ++drawnLevel;
        }
        if (drawnLevel >= TerrainMaxLodLevels)
            drawnLevel = HighestResidentLevel(residentMask);
        if (drawnLevel >= TerrainMaxLodLevels)
            continue;
        state.drawnLevel = drawnLevel;
        state.residentLevelMask = residentMask;

        if (meshObject.lodLevel != drawnLevel)
            continue;

        emittedLodGroups.insert(meshObject.lodGroupId);
        ++drawnChunks;
        ++drawnByLevel[drawnLevel];
        snapshot.meshDraws.push_back(BuildMeshDrawSnapshot(
            objectName,
            objectId,
            meshObject,
            previousModel,
            selected));
        currentObjectModels[objectId] = meshObject.model;
    }

    for (const std::string& groupId : visitedLodGroups)
    {
        const auto stateIt = lodGroupStates.find(groupId);
        if (stateIt == lodGroupStates.end())
            continue;
        const LodGroupState& state = stateIt->second;
        TerrainLodGroupStats groupStats;
        groupStats.groupId = groupId;
        groupStats.level = state.level;
        groupStats.levelCount = state.levelCount;
        groupStats.residentLevelMask = state.residentLevelMask;
        groupStats.drawnLevel = state.drawnLevel;
        groupStats.distanceCm = state.lastDistanceCm;
        groupStats.switchCount = state.switchCount;
        lodGroupStats.push_back(std::move(groupStats));
    }

    // 防御性兜底：如果某组缺少当前级的可见网格（资产/加载缺陷），输出
    // LOD0 代表而不是留下空洞。统计仍记录选择级，便于从命令行发现异常。
    for (const auto& [groupId, representative] : lodGroupRepresentatives)
    {
        if (emittedLodGroups.find(groupId) != emittedLodGroups.end())
            continue;
        if (representative == nullptr || !representative->visible)
            continue;
        const RuntimeId objectId = WorldMeshObjectRuntimeId(
            world,
            representative->debugName,
            *representative);
        const auto previousModelIt = previousObjectModels.find(objectId);
        const Eigen::Matrix4f previousModel =
            previousModelIt != previousObjectModels.end()
                ? previousModelIt->second
                : representative->model;
        const bool selected = desc.hasSelectedDraw &&
            desc.selectedObjectId == objectId &&
            (desc.selectedAllMaterialSlots ||
             desc.selectedMaterialSlotIndex ==
                 representative->materialSlotIndex);
        snapshot.meshDraws.push_back(BuildMeshDrawSnapshot(
            representative->debugName,
            objectId,
            *representative,
            previousModel,
            selected));
        // 代表兜底也算一次出图；它的级以实际网格为准。
        ++drawnChunks;
        if (representative->lodLevel < TerrainMaxLodLevels)
            ++drawnByLevel[representative->lodLevel];
        currentObjectModels[objectId] = representative->model;
    }

    const size_t lightCount =
        world.GetDirectionalLights().size() +
        world.GetPointLights().size() +
        world.GetSpotLights().size() +
        world.GetSquareAreaLights().size() + world.GetScriptLights().size();
    snapshot.lights.reserve(lightCount);

    for (const auto& [lightName, light] : world.GetDirectionalLights())
    {
        if (!light->IsVisible()) continue;
        snapshot.lights.push_back(BuildDirectionalLightSnapshot(world, lightName, *light));
    }
    for (const auto& [lightName, light] : world.GetPointLights())
    {
        if (!light->IsVisible()) continue;
        snapshot.lights.push_back(BuildPointLightSnapshot(world, lightName, *light));
    }
    for (const auto& [lightName, light] : world.GetSpotLights())
    {
        if (!light->IsVisible()) continue;
        snapshot.lights.push_back(BuildSpotLightSnapshot(world, lightName, *light));
    }
    for (const auto& [lightName, light] : world.GetSquareAreaLights())
    {
        if (!light.visible) continue;
        snapshot.lights.push_back(BuildSquareAreaLightSnapshot(world, lightName, light));
    }
    for (const auto& [identity, light] : world.GetScriptLights())
    {
        auto frozen = light;
        frozen.lightId = StableRuntimeId(BuildLightRuntimeKey(world, identity));
        snapshot.lights.push_back(std::move(frozen));
    }

    snapshot.environment = BuildEnvironmentSnapshot(world, desc);
    previousViewProjection = snapshot.camera.viewProjection;
    previousObjectModels = std::move(currentObjectModels);

    std::sort(
        lodGroupStats.begin(),
        lodGroupStats.end(),
        TerrainLodGroupStatsLess);
    lodStats.valid = true;
    lodStats.worldGeneration = world.GetGeneration();
    lodStats.frameIndex = desc.frameIndex;
    lodStats.groupCount =
        static_cast<uint32_t>(lodGroupStats.size());
    lodStats.switchedLastFrame = lodSwitchedLastFrame;
    lodStats.drawnChunks = drawnChunks;
    lodStats.totalMeshDraws =
        static_cast<uint32_t>(snapshot.meshDraws.size());
    lodStats.drawnByLevel = drawnByLevel;
    lodStats.residentObjectsByLevel = residentObjectsByLevel;
    lodStats.groups = std::move(lodGroupStats);

    return RuntimeResult<WorldSnapshot>::Success(std::move(snapshot));
}

void WorldSnapshotBuilder::Reset()
{
    previousWorldGeneration.reset();
    previousViewProjection.reset();
    previousObjectModels.clear();
    lodGroupStates.clear();
    lodStats = TerrainLodStatsSnapshot{};
}

void WorldSnapshotBuilder::Swap(
    WorldSnapshotBuilder& other) noexcept
{
    previousWorldGeneration.swap(
        other.previousWorldGeneration);
    previousViewProjection.swap(
        other.previousViewProjection);
    previousObjectModels.swap(
        other.previousObjectModels);
    lodGroupStates.swap(
        other.lodGroupStates);
    std::swap(lodStats, other.lodStats);
}

} // namespace VL

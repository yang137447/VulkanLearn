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

std::string BuildDecalRuntimeKey(const World& world, const std::string& decalName)
{
    return world.GetScenePath() + "|decal|" + decalName;
}

std::string BuildParticleRuntimeKey(
    const World& world,
    const std::string& particleSystemName)
{
    return world.GetScenePath() + "|particle|" + particleSystemName;
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
    bool selected,
    const WorldSnapshotBuildDesc& desc,
    const TressfxGroupPreviewState& groupPreview)
{
    MeshDrawSnapshot snapshot;
    snapshot.skinPose = meshObject.skinPose;
    snapshot.objectId = objectId;
    snapshot.selected = selected;
    snapshot.debugName = meshObject.debugName.empty() ? objectName : meshObject.debugName;
    snapshot.sceneObjectIdentity = meshObject.sceneObjectIdentity.empty()
        ? objectName
        : meshObject.sceneObjectIdentity;
    // 与 World::TickAnimations 的普通/蓝图归属规则一致；在 GT 冻结，
    // RT 仅消费值，不查询脚本宿主或从发丝对象身份反推来源时钟。
    snapshot.simulationDeltaSeconds =
        snapshot.sceneObjectIdentity.rfind("__blueprint/", 0) == 0
            ? desc.gameSimulationDeltaSeconds : desc.simulationDeltaSeconds;
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
    snapshot.isTerrain = meshObject.isTerrain;
    snapshot.lodGroupId = meshObject.lodGroupId;
    snapshot.lodLevel = meshObject.lodLevel;
    snapshot.windResponseProfileKey = meshObject.windResponseProfileKey;
    snapshot.tressfxPointsPerGuide = meshObject.tressfxPointsPerGuide;
    snapshot.tressfxSelectedGroup = groupPreview.selectedGroup;
    snapshot.tressfxGroupFlags =
        (groupPreview.groupColors ? TressfxGroupPreviewColors : 0u) |
        (groupPreview.solo ? TressfxGroupPreviewSolo : 0u);
    // 放置级网格：把模板键与通用 placementBinding 一起冻结，渲染线程在 resolve
    // 前据此克隆独立 RenderableObject；普通对象两者为空。
    snapshot.placementTemplateMeshKey = meshObject.placementTemplateMeshKey;
    snapshot.placementBinding = meshObject.placementBinding;

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
    snapshot.pulsePeriod = light.GetPulsePeriod();
    snapshot.pulseDuration = light.GetPulseDuration();
    snapshot.pulsePhase = light.GetPulsePhase();
    snapshot.pulsePeakIntensity = light.GetPulsePeakIntensity();
    snapshot.castShadows = light.GetCastShadows();
    snapshot.shadowBias = light.GetShadowBias();
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
    snapshot.castShadows = light.GetCastShadows();
    snapshot.shadowBias = light.GetShadowBias();
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
    // 体积雾是渲染期效果，不随全局环境强度缩放；由 fog.intensity 单独控制。
    snapshot.fog = environment.fog;

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
        objectRuntimeIds.clear();
        lodGroupStates.clear();
        lodStats = TerrainLodStatsSnapshot{};
        previousWorldGeneration = world.GetGeneration();
    }

    WorldSnapshot snapshot;
    snapshot.worldGeneration = world.GetGeneration();
    snapshot.frameIndex = desc.frameIndex;
    snapshot.simulationDeltaSeconds = desc.simulationDeltaSeconds;
    snapshot.hairSimulationDeltaSeconds = desc.hairSimulationDeltaSeconds;
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
    auto objectRuntimeId = [&](const std::string& objectName,
                               const WorldMeshObject& meshObject)
    {
        const std::string& key = meshObject.lodGroupId.empty()
            ? objectName
            : meshObject.lodGroupId;
        auto inserted = objectRuntimeIds.try_emplace(key, 0);
        if (inserted.second)
        {
            inserted.first->second = WorldMeshObjectRuntimeId(
                world, objectName, meshObject);
        }
        return inserted.first->second;
    };
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
        const auto representativeIt =
            lodGroupRepresentatives.find(meshObject.lodGroupId);
        if (representativeIt == lodGroupRepresentatives.end() ||
            meshObject.lodLevel == 0)
        {
            lodGroupRepresentatives[meshObject.lodGroupId] = &meshObject;
        }
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
            objectRuntimeId(objectName, meshObject);

        if (!hasLodGroup)
        {
            const auto previousModelIt = previousObjectModels.find(objectId);
            const Eigen::Matrix4f previousModel =
                previousModelIt != previousObjectModels.end()
                    ? previousModelIt->second
                    : meshObject.model;
            const bool selected = desc.hasSelectedDraw &&
                desc.selectedObjectId == objectId &&
                (desc.selectedAllMaterialSlots ||
                 desc.selectedMaterialSlotIndex == meshObject.materialSlotIndex);
            snapshot.meshDraws.push_back(BuildMeshDrawSnapshot(
                objectName,
                objectId,
                meshObject,
                previousModel,
                selected,
                desc,
                world.GetTressfxGroupPreview()));
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
        const bool firstVisit = visitedLodGroups.insert(
            meshObject.lodGroupId).second;
        if (firstVisit)
        {
            // 组 AABB 来自 LOD0，与当前级无关；每个组只投影一次。
            const std::array<Eigen::Vector3f, 8> groupCorners =
                BuildWorldCorners(
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

            // 目标级没驻留时不能留空洞：沿更粗方向找最近的 resident 级。
            // 加载期驻留保证最粗一级一定存在，因此这个循环最坏也会停在
            // coarsest；若 World 因资产缺陷一级都没有，才走下面的代表兜底。
            const uint32_t residentMask =
                lodGroupLevelMasks[meshObject.lodGroupId];
            uint32_t drawnLevel =
                std::min(state.level, TerrainMaxLodLevels);
            while (drawnLevel < TerrainMaxLodLevels &&
                (residentMask & (1u << drawnLevel)) == 0)
            {
                ++drawnLevel;
            }
            if (drawnLevel >= TerrainMaxLodLevels)
                drawnLevel = HighestResidentLevel(residentMask);
            state.drawnLevel = drawnLevel;
            state.residentLevelMask = residentMask;
        }

        if (state.drawnLevel >= TerrainMaxLodLevels)
            continue;
        if (meshObject.lodLevel != state.drawnLevel)
            continue;
        if (!emittedLodGroups.insert(meshObject.lodGroupId).second)
            continue;

        const auto previousModelIt = previousObjectModels.find(objectId);
        const Eigen::Matrix4f previousModel =
            previousModelIt != previousObjectModels.end()
                ? previousModelIt->second
                : meshObject.model;
        const bool selected = desc.hasSelectedDraw &&
            desc.selectedObjectId == objectId &&
            (desc.selectedAllMaterialSlots ||
             desc.selectedMaterialSlotIndex == meshObject.materialSlotIndex);
        ++drawnChunks;
        ++drawnByLevel[state.drawnLevel];
        snapshot.meshDraws.push_back(BuildMeshDrawSnapshot(
            objectName,
            objectId,
            meshObject,
            previousModel,
            selected,
            desc,
            world.GetTressfxGroupPreview()));
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
            selected,
            desc,
            world.GetTressfxGroupPreview()));
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

    snapshot.decals.reserve(world.GetDecals().size());
    for (const auto& [name, decal] : world.GetDecals())
    {
        if (!decal.visible) continue;
        DecalSnapshot decalSnapshot;
        decalSnapshot.decalId =
            StableRuntimeId(BuildDecalRuntimeKey(world, name));
        decalSnapshot.name = name;
        decalSnapshot.decalToWorld = decal.decalToWorld;
        decalSnapshot.color = decal.color;
        decalSnapshot.opacity = decal.opacity;
        decalSnapshot.edgeSoftness = decal.edgeSoftness;
        decalSnapshot.noiseStrength = decal.noiseStrength;
        decalSnapshot.randomSeed = decal.randomSeed;
        decalSnapshot.roughness = decal.roughness;
        decalSnapshot.metallic = decal.metallic;
        decalSnapshot.ambientOcclusion = decal.ambientOcclusion;
        decalSnapshot.emissive = decal.emissive;
        decalSnapshot.emissiveStrength = decal.emissiveStrength;
        decalSnapshot.responseMask = decal.responseMask;
        decalSnapshot.normalStrength = decal.normalStrength;
        decalSnapshot.albedoMap = decal.albedoMap;
        decalSnapshot.normalMap = decal.normalMap;
        decalSnapshot.ormMap = decal.ormMap;
        decalSnapshot.sortOrder = decal.sortOrder;
        decalSnapshot.visible = decal.visible;
        snapshot.decals.push_back(std::move(decalSnapshot));
    }

    snapshot.particleSystems.reserve(
        world.GetParticleSystems().size());
    for (const auto& [name, particleSystem] :
         world.GetParticleSystems())
    {
        ParticleSystemSnapshot snapshotSystem;
        snapshotSystem.particleSystemId =
            StableRuntimeId(BuildParticleRuntimeKey(world, name));
        snapshotSystem.name = name;
        snapshotSystem.assetPath = particleSystem.assetPath;
        snapshotSystem.asset = particleSystem.asset;
        bool attachmentResolved = true;
        snapshotSystem.model =
            world.ResolveParticleSystemModel(
                particleSystem, attachmentResolved);
        snapshotSystem.visible = particleSystem.visible;
        snapshotSystem.runtime = particleSystem.runtime;
        snapshotSystem.budget =
            world.GetEffectiveParticleBudget(name);
        snapshotSystem.telemetry = particleSystem.telemetry;
        snapshotSystem.telemetry.budgetedCapacity =
            snapshotSystem.budget.effectiveCapacity;
        snapshotSystem.userParameters = particleSystem.userParameters;
        snapshotSystem.attachment = particleSystem.attachment;
        snapshotSystem.attachment.resolved = attachmentResolved;
        snapshot.particleSystems.push_back(
            std::move(snapshotSystem));
    }

    // 数据通道是 World 级状态，快照必须拿走独立副本；渲染线程不得读取
    // 可变 World，也不能在下一帧被 Console/C# 写入时看到半更新内容。
    snapshot.particleDataChannels = world.GetParticleDataChannels();

    snapshot.environment = BuildEnvironmentSnapshot(world, desc);
    // 风是场景全局状态：冻结进快照后，头发/离线消费者看到的是同一代 World 的风，
    // 不会在渲染线程重新读取可能已被换代的 World。
    snapshot.wind = world.GetWind();
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
    objectRuntimeIds.clear();
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
    objectRuntimeIds.swap(
        other.objectRuntimeIds);
    lodGroupStates.swap(
        other.lodGroupStates);
    std::swap(lodStats, other.lodStats);
}

} // namespace VL

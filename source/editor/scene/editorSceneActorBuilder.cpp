#include "editor/scene/editorSceneActorBuilder.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "editor/ui/editorSceneActorTree.h"
#include "sceneNode.h"
#include "world/world.h"
#include "world/worldSnapshotBuilder.h"

namespace VL
{
namespace
{

using EditorUi::EditorSceneActorSnapshot;
using EditorUi::EditorSceneActorType;

// 一个场景对象（sceneObjectIdentity）聚合出的 mesh actor：section 按 World meshObjects
// 的键（对象名）排序，保证多 section 模型的行顺序不依赖 unordered_map 的遍历顺序。
struct MeshActorGroup
{
    std::string sceneObjectIdentity;
    std::vector<std::pair<std::string, const WorldMeshObject*>> sections;
};

std::string SceneObjectIdentity(const WorldMeshObject& object, const std::string& objectName)
{
    return object.sceneObjectIdentity.empty() ? objectName : object.sceneObjectIdentity;
}

std::vector<MeshActorGroup> CollectMeshGroups(const World& world)
{
    std::map<std::string, MeshActorGroup> sorted;
    for (const auto& entry : world.GetMeshObjects())
    {
        MeshActorGroup& group = sorted[SceneObjectIdentity(entry.second, entry.first)];
        group.sceneObjectIdentity = SceneObjectIdentity(entry.second, entry.first);
        group.sections.push_back({entry.first, &entry.second});
    }

    std::vector<MeshActorGroup> groups;
    groups.reserve(sorted.size());
    for (auto& entry : sorted)
    {
        std::sort(entry.second.sections.begin(), entry.second.sections.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
        groups.push_back(std::move(entry.second));
    }
    return groups;
}

EditorSceneActorSnapshot BuildMeshActor(const World& world, const MeshActorGroup& group)
{
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::Mesh;
    actor.worldGeneration = world.GetGeneration();
    actor.scenePath = world.GetScenePath();
    actor.identity = EditorUi::SceneActorIdentity(EditorSceneActorType::Mesh, group.sceneObjectIdentity);
    actor.displayName = group.sceneObjectIdentity;
    actor.sceneObjectIdentity = group.sceneObjectIdentity;
    actor.sections.reserve(group.sections.size());
    for (const auto& entry : group.sections)
    {
        const WorldMeshObject& object = *entry.second;
        EditorUi::EditorSceneMeshSectionSnapshot section;
        section.objectName = entry.first;
        // RuntimeId 必须由 WorldObjectRuntimeId 派生：它是选择描边命中 draw packet 的唯一依据。
        section.objectId = WorldObjectRuntimeId(world, entry.first);
        section.meshKey = object.meshKey;
        section.materialKey = object.materialKey;
        section.materialInstancePath = object.materialInstanceKey;
        section.materialSlotIndex = object.materialSlotIndex;
        section.materialSlotName = object.materialSlotName;
        section.windProfileKey = object.speedTreeWindProfileKey;
        actor.sections.push_back(std::move(section));
    }
    // 模型级 id 取排序后的第一个 section：选择时整模型高亮走的是同一个 RuntimeId。
    actor.meshObjectId = actor.sections.empty() ? 0 : actor.sections.front().objectId;
    return actor;
}

EditorSceneActorSnapshot BuildLightActor(
    const World& world, EditorSceneActorType type, const std::string& name,
    const LightSnapshot& light, bool primary)
{
    EditorSceneActorSnapshot actor;
    actor.type = type;
    actor.worldGeneration = world.GetGeneration();
    actor.scenePath = world.GetScenePath();
    actor.identity = EditorUi::SceneActorIdentity(type, name);
    actor.displayName = name;
    actor.light = light;
    actor.primaryDirectionalLight = primary;
    return actor;
}

EditorSceneActorSnapshot BuildCameraActor(const World& world, const Camera& camera)
{
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::Camera;
    actor.worldGeneration = world.GetGeneration();
    actor.scenePath = world.GetScenePath();
    actor.displayName = camera.GetName().empty() ? "Camera" : camera.GetName();
    actor.identity = EditorUi::SceneActorIdentity(EditorSceneActorType::Camera, actor.displayName);
    actor.camera.position = camera.GetPosition();
    actor.camera.forward = camera.GetForwardVector();
    actor.camera.right = camera.GetRightVector();
    actor.camera.up = camera.GetUpVector();
    actor.camera.horizontalFovDegrees = camera.GetHFOV();
    actor.camera.clipNear = camera.GetClipNear();
    actor.camera.clipFar = camera.GetClipFar();
    return actor;
}

EditorSceneActorSnapshot BuildEnvironmentActor(const World& world)
{
    const WorldEnvironment& environment = world.GetEnvironment();
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::Environment;
    actor.worldGeneration = world.GetGeneration();
    actor.scenePath = world.GetScenePath();
    // 场景没有 environment 对象时 WorldEnvironment 保持默认值且没有名字，用固定标签回退。
    actor.displayName = environment.name.empty() ? "Environment" : environment.name;
    actor.identity = EditorUi::SceneActorIdentity(EditorSceneActorType::Environment, actor.displayName);
    actor.environment.type = environment.type;
    actor.environment.cubeSize = environment.cubeSize;
    actor.environment.intensity = environment.intensity;
    actor.environment.hdrPath = environment.hdrPath;
    return actor;
}

EditorSceneActorSnapshot BuildWindActor(const World& world, const SpeedTreeWindProfile& profile)
{
    const SpeedTreeWindConfig& config = profile.config;
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::WindProfile;
    actor.worldGeneration = world.GetGeneration();
    actor.scenePath = world.GetScenePath();
    actor.displayName = profile.key;
    actor.identity = EditorUi::SceneActorIdentity(EditorSceneActorType::WindProfile, profile.key);
    actor.wind.key = profile.key;
    actor.wind.sourceBoundsMin = profile.sourceBoundsMin;
    actor.wind.sourceBoundsMax = profile.sourceBoundsMax;
    actor.wind.strengthResponse = config.common.strengthResponse;
    actor.wind.directionResponse = config.common.directionResponse;
    actor.wind.gustFrequency = config.common.gustFrequency;
    actor.wind.gustStrengthMin = config.common.gustStrengthMin;
    actor.wind.gustStrengthMax = config.common.gustStrengthMax;
    actor.wind.gustDurationMin = config.common.gustDurationMin;
    actor.wind.gustDurationMax = config.common.gustDurationMax;
    actor.wind.gustRiseScalar = config.common.gustRiseScalar;
    actor.wind.gustFallScalar = config.common.gustFallScalar;
    actor.wind.sharedStartHeight = config.sharedStartHeight;
    actor.wind.branch1StretchLimit = config.branch1StretchLimit;
    actor.wind.branch2StretchLimit = config.branch2StretchLimit;
    actor.wind.doShared = config.doShared;
    actor.wind.doBranch1 = config.doBranch1;
    actor.wind.doBranch2 = config.doBranch2;
    actor.wind.doRipple = config.doRipple;
    actor.wind.doShimmer = config.doShimmer;
    return actor;
}

} // namespace

std::vector<EditorUi::EditorSceneActorSnapshot> BuildEditorSceneActors(const World& world)
{
    std::vector<EditorSceneActorSnapshot> actors;

    for (const MeshActorGroup& group : CollectMeshGroups(world))
        actors.push_back(BuildMeshActor(world, group));

    const std::shared_ptr<DirectionalLight>& primary = world.GetPrimaryDirectionalLight();
    for (const auto& entry : world.GetDirectionalLights())
    {
        actors.push_back(BuildLightActor(world, EditorSceneActorType::DirectionalLight, entry.first,
            BuildWorldDirectionalLightSnapshot(world, entry.first, *entry.second),
            primary != nullptr && entry.second == primary));
    }
    for (const auto& entry : world.GetPointLights())
    {
        actors.push_back(BuildLightActor(world, EditorSceneActorType::PointLight, entry.first,
            BuildWorldPointLightSnapshot(world, entry.first, *entry.second), false));
    }
    for (const auto& entry : world.GetSpotLights())
    {
        actors.push_back(BuildLightActor(world, EditorSceneActorType::SpotLight, entry.first,
            BuildWorldSpotLightSnapshot(world, entry.first, *entry.second), false));
    }
    for (const auto& entry : world.GetSquareAreaLights())
    {
        actors.push_back(BuildLightActor(world, EditorSceneActorType::SquareAreaLight, entry.first,
            BuildWorldSquareAreaLightSnapshot(world, entry.first, entry.second), false));
    }

    if (world.GetCamera() != nullptr)
        actors.push_back(BuildCameraActor(world, *world.GetCamera()));

    actors.push_back(BuildEnvironmentActor(world));

    for (const auto& entry : world.GetSpeedTreeWindProfiles())
        actors.push_back(BuildWindActor(world, entry.second));

    return actors;
}

} // namespace VL

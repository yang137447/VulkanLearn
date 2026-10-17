#include "editor/scene/editorSceneActorBuilder.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "editor/ui/editorSceneActorTree.h"
#include "sceneNode.h"
#include "world/meshSceneObjectIdentity.h"
#include "world/world.h"
#include "world/worldSnapshotBuilder.h"
#include "script/scriptScene.h"
#include "script/scriptComponents.h"

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
    return MeshSceneObjectIdentity(object, objectName);
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
    if (!group.sections.empty())
    {
        actor.position = group.sections.front().second->scenePosition;
        actor.rotation = group.sections.front().second->sceneRotation;
        actor.scale = group.sections.front().second->sceneScale;
    }
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
    // 多 section 网格在 World 里是多个对象；只要还有可见 section 就按可见显示，
    // 与渲染快照仍画出这些 section 的事实一致。写入端
    // （World::SetMeshSceneObjectVisible）按同一身份一次写全所有 section，
    // 因此这个"任一可见"的读数在点击之后与整行状态必然一致。
    actor.visible = std::any_of(group.sections.begin(), group.sections.end(),
        [](const auto& entry) { return entry.second->visible; });
    // 模型级 id 取排序后的第一个 section：选择时整模型高亮走的是同一个 RuntimeId。
    actor.meshObjectId = actor.sections.empty() ? 0 : actor.sections.front().objectId;
    return actor;
}

EditorSceneActorSnapshot BuildLightActor(
    const World& world, EditorSceneActorType type, const std::string& name,
    const LightSnapshot& light, bool primary, bool visible)
{
    EditorSceneActorSnapshot actor;
    actor.type = type;
    actor.worldGeneration = world.GetGeneration();
    actor.scenePath = world.GetScenePath();
    actor.identity = EditorUi::SceneActorIdentity(type, name);
    actor.displayName = name;
    actor.visible = visible;
    actor.light = light;
    actor.position = light.position;
    actor.primaryDirectionalLight = primary;
    actor.castShadows = world.GetCsmSettings().castShadows;
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
    actor.position = camera.GetPosition();
    actor.rotation = camera.GetRotation();
    actor.camera.forward = camera.GetForwardVector();
    actor.camera.right = camera.GetRightVector();
    actor.camera.up = camera.GetUpVector();
    actor.camera.horizontalFovDegrees = camera.GetHFOV();
    actor.camera.clipNear = camera.GetClipNear();
    actor.camera.clipFar = camera.GetClipFar();
    actor.camera.exposure = camera.GetExposure();
    if (world.GetRenderCamera())
    {
        const Camera& view = *world.GetRenderCamera();
        actor.viewportCamera.position = view.GetPosition();
        actor.viewportCamera.forward = view.GetForwardVector();
        actor.viewportCamera.right = view.GetRightVector();
        actor.viewportCamera.up = view.GetUpVector();
        actor.viewportCamera.horizontalFovDegrees = view.GetHFOV();
        actor.viewportCamera.clipNear = view.GetClipNear();
        actor.viewportCamera.clipFar = view.GetClipFar();
        actor.hasViewportCamera = true;
    }
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
        if (group.sceneObjectIdentity.rfind("__blueprint/", 0) != 0)
            actors.push_back(BuildMeshActor(world, group));

    auto declarations = world.GetScriptBlueprints();
    for (const auto& state : world.GetScriptBlueprintStates())
        if (state.contains("__spawnPath")) declarations.push_back(state);
    for (const auto& declaration : declarations)
    {
        EditorSceneActorSnapshot actor;
        actor.type = EditorSceneActorType::Script;
        actor.worldGeneration = world.GetGeneration();
        actor.scenePath = world.GetScenePath();
        const auto id = declaration.at("id").get<std::string>();
        actor.identity = "blueprintInstance|" + id;
        actor.displayName = declaration.at("name").get<std::string>();
        actor.scriptClass = declaration.at("class").get<std::string>();
        actor.scriptProperties = declaration.at("properties");
        actor.scriptInstanceEditable = declaration.value("__instanceEditable", nlohmann::json::array());
        actor.scriptComponentInstanceEditable = declaration.value("__componentInstanceEditable", nlohmann::json::object());
        actor.scriptComponents = declaration.value("components", nlohmann::json::array());
        actor.scriptLogicComponents = declaration.value("scriptComponents", nlohmann::json::array());
        actor.scriptPhysics = declaration.value("physics", nlohmann::json(nullptr));
        actor.scriptSpawned = declaration.contains("__spawnPath");
        if (actor.scriptSpawned) actor.scriptBlueprintPath = declaration.at("__spawnPath");
        for (const auto& source : world.GetScriptBlueprintSources())
            if (source.at("id") == id)
            {
                actor.scriptBlueprintPath = source.value("blueprintPath", std::string());
                actor.scriptOverrides = source.at("properties");
                actor.scriptComponentOverrides = source.value("componentOverrides", nlohmann::json::object());
                actor.scriptLogicOverrides = source.value("scriptComponentOverrides", nlohmann::json::object());
                if (actor.scriptBlueprintPath.empty())
                    for (const auto& node : source.value("scriptComponents", nlohmann::json::array()))
                        actor.scriptLogicOverrides[node.at("id").get<std::string>()] = node.at("properties");
                break;
            }
        for (const char* field : {"position", "rotation", "scale"})
        {
            const auto& value = declaration.at(field);
            Eigen::Vector3f vector(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
            if (std::string(field) == "position") actor.position = vector;
            else if (std::string(field) == "rotation") actor.rotation = vector;
            else actor.scale = vector;
        }
        for (const auto& state : world.GetScriptBlueprintStates())
            if (state.at("id") == id)
            {
                actor.visible = !state.value("destroyed", false);
                actor.scriptComponents = state.at("components");
                // 播放时显示当前逻辑组件状态；作者权限仍从声明按稳定 ID 保留。
                auto liveLogic = state.value("scriptComponents", nlohmann::json::array());
                for (auto& node : liveLogic)
                    for (const auto& authored : actor.scriptLogicComponents)
                        if (authored.at("id") == node.at("id") && authored.contains("instanceEditable"))
                            node["instanceEditable"] = authored.at("instanceEditable");
                actor.scriptLogicComponents = std::move(liveLogic);
                actor.scriptPhysics = state.value("physics", nlohmann::json(nullptr));
                for (const char* field : {"position", "rotation", "scale"})
                {
                    const auto& value = state.at(field);
                    Eigen::Vector3f vector(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
                    if (std::string(field) == "position") actor.position = vector;
                    else if (std::string(field) == "rotation") actor.rotation = vector;
                    else actor.scale = vector;
                }
                break;
            }
        // 只冻结预览所需 section；拾取必须使用正式导入后的 bounds，不能猜测源模型尺寸。
        if (id.rfind("~preview.", 0) == 0)
            for (const auto& component : actor.scriptComponents)
            {
                if (component.at("type") != "mesh") continue;
                const auto identity = ScriptMeshIdentity(id, component);
                for (const auto& entry : world.GetMeshObjects())
                {
                    const auto& mesh = entry.second;
                    if (!mesh.visible || mesh.sceneObjectIdentity != identity) continue;
                    EditorUi::EditorScriptComponentBounds bounds;
                    bounds.componentId = ScriptComponentId(component);
                    bounds.worldFromLocal = mesh.model;
                    bounds.minimum = mesh.localBoundsMin;
                    bounds.maximum = mesh.localBoundsMax;
                    actor.scriptComponentBounds.push_back(std::move(bounds));
                }
            }
        actors.push_back(std::move(actor));
    }

    const std::shared_ptr<DirectionalLight>& primary = world.GetPrimaryDirectionalLight();
    for (const auto& entry : world.GetDirectionalLights())
    {
        actors.push_back(BuildLightActor(world, EditorSceneActorType::DirectionalLight, entry.first,
            BuildWorldDirectionalLightSnapshot(world, entry.first, *entry.second),
            primary != nullptr && entry.second == primary, entry.second->IsVisible()));
        actors.back().rotation = entry.second->GetRotation();
    }
    for (const auto& entry : world.GetPointLights())
    {
        actors.push_back(BuildLightActor(world, EditorSceneActorType::PointLight, entry.first,
            BuildWorldPointLightSnapshot(world, entry.first, *entry.second), false,
            entry.second->IsVisible()));
        actors.back().rotation = entry.second->GetRotation();
    }
    for (const auto& entry : world.GetSpotLights())
    {
        actors.push_back(BuildLightActor(world, EditorSceneActorType::SpotLight, entry.first,
            BuildWorldSpotLightSnapshot(world, entry.first, *entry.second), false,
            entry.second->IsVisible()));
        actors.back().rotation = entry.second->GetRotation();
    }
    for (const auto& entry : world.GetSquareAreaLights())
    {
        actors.push_back(BuildLightActor(world, EditorSceneActorType::SquareAreaLight, entry.first,
            BuildWorldSquareAreaLightSnapshot(world, entry.first, entry.second), false,
            entry.second.visible));
    }

    if (world.GetCamera() != nullptr)
        actors.push_back(BuildCameraActor(world, *world.GetCamera()));

    actors.push_back(BuildEnvironmentActor(world));

    for (const auto& entry : world.GetSpeedTreeWindProfiles())
        actors.push_back(BuildWindActor(world, entry.second));

    return actors;
}

} // namespace VL

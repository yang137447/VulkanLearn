#include "world/world.h"

#include <algorithm>
#include <set>
#include <utility>

#include "sceneNode.h"
#include "world/meshSceneObjectIdentity.h"
#include "script/scriptScene.h"
#include "world/scriptTemplateExtension.h"
#include "animation/skeletalPlayback.h"

namespace VL
{
namespace
{
// 克隆/重建只共享不可变资产。组件池与 Spawn 的可变播放时钟不能相互借用。
void CloneAnimation(WorldMeshObject& mesh,
    std::unordered_map<std::string, std::shared_ptr<Animation::SkeletalPlayback>>& players)
{
    if (!mesh.animation) return;
    auto& player = players[mesh.sceneObjectIdentity];
    if (!player) player = mesh.animation->NewInstance();
    mesh.animation = player;
    mesh.skinPose = player->Pose(mesh.skinSection);
    mesh.localBoundsMin = mesh.skinPose->boundsMin;
    mesh.localBoundsMax = mesh.skinPose->boundsMax;
}
}

World::World(uint64_t generation, std::string scenePath)
    : generation(generation)
    , scenePath(std::move(scenePath))
{
}

std::unique_ptr<ScriptTemplateExtension> World::PrepareScriptTemplateExtension(
    const std::string& path, const std::string& digest, const nlohmann::json& declaration,
    const std::vector<WorldMeshObject>& meshes, const std::vector<SpeedTreeWindProfile>& profiles) const
{
    if (scriptTemplates.contains(path)) throw std::invalid_argument("Blueprint asset is already loaded");
    if (scriptTemplates.size() >= 128) throw std::invalid_argument("Too many Blueprint spawn templates");
    World staged(*this);
    staged.scriptTemplates[path] = declaration;
    staged.scriptAssetDigests[path] = digest;
    staged.scriptResolvedBlueprints.push_back(declaration);
    for (const auto& mesh : meshes)
        if (!staged.meshObjects.emplace(mesh.debugName, mesh).second)
            throw std::invalid_argument("Dynamic Blueprint mesh identity collision");
    for (const auto& profile : profiles) staged.speedTreeWindProfiles.emplace(profile.key, profile);
    staged.ApplyScriptBlueprintVisuals(declaration, false);
    staged.AppendScriptComponentMeshTemplate(declaration);
    auto extension = std::make_unique<ScriptTemplateExtension>();
    extension->meshes = std::move(staged.meshObjects);
    extension->windProfiles = std::move(staged.speedTreeWindProfiles);
    extension->templates = std::move(staged.scriptTemplates);
    extension->digests = std::move(staged.scriptAssetDigests);
    extension->resolved = std::move(staged.scriptResolvedBlueprints);
    extension->matrices = std::move(staged.scriptComponentMatrices);
    extension->componentTemplates = std::move(staged.scriptComponentMeshTemplates);
    extension->structures = std::move(staged.scriptComponentMeshStructures);
    return extension;
}

std::unique_ptr<ScriptTemplateExtension> World::PrepareScriptTemplateRemoval(const std::string& path) const
{
    if (!scriptTemplates.contains(path)) throw std::invalid_argument("Blueprint asset is not loaded");
    for (const auto& state : scriptBlueprintStates)
        if (state.value("__spawnPath", "") == path && !state.value("destroyed", false))
            throw std::invalid_argument("Blueprint asset still has live instances");
    World staged(*this);
    std::set<std::string> removedWindProfiles;
    for (const auto& mesh : staged.meshObjects)
        if (!mesh.second.speedTreeWindProfileKey.empty())
            removedWindProfiles.insert(mesh.second.speedTreeWindProfileKey);
    const std::string id = staged.scriptTemplates.at(path).at("id");
    staged.RemoveScriptBlueprintMeshes(id);
    for (const auto& state : scriptBlueprintStates)
        if (state.value("__spawnPath", "") == path)
        {
            staged.RemoveScriptBlueprintMeshes(state.at("id"));
            staged.scriptComponentMeshStructures.erase(state.at("id").get<std::string>());
        }
    staged.scriptTemplates.erase(path);
    staged.scriptAssetDigests.erase(path);
    auto& resolved = staged.scriptResolvedBlueprints;
    for (auto it = resolved.begin(); it != resolved.end();)
        if (it->at("id") == id) it = resolved.erase(it);
        else ++it;
    staged.scriptComponentMeshStructures.erase(id);
    std::set<std::string> usedComponentTemplates;
    for (const auto& blueprint : resolved)
        for (const auto& component : blueprint.value("components", nlohmann::json::array()))
            if (IsScriptMeshComponent(component.at("type").get<std::string>()))
                usedComponentTemplates.insert(ScriptComponentMeshTemplateKey(blueprint, component));
    for (auto it = staged.scriptComponentMeshTemplates.begin(); it != staged.scriptComponentMeshTemplates.end();)
        if (!usedComponentTemplates.count(it->first)) it = staged.scriptComponentMeshTemplates.erase(it);
        else ++it;
    for (const auto& mesh : staged.meshObjects) removedWindProfiles.erase(mesh.second.speedTreeWindProfileKey);
    for (const auto& key : removedWindProfiles) staged.speedTreeWindProfiles.erase(key);
    auto extension = std::make_unique<ScriptTemplateExtension>();
    extension->meshes = std::move(staged.meshObjects);
    extension->windProfiles = std::move(staged.speedTreeWindProfiles);
    extension->templates = std::move(staged.scriptTemplates);
    extension->digests = std::move(staged.scriptAssetDigests);
    extension->resolved = std::move(staged.scriptResolvedBlueprints);
    extension->matrices = std::move(staged.scriptComponentMatrices);
    extension->componentTemplates = std::move(staged.scriptComponentMeshTemplates);
    extension->structures = std::move(staged.scriptComponentMeshStructures);
    return extension;
}

void World::CommitScriptTemplateExtension(std::unique_ptr<ScriptTemplateExtension> extension) noexcept
{
    meshObjects.swap(extension->meshes);
    speedTreeWindProfiles.swap(extension->windProfiles);
    scriptTemplates.swap(extension->templates);
    scriptAssetDigests.swap(extension->digests);
    scriptResolvedBlueprints.swap(extension->resolved);
    scriptComponentMatrices.swap(extension->matrices);
    scriptComponentMeshTemplates.swap(extension->componentTemplates);
    scriptComponentMeshStructures.swap(extension->structures);
}

bool World::SetScriptBlueprintTransform(const std::string& id, const std::string& field,
    uint32_t component, float value)
{
    for (auto& actor : scriptBlueprints)
        if (actor.at("id") == id)
        {
            actor[field][component] = value;
            for (auto& source : scriptResolvedBlueprints)
                if (source.at("id") == id) source[field][component] = value;
            ApplyScriptBlueprintVisuals(actor);
            return true;
        }
    return false;
}

void World::ApplyScriptBlueprintVisuals(const nlohmann::json& actor, bool visible)
{
    const auto id = actor.at("id").get<std::string>();
    const auto prefix = ScriptBlueprintMeshIdentity(id);
    for (auto it = scriptLights.begin(); it != scriptLights.end();)
        if (it->first == prefix || it->first.rfind(prefix + "/", 0) == 0) it = scriptLights.erase(it);
        else ++it;
    const auto components = actor.value("components", nlohmann::json::array());
    // 空组件仍兼容测试/旧加载器提供的单根对象；正式逻辑 Actor 不会命中网格。
    if (components.empty())
    {
        SetMeshSceneObjectTransform(ScriptBlueprintMeshIdentity(id), ScriptVector(actor, "position"),
            ScriptVector(actor, "rotation"), ScriptVector(actor, "scale", 1));
        SetMeshSceneObjectVisible(ScriptBlueprintMeshIdentity(id), visible);
    }
    for (size_t index = 0; index < components.size(); ++index)
    {
        const auto type = components[index].at("type").get<std::string>();
        if (IsScriptLight(type))
        {
            const auto identity = ScriptMeshIdentity(id, components[index]);
            const auto& node = components[index];
            if (!visible || !actor.value("enabled", true) || actor.value("failed", false) || !node.value("visible", true))
            {
                scriptLights.erase(identity);
                continue;
            }
            LightSnapshot light;
            light.type = type == "directionalLight" ? LightSnapshotType::Directional :
                type == "spotLight" ? LightSnapshotType::Spot : LightSnapshotType::Point;
            light.color = ScriptVector(node, "color", 1);
            light.intensity = node.value("intensity", 1000.0f);
            light.position = ScriptComponentMatrix(actor, index).block<3, 1>(0, 3);
            const Eigen::Matrix3f rotation = ScriptComponentRotation(actor, index).toRotationMatrix();
            light.direction = rotation * -Eigen::Vector3f::UnitZ();
            light.worldToLight = rotation.transpose();
            light.coneAngleInner = node.value("coneAngleInner", 30.0f) * (3.14159265358979323846f / 180);
            light.coneAngleOuter = node.value("coneAngleOuter", 45.0f) * (3.14159265358979323846f / 180);
            scriptLights[identity] = std::move(light);
            continue;
        }
        if (!IsScriptMeshComponent(components[index].at("type").get<std::string>())) continue;
        const auto identity = ScriptMeshIdentity(id, components[index]);
        const Eigen::Matrix4f next = ScriptComponentMatrix(actor, index);
        auto previous = scriptComponentMatrices.find(identity);
        for (auto& entry : meshObjects)
        {
            auto& mesh = entry.second;
            if (MeshSceneObjectIdentity(mesh, entry.first) != identity) continue;
            SceneNode initial;
            initial.SetTransform(mesh.scenePosition, mesh.sceneRotation, mesh.sceneScale);
            const Eigen::Matrix4f old = previous == scriptComponentMatrices.end() ?
                initial.GetModelMatrix() : previous->second;
            mesh.model = (next * old.inverse() * mesh.model).eval();
            mesh.scenePosition = ScriptVector(actor, "position");
            mesh.sceneRotation = ScriptVector(actor, "rotation");
            mesh.sceneScale = ScriptVector(actor, "scale", 1);
            mesh.visible = visible;
        }
        scriptComponentMatrices[identity] = next;
    }
}

void World::PrepareScriptComponentMeshTemplates()
{
    scriptComponentMeshTemplates.clear();
    scriptComponentMeshStructures.clear();
    for (const auto& actor : scriptResolvedBlueprints) AppendScriptComponentMeshTemplate(actor);
}

void World::AppendScriptComponentMeshTemplate(const nlohmann::json& actor)
{
        const auto components = actor.value("components", nlohmann::json::array());
        nlohmann::json structure = nlohmann::json::array();
        for (const auto& node : components)
            if (IsScriptMeshComponent(node.at("type").get<std::string>()))
                structure.push_back(ScriptComponentMeshStructureEntry(actor, node));
        scriptComponentMeshStructures[actor.at("id").get<std::string>()] = structure.dump();
        for (size_t index = 0; index < components.size(); ++index)
        {
            const auto& node = components[index];
            if (!IsScriptMeshComponent(node.at("type").get<std::string>())) continue;
            const auto key = ScriptComponentMeshTemplateKey(actor, node);
            if (scriptComponentMeshTemplates.count(key)) continue;
            const auto identity = ScriptMeshIdentity(actor.at("id"), node);
            std::vector<WorldMeshObject> sections;
            for (const auto& entry : meshObjects)
                if (MeshSceneObjectIdentity(entry.second, entry.first) == identity)
                {
                    auto section = entry.second;
                    // 池内保存正式导入后的 section 局部矩阵与资源键；动态组件不能触发第二条导入链。
                    section.model = (ScriptComponentMatrix(actor, index).inverse() * section.model).eval();
                    sections.push_back(std::move(section));
                }
            scriptComponentMeshTemplates.emplace(key, std::move(sections));
        }
}

void World::SynchronizeScriptComponentMeshes(const nlohmann::json& actor)
{
    const auto id = actor.at("id").get<std::string>();
    nlohmann::json structure = nlohmann::json::array();
    const auto components = actor.value("components", nlohmann::json::array());
    for (const auto& node : components)
        if (IsScriptMeshComponent(node.at("type").get<std::string>()))
            structure.push_back(ScriptComponentMeshStructureEntry(actor, node));
    const auto signature = structure.dump();
    if (scriptComponentMeshStructures[id] == signature) return;
    std::vector<std::pair<std::string, WorldMeshObject>> candidates;
    std::unordered_map<std::string, std::shared_ptr<Animation::SkeletalPlayback>> players;
    // 添加兄弟组件不应重启现有动画。仅保留稳定 ID、模型和材质均未变化的播放器。
    const auto previousStructure = scriptComponentMeshStructures[id].empty() ?
        nlohmann::json::array() : nlohmann::json::parse(scriptComponentMeshStructures[id]);
    for (const auto& node : components)
    {
        if (!IsScriptMeshComponent(node.at("type").get<std::string>())) continue;
        nlohmann::json item = ScriptComponentMeshStructureEntry(actor, node);
        if (std::find(previousStructure.begin(), previousStructure.end(), item) == previousStructure.end()) continue;
        const auto identity = ScriptMeshIdentity(id, node);
        for (const auto& entry : meshObjects)
            if (entry.second.sceneObjectIdentity == identity && entry.second.animation)
            { players.emplace(identity, entry.second.animation); break; }
    }
    for (size_t index = 0; index < components.size(); ++index)
    {
        const auto& node = components[index];
        if (!IsScriptMeshComponent(node.at("type").get<std::string>())) continue;
        const auto key = ScriptComponentMeshTemplateKey(actor, node);
        const auto source = scriptComponentMeshTemplates.find(key);
        if (source == scriptComponentMeshTemplates.end())
            throw std::invalid_argument("Script.ComponentMeshNotPreloaded");
        size_t sectionIndex = 0;
        for (const auto& sourceSection : source->second)
        {
            auto section = sourceSection;
            section.sceneObjectIdentity = ScriptMeshIdentity(id, node);
            CloneAnimation(section, players);
            section.debugName = section.sceneObjectIdentity + (sectionIndex == 0 ? "" : "~section." + std::to_string(sectionIndex));
            ++sectionIndex;
            section.model = (ScriptComponentMatrix(actor, index) * sourceSection.model).eval();
            section.scenePosition = ScriptVector(actor, "position");
            section.sceneRotation = ScriptVector(actor, "rotation");
            section.sceneScale = ScriptVector(actor, "scale", 1);
            candidates.emplace_back(section.debugName, std::move(section));
        }
    }
    // 所有池命中后才移除旧对象；没有文件/Vulkan工作，RT 只看到下一份冻结快照。
    RemoveScriptBlueprintMeshes(id);
    for (auto& entry : candidates) meshObjects.emplace(std::move(entry));
    for (size_t index = 0; index < components.size(); ++index)
        if (IsScriptMeshComponent(components[index].at("type").get<std::string>()))
            scriptComponentMatrices[ScriptMeshIdentity(id, components[index])] = ScriptComponentMatrix(actor, index);
    scriptComponentMeshStructures[id] = signature;
}

void World::CloneScriptBlueprintMeshes(const std::string& templateId, const nlohmann::json& actor)
{
    const std::string prefix = ScriptBlueprintMeshIdentity(templateId);
    const std::string destination = ScriptBlueprintMeshIdentity(actor.at("id").get<std::string>());
    std::vector<std::pair<std::string, WorldMeshObject>> clones;
    std::unordered_map<std::string, std::shared_ptr<Animation::SkeletalPlayback>> players;
    for (const auto& entry : meshObjects)
    {
        const auto identity = MeshSceneObjectIdentity(entry.second, entry.first);
        if (identity != prefix && identity.rfind(prefix + "/", 0) != 0) continue;
        auto clone = entry.second;
        clone.sceneObjectIdentity = destination + identity.substr(prefix.size());
        CloneAnimation(clone, players);
        clone.debugName = destination + entry.first.substr(prefix.size());
        clones.emplace_back(clone.debugName, std::move(clone));
        const auto matrix = scriptComponentMatrices.find(identity);
        if (matrix != scriptComponentMatrices.end())
            scriptComponentMatrices[destination + identity.substr(prefix.size())] = matrix->second;
    }
    // 不在遍历 unordered_map 时插入；所有克隆只共享只读资源键，不创建 Vulkan 对象。
    for (auto& entry : clones) meshObjects.emplace(std::move(entry));
    ApplyScriptBlueprintVisuals(actor);
}

void World::RemoveScriptBlueprintMeshes(const std::string& id)
{
    const auto prefix = ScriptBlueprintMeshIdentity(id);
    for (auto it = meshObjects.begin(); it != meshObjects.end();)
        if (it->second.sceneObjectIdentity == prefix || it->second.sceneObjectIdentity.rfind(prefix + "/", 0) == 0)
            it = meshObjects.erase(it);
        else ++it;
    for (auto it = scriptComponentMatrices.begin(); it != scriptComponentMatrices.end();)
        if (it->first == prefix || it->first.rfind(prefix + "/", 0) == 0) it = scriptComponentMatrices.erase(it);
        else ++it;
    for (auto it = scriptLights.begin(); it != scriptLights.end();)
        if (it->first == prefix || it->first.rfind(prefix + "/", 0) == 0) it = scriptLights.erase(it);
        else ++it;
}

void World::SetViewTarget(std::shared_ptr<SceneNode> viewTarget)
{
    this->viewTarget = std::move(viewTarget);
}

void World::SetCamera(std::shared_ptr<Camera> camera)
{
    this->camera = std::move(camera);
    editorCamera.reset();
    editorViewEnabled = false;
}

void World::SetEditorViewEnabled(bool enabled)
{
    if (enabled && !editorCamera && camera)
    {
        editorCamera = std::make_shared<Camera>(*camera);
        editorCamera->SetName("Editor View");
    }
    editorViewEnabled = enabled;
    // 显示曝光仍来自场景 Camera；编辑器观察位姿和投影则独立保留。
    if (editorCamera && camera) editorCamera->SetExposure(camera->GetExposure());
}

void World::UpdateScriptCamera(const nlohmann::json& states)
{
    for (const auto& actor : states)
    {
        if (actor.value("destroyed", false) || actor.value("failed", false) || !actor.value("enabled", true)) continue;
        const auto& nodes = actor.at("components");
        for (size_t index = 0; index < nodes.size(); ++index)
        {
            const auto& node = nodes[index];
            if (node.at("type") != "camera" || !node.value("active", false)) continue;
            if (!scriptCamera) scriptCamera = camera ? std::make_shared<Camera>(*camera) : std::make_shared<Camera>();
            const Eigen::Matrix4f matrix = ScriptComponentMatrix(actor, index);
            scriptCamera->SetPosition(matrix.block<3, 1>(0, 3));
            scriptCamera->SetRotation(ScriptComponentRotation(actor, index));
            scriptCamera->SetName("C# Camera");
            // 宽高比沿用当前视口，投影/曝光仍经现有 World 快照冻结到 RT。
            const Eigen::Matrix4f projection = scriptCamera->GetProjectionMatrix();
            const float aspect = std::abs(projection(1, 1) / projection(0, 0));
            scriptCamera->SetProjection(node.value("fov", 60.0f), aspect,
                node.value("near_clip", 1.0f), node.value("far_clip", 10000.0f));
            if (camera) scriptCamera->SetExposure(camera->GetExposure());
            return;
        }
    }
    scriptCamera.reset();
}

void World::SetEnvironment(WorldEnvironment environment)
{
    this->environment = std::move(environment);
}

void World::SetCsmSettings(CsmSettings settings)
{
    csmSettings = std::move(settings);
}

void World::AddMeshObject(std::string name, WorldMeshObject object)
{
    meshObjects[std::move(name)] = std::move(object);
}

void World::AddSpeedTreeWindProfile(SpeedTreeWindProfile profile)
{
    speedTreeWindProfiles[profile.key] = std::move(profile);
}

void World::AddDirectionalLight(std::string name, std::shared_ptr<DirectionalLight> light)
{
    if (directionalLights.empty())
    {
        primaryDirectionalLightName = name;
    }
    directionalLights[std::move(name)] = std::move(light);
}

const std::shared_ptr<DirectionalLight>& World::GetPrimaryDirectionalLight() const
{
    static const std::shared_ptr<DirectionalLight> empty;
    const auto lightIt = directionalLights.find(primaryDirectionalLightName);
    return lightIt != directionalLights.end() ? lightIt->second : empty;
}

void World::AddPointLight(std::string name, std::shared_ptr<PointLight> light)
{
    pointLights[std::move(name)] = std::move(light);
}

void World::AddSpotLight(std::string name, std::shared_ptr<SpotLight> light)
{
    spotLights[std::move(name)] = std::move(light);
}

void World::AddSquareAreaLight(std::string name, WorldSquareAreaLight light)
{
    squareAreaLights[std::move(name)] = std::move(light);
}

template <typename ApplyFn>
bool World::ForEachMeshObjectInSceneObject(const std::string& sceneObjectIdentity, ApplyFn&& apply)
{
    bool matched = false;
    for (auto& entry : meshObjects)
    {
        if (MeshSceneObjectIdentity(entry.second, entry.first) != sceneObjectIdentity) continue;
        apply(entry.second);
        matched = true;
    }
    return matched;
}

bool World::SetMeshSceneObjectVisible(const std::string& sceneObjectIdentity, bool visible)
{
    // 一次写全该场景对象的所有 section：大纲的一行代表整个 actor，
    // 只改命中 identity 的那一个 section 会留下"行说隐藏、画面还有一半"的不一致。
    return ForEachMeshObjectInSceneObject(
        sceneObjectIdentity, [visible](WorldMeshObject& object) { object.visible = visible; });
}

bool World::IsMeshSceneObjectVisible(const std::string& sceneObjectIdentity) const
{
    // 与 actor 行一致：部分 section 可见时整行按可见显示。
    return std::any_of(meshObjects.begin(), meshObjects.end(),
        [&sceneObjectIdentity](const auto& entry)
        {
            return MeshSceneObjectIdentity(entry.second, entry.first) == sceneObjectIdentity &&
                entry.second.visible;
        });
}

bool World::SetLightVisible(const std::string& name, bool visible)
{
    // 三类灯光各有独立表；同名对象只会出现在其中一张表里，因此顺序查找即可。
    const auto directionalIt = directionalLights.find(name);
    if (directionalIt != directionalLights.end())
    {
        directionalIt->second->SetVisible(visible);
        return true;
    }
    const auto pointIt = pointLights.find(name);
    if (pointIt != pointLights.end())
    {
        pointIt->second->SetVisible(visible);
        return true;
    }
    const auto spotIt = spotLights.find(name);
    if (spotIt != spotLights.end())
    {
        spotIt->second->SetVisible(visible);
        return true;
    }
    return false;
}

bool World::SetSquareAreaLightVisible(const std::string& name, bool visible)
{
    const auto lightIt = squareAreaLights.find(name);
    if (lightIt == squareAreaLights.end()) return false;
    lightIt->second.visible = visible;
    return true;
}

bool World::SetSquareAreaLight(const std::string& name, const WorldSquareAreaLight& light)
{
    const auto it = squareAreaLights.find(name);
    if (it == squareAreaLights.end()) return false;
    it->second = light;
    return true;
}

bool World::GetMeshSceneObjectTransform(const std::string& identity,
    Eigen::Vector3f& position, Eigen::Vector3f& rotation,
    Eigen::Vector3f& scale) const
{
    // scene TRS 是 actor 级的：同一 identity 的每个 section 都保存同一份根 TRS，
    // 所以这里读第一个命中项就够，不需要像写入那样遍历所有 section。
    for (const auto& entry : meshObjects)
    {
        const WorldMeshObject& object = entry.second;
        if (MeshSceneObjectIdentity(object, entry.first) != identity) continue;
        position = object.scenePosition;
        rotation = object.sceneRotation;
        scale = object.sceneScale;
        return true;
    }
    return false;
}

bool World::SetMeshSceneObjectTransform(const std::string& identity,
    const Eigen::Vector3f& position, const Eigen::Vector3f& rotation,
    const Eigen::Vector3f& scale)
{
    SceneNode nextRoot;
    nextRoot.SetTransform(position, rotation, scale);
    bool matched = false;
    for (auto& entry : meshObjects)
    {
        WorldMeshObject& object = entry.second;
        if (MeshSceneObjectIdentity(object, entry.first) != identity) continue;
        SceneNode previousRoot;
        previousRoot.SetTransform(object.scenePosition, object.sceneRotation, object.sceneScale);
        // 不能用新根直接替换 model：section 的导入节点变换仍然属于该实例。
        object.model = (nextRoot.GetModelMatrix() *
            previousRoot.GetModelMatrix().inverse() * object.model).eval();
        object.scenePosition = position;
        object.sceneRotation = rotation;
        object.sceneScale = scale;
        matched = true;
    }
    return matched;
}

} // namespace VL

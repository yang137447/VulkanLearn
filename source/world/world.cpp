#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include "commonFunction.h"
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

// 负缩放是合法的镜像变换；只拒绝会让粒子 model 退化的零轴。
bool ValidParticleScaleAxis(float value)
{
    constexpr float kMinAbsoluteScale = 1.0e-6f;
    return std::isfinite(value) && std::abs(value) >= kMinAbsoluteScale;
}

constexpr size_t kMaxParticleUserParameters = 64;
constexpr size_t kMaxParticleParameterKeyLength = 64;
constexpr float kMaxParticleParameterAbsoluteValue = 1.0e6f;
constexpr size_t kMaxParticleRuntimeEvents = 256;
constexpr uint32_t kMaxParticleTotalBudget = 1000000u;

bool ValidParticleBudgetScale(float value)
{
    return std::isfinite(value) && value >= 0.0f && value <= 16.0f;
}

uint32_t ScaledParticleCapacity(uint32_t capacity, float scale)
{
    const double scaled = static_cast<double>(capacity) *
        static_cast<double>(scale);
    if (scaled <= 0.0) return 0;
    if (scaled >= static_cast<double>(
            std::numeric_limits<uint32_t>::max()))
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(scaled);
}

uint32_t EffectiveParticleMaxTotal(
    const WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget)
{
    return system.budget.maxTotalParticles != 0
        ? system.budget.maxTotalParticles
        : worldBudget.maxTotalParticles;
}

float EffectiveParticleBudgetScale(
    const WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget)
{
    return std::clamp(
        system.budget.emitterBudgetScale *
            worldBudget.emitterBudgetScale,
        0.0f,
        16.0f);
}

// 槽位身份与运行时资产是两份视图：槽位保留 tombstone，asset.emitters 只放
// active 项。渲染器仍按 emitter.name 建池，因此 destroy 能退休资源，而
// stableIndex 不会因删除中间项而漂移。enabledEmitterMask 始终按紧凑位置编码。
void RefreshParticleRuntime(
    WorldParticleSystem& system,
    const ParticleSystemBudget& worldBudget)
{
    system.asset.emitters.clear();
    uint32_t mask = 0;
    for (size_t stableIndex = 0;
         stableIndex < system.emitterSlots.size();
         ++stableIndex)
    {
        const auto& slot = system.emitterSlots[stableIndex];
        if (!slot.active) continue;
        const uint32_t position =
            static_cast<uint32_t>(system.asset.emitters.size());
        system.asset.emitters.push_back(slot.asset);
        if (slot.enabled && position < kMaxParticleEmitters)
            mask |= (1u << position);
    }
    if (!system.emitterSlots.empty())
        system.runtime.enabledEmitterMask = mask;
    system.nextEmitterSlot =
        static_cast<uint32_t>(system.emitterSlots.size());

    const ParticleSystemTelemetry previous = system.telemetry;
    system.telemetry = ParticleSystemTelemetry{};
    system.telemetry.available = previous.available;
    system.telemetry.aliveParticles = previous.aliveParticles;
    system.telemetry.lastFrameBudget = previous.lastFrameBudget;

    const uint32_t maxTotal = EffectiveParticleMaxTotal(system, worldBudget);
    const float scale = EffectiveParticleBudgetScale(system, worldBudget);
    uint32_t consumed = 0;
    for (size_t stableIndex = 0;
         stableIndex < system.emitterSlots.size();
         ++stableIndex)
    {
        const auto& slot = system.emitterSlots[stableIndex];
        ParticleEmitterTelemetry emitter;
        emitter.stableIndex = static_cast<uint32_t>(stableIndex);
        emitter.name = slot.asset.name;
        emitter.active = slot.active;
        emitter.enabled = slot.active && slot.enabled;
        emitter.restartEpoch = slot.restartEpoch;
        emitter.maxParticles = slot.active ? slot.asset.maxParticles : 0;
        if (stableIndex < previous.emitters.size() &&
            previous.emitters[stableIndex].stableIndex == stableIndex)
        {
            emitter.aliveParticles =
                previous.emitters[stableIndex].aliveParticles;
            emitter.aliveAvailable =
                previous.emitters[stableIndex].aliveAvailable;
        }
        system.telemetry.emitters.push_back(std::move(emitter));
        if (!slot.active) continue;
        ++system.telemetry.activeEmitterCount;
        if (slot.enabled) ++system.telemetry.enabledEmitterCount;
        // beam/light 不消费 GPU 粒子池：beam 直接绘制，light 由 CPU 运行时
        // 生成场景点光；两者都不计入 authored/budgetedCapacity，也不消耗
        // 其它 emitter 的总预算。
        if (slot.asset.renderMode == "beam" ||
            slot.asset.renderMode == "light")
        {
            system.telemetry.emitters.back().budgetedMaxParticles =
                slot.asset.renderMode == "beam"
                    ? slot.asset.maxParticles
                    : 0u;
            continue;
        }
        system.telemetry.authoredCapacity += slot.asset.maxParticles;
        const uint32_t scaled =
            ScaledParticleCapacity(slot.asset.maxParticles, scale);
        const uint32_t remaining = maxTotal == 0
            ? std::numeric_limits<uint32_t>::max()
            : (consumed >= maxTotal ? 0 : maxTotal - consumed);
        const uint32_t budgeted = std::min(scaled, remaining);
        consumed += budgeted;
        system.telemetry.emitters.back().budgetedMaxParticles = budgeted;
        system.telemetry.budgetedCapacity += budgeted;
    }
    system.telemetry.emitterCount =
        static_cast<uint32_t>(system.emitterSlots.size());
    system.telemetry.visibleEmitterCount = system.visible
        ? system.telemetry.enabledEmitterCount
        : 0u;
}

void InitializeParticleEmitterSlots(WorldParticleSystem& system)
{
    system.emitterSlots.clear();
    for (const auto& emitter : system.asset.emitters)
    {
        WorldParticleSystem::EmitterSlot slot;
        slot.asset = emitter;
        system.emitterSlots.push_back(std::move(slot));
    }
    system.nextEmitterSlot =
        static_cast<uint32_t>(system.emitterSlots.size());
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
            if (component.at("type") == "mesh")
                usedComponentTemplates.insert(component.at("modelPath").get<std::string>() + "\n" +
                    component.value("materialOverride", ""));
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
        if (components[index].at("type") != "mesh") continue;
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
            if (node.at("type") == "mesh")
                structure.push_back({ScriptComponentId(node), node.at("modelPath"), node.value("materialOverride", "")});
        scriptComponentMeshStructures[actor.at("id").get<std::string>()] = structure.dump();
        for (size_t index = 0; index < components.size(); ++index)
        {
            const auto& node = components[index];
            if (node.at("type") != "mesh") continue;
            const auto key = node.at("modelPath").get<std::string>() + "\n" + node.value("materialOverride", "");
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
        if (node.at("type") == "mesh")
            structure.push_back({ScriptComponentId(node), node.at("modelPath"), node.value("materialOverride", "")});
    const auto signature = structure.dump();
    if (scriptComponentMeshStructures[id] == signature) return;
    std::vector<std::pair<std::string, WorldMeshObject>> candidates;
    std::unordered_map<std::string, std::shared_ptr<Animation::SkeletalPlayback>> players;
    // 添加兄弟组件不应重启现有动画。仅保留稳定 ID、模型和材质均未变化的播放器。
    const auto previousStructure = scriptComponentMeshStructures[id].empty() ?
        nlohmann::json::array() : nlohmann::json::parse(scriptComponentMeshStructures[id]);
    for (const auto& node : components)
    {
        if (node.at("type") != "mesh") continue;
        const nlohmann::json item = {ScriptComponentId(node), node.at("modelPath"), node.value("materialOverride", "")};
        if (std::find(previousStructure.begin(), previousStructure.end(), item) == previousStructure.end()) continue;
        const auto identity = ScriptMeshIdentity(id, node);
        for (const auto& entry : meshObjects)
            if (entry.second.sceneObjectIdentity == identity && entry.second.animation)
            { players.emplace(identity, entry.second.animation); break; }
    }
    for (size_t index = 0; index < components.size(); ++index)
    {
        const auto& node = components[index];
        if (node.at("type") != "mesh") continue;
        const auto key = node.at("modelPath").get<std::string>() + "\n" + node.value("materialOverride", "");
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
        if (components[index].at("type") == "mesh")
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

void World::AddDecal(std::string name, WorldDecal decal)
{
    decals[std::move(name)] = std::move(decal);
}

void World::AddParticleSystem(
    std::string name,
    WorldParticleSystem system)
{
    InitializeParticleEmitterSlots(system);
    RefreshParticleRuntime(system, particleBudget);
    particleSystems[std::move(name)] = std::move(system);
}

bool World::RemoveParticleSystem(const std::string& name)
{
    return particleSystems.erase(name) > 0;
}

const WorldParticleSystem* World::FindParticleSystem(
    const std::string& name) const
{
    const auto systemIt = particleSystems.find(name);
    return systemIt != particleSystems.end() ? &systemIt->second : nullptr;
}

size_t World::UpdateParticleSystemAsset(
    const std::string& assetPath,
    ParticleSystemAsset asset)
{
    size_t replaced = 0;
    for (auto& [name, particleSystem] : particleSystems)
    {
        if (particleSystem.assetPath != assetPath) continue;
        particleSystem.asset = asset;
        InitializeParticleEmitterSlots(particleSystem);
        // 资产替换会重建发射器槽位，但实例级预算/附着/用户参数仍属于
        // 当前 World 实例，不应被编辑器预览资产刷新静默清掉。
        RefreshParticleRuntime(particleSystem, particleBudget);
        ++replaced;
    }
    return replaced;
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

bool World::SetParticleSystemVisible(const std::string& name, bool visible)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.visible = visible;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::SetParticleSystemTransform(const std::string& name,
    const Eigen::Vector3f& position, const Eigen::Vector3f& rotation,
    const Eigen::Vector3f& scale)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;

    if (!position.allFinite() || !rotation.allFinite() ||
        !ValidParticleScaleAxis(scale.x()) ||
        !ValidParticleScaleAxis(scale.y()) ||
        !ValidParticleScaleAxis(scale.z()))
    {
        return false;
    }

    Eigen::Matrix4f scaleMatrix = Eigen::Matrix4f::Identity();
    scaleMatrix(0, 0) = scale.x();
    scaleMatrix(1, 1) = scale.y();
    scaleMatrix(2, 2) = scale.z();
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = position;
    // 与 WorldBuilder::BuildParticleSystemModel 保持同一 bake 顺序。
    systemIt->second.model = translation *
        CommonFunction::RotationToMatrix(rotation) *
        scaleMatrix;
    systemIt->second.scenePosition = position;
    systemIt->second.sceneRotation = rotation;
    systemIt->second.sceneScale = scale;
    // 显式移动 actor 视为脱离附着，避免下一帧又被目标矩阵拉回去。
    systemIt->second.attachment = ParticleSystemAttachment{};
    return true;
}

bool World::SetParticleSystemSpawnRateScale(const std::string& name, float scale)
{
    std::string error;
    if (!ValidateParticleSpawnRateScale(scale, error))
    {
        return false;
    }
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.runtime.spawnRateScale = scale;
    return true;
}

bool World::SetParticleSystemColorTint(const std::string& name,
    const Eigen::Vector4f& colorTint)
{
    std::string error;
    if (!ValidateParticleColorTint(colorTint, error))
    {
        return false;
    }
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.runtime.colorTint = colorTint;
    return true;
}

bool World::SetParticleSystemEmitterEnabled(const std::string& name,
    uint32_t emitterIndex, bool enabled)
{
    std::string error;
    if (!ValidateParticleEmitterIndex(emitterIndex, error))
    {
        return false;
    }
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;

    if (!systemIt->second.emitterSlots.empty())
    {
        if (emitterIndex >= systemIt->second.emitterSlots.size())
            return false;
        auto& slot = systemIt->second.emitterSlots[emitterIndex];
        if (!slot.active) return false;
        slot.enabled = enabled;
        RefreshParticleRuntime(systemIt->second, particleBudget);
        return true;
    }

    // 只改对应低位；高位保留隐式/未来发射器的默认启用状态。
    const uint32_t emitterBit = 1u << emitterIndex;
    if (enabled)
    {
        systemIt->second.runtime.enabledEmitterMask |= emitterBit;
    }
    else
    {
        systemIt->second.runtime.enabledEmitterMask &= ~emitterBit;
    }
    return true;
}

bool World::ResetParticleSystemRuntimeOverrides(const std::string& name)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    // 默认构造即恢复全发射器启用、倍率 1、白色 tint；visible 不属于运行时覆盖。
    systemIt->second.runtime = ParticleSystemRuntimeOverrides{};
    for (auto& slot : systemIt->second.emitterSlots)
        if (slot.active) slot.enabled = true;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::SpawnParticleEmitter(const std::string& systemName,
    uint32_t sourceStableIndex, const std::string& requestedName,
    uint32_t& outStableIndex)
{
    const auto systemIt = particleSystems.find(systemName);
    if (systemIt == particleSystems.end()) return false;
    WorldParticleSystem& system = systemIt->second;
    if (sourceStableIndex >= system.emitterSlots.size() ||
        !system.emitterSlots[sourceStableIndex].active)
        return false;
    if (system.emitterSlots.size() >= kMaxParticleEmitters) return false;

    const uint32_t stableIndex =
        static_cast<uint32_t>(system.emitterSlots.size());
    std::string name = requestedName.empty()
        ? "runtime." + std::to_string(stableIndex)
        : requestedName;
    if (name.empty() || name.size() > 128 ||
        name.find('\0') != std::string::npos)
        return false;
    for (const auto& slot : system.emitterSlots)
        if (slot.asset.name == name) return false;

    WorldParticleSystem::EmitterSlot slot;
    slot.asset = system.emitterSlots[sourceStableIndex].asset;
    slot.asset.name = name;
    slot.active = true;
    slot.enabled = true;
    system.emitterSlots.push_back(std::move(slot));
    RefreshParticleRuntime(system, particleBudget);
    outStableIndex = stableIndex;
    return true;
}

bool World::DestroyParticleEmitter(const std::string& systemName,
    uint32_t stableIndex)
{
    const auto systemIt = particleSystems.find(systemName);
    if (systemIt == particleSystems.end()) return false;
    auto& slots = systemIt->second.emitterSlots;
    if (stableIndex >= slots.size() || !slots[stableIndex].active)
        return false;
    slots[stableIndex].active = false;
    slots[stableIndex].enabled = false;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::RestartParticleEmitter(const std::string& systemName,
    uint32_t stableIndex)
{
    const auto systemIt = particleSystems.find(systemName);
    if (systemIt == particleSystems.end()) return false;
    auto& slots = systemIt->second.emitterSlots;
    if (stableIndex >= slots.size() || !slots[stableIndex].active)
        return false;
    if (slots[stableIndex].restartEpoch !=
        std::numeric_limits<uint32_t>::max())
        ++slots[stableIndex].restartEpoch;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::FindParticleEmitterIndex(const std::string& systemName,
    const std::string& emitterName, uint32_t& outStableIndex) const
{
    const auto systemIt = particleSystems.find(systemName);
    if (systemIt == particleSystems.end()) return false;
    for (size_t index = 0;
         index < systemIt->second.emitterSlots.size();
         ++index)
    {
        if (systemIt->second.emitterSlots[index].asset.name != emitterName)
            continue;
        outStableIndex = static_cast<uint32_t>(index);
        return true;
    }
    return false;
}

bool World::SetParticleSystemBudget(const std::string& name,
    uint32_t maxTotalParticles, float emitterBudgetScale)
{
    if (!ValidParticleBudgetScale(emitterBudgetScale) ||
        maxTotalParticles > kMaxParticleTotalBudget)
        return false;
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.budget.maxTotalParticles = maxTotalParticles;
    systemIt->second.budget.emitterBudgetScale = emitterBudgetScale;
    RefreshParticleRuntime(systemIt->second, particleBudget);
    return true;
}

bool World::SetWorldParticleBudget(uint32_t maxTotalParticles,
    float emitterBudgetScale)
{
    if (!ValidParticleBudgetScale(emitterBudgetScale) ||
        maxTotalParticles > kMaxParticleTotalBudget)
        return false;
    particleBudget.maxTotalParticles = maxTotalParticles;
    particleBudget.emitterBudgetScale = emitterBudgetScale;
    particleBudget.effectiveCapacity = 0;
    for (auto& [name, system] : particleSystems)
    {
        (void)name;
        RefreshParticleRuntime(system, particleBudget);
    }
    return true;
}

ParticleSystemBudget World::GetEffectiveParticleBudget(
    const std::string& name) const
{
    const auto target = particleSystems.find(name);
    if (target == particleSystems.end()) return ParticleSystemBudget{};

    const uint32_t worldMax = particleBudget.maxTotalParticles;
    uint32_t cap = target->second.budget.maxTotalParticles;
    if (cap == 0 && worldMax != 0)
    {
        std::vector<std::string> names;
        uint32_t explicitTotal = 0;
        for (const auto& entry : particleSystems)
        {
            if (entry.second.budget.maxTotalParticles != 0)
                explicitTotal += entry.second.budget.maxTotalParticles;
            else
                names.push_back(entry.first);
        }
        std::sort(names.begin(), names.end());
        const uint32_t remaining = explicitTotal >= worldMax
            ? 0
            : worldMax - explicitTotal;
        if (!names.empty())
        {
            const uint32_t base = remaining /
                static_cast<uint32_t>(names.size());
            const uint32_t extra = remaining %
                static_cast<uint32_t>(names.size());
            for (size_t index = 0; index < names.size(); ++index)
                if (names[index] == name)
                {
                    cap = base + (index < extra ? 1u : 0u);
                    break;
                }
        }
    }

    ParticleSystemBudget result;
    result.maxTotalParticles = cap;
    result.emitterBudgetScale = std::clamp(
        target->second.budget.emitterBudgetScale *
            particleBudget.emitterBudgetScale,
        0.0f,
        16.0f);
    uint32_t consumed = 0;
    for (const auto& slot : target->second.emitterSlots)
    {
        if (!slot.active) continue;
        // beam 直接绘制、light 由 CPU 运行时生成点光，都不分配模拟池，
        // 因此不参与有效容量/预算消耗。
        if (slot.asset.renderMode == "beam" ||
            slot.asset.renderMode == "light") continue;
        const uint32_t scaled =
            ScaledParticleCapacity(slot.asset.maxParticles,
                result.emitterBudgetScale);
        const uint32_t remaining = cap == 0
            ? std::numeric_limits<uint32_t>::max()
            : (consumed >= cap ? 0 : cap - consumed);
        const uint32_t budgeted = std::min(scaled, remaining);
        consumed += budgeted;
        result.effectiveCapacity += budgeted;
    }
    return result;
}

bool World::SetParticleSystemUserParameter(const std::string& name,
    const std::string& key, float value)
{
    if (key.empty() || key.size() > kMaxParticleParameterKeyLength ||
        key.find('\0') != std::string::npos ||
        !std::isfinite(value) ||
        std::abs(value) > kMaxParticleParameterAbsoluteValue)
        return false;
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    auto& parameters = systemIt->second.userParameters;
    if (!parameters.count(key) &&
        parameters.size() >= kMaxParticleUserParameters)
        return false;
    parameters[key] = value;
    return true;
}

bool World::ClearParticleSystemUserParameter(const std::string& name,
    const std::string& key)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.userParameters.erase(key);
    return true;
}

bool World::SetParticleDataChannel(const std::string& name, float value)
{
    std::string error;
    if (!ValidateParticleDataChannelName(name, error) ||
        !ValidateParticleDataChannelValue(value, error))
        return false;
    const auto existing = particleDataChannels.find(name);
    if (existing == particleDataChannels.end() &&
        particleDataChannels.size() >= kMaxParticleDataChannels)
        return false;
    particleDataChannels[name] = value;
    return true;
}

bool World::ClearParticleDataChannel(const std::string& name)
{
    std::string error;
    if (!ValidateParticleDataChannelName(name, error)) return false;
    return particleDataChannels.erase(name) != 0;
}

bool World::GetParticleDataChannel(const std::string& name,
    float& outValue) const
{
    std::string error;
    if (!ValidateParticleDataChannelName(name, error)) return false;
    const auto found = particleDataChannels.find(name);
    if (found == particleDataChannels.end()) return false;
    outValue = found->second;
    return true;
}

bool ExecuteParticleDataChannelCommand(World& world, bool clear,
    const std::string& name, float value, std::string& outError)
{
    if (!ValidateParticleDataChannelName(name, outError)) return false;
    if (clear)
    {
        if (!world.ClearParticleDataChannel(name))
        {
            outError =
                "Particle data channel clear found no entry named: " + name;
            return false;
        }
        outError.clear();
        return true;
    }
    if (!ValidateParticleDataChannelValue(value, outError)) return false;
    if (!world.SetParticleDataChannel(name, value))
    {
        outError =
            "Particle data channel set was rejected (64-channel limit): " +
            name;
        return false;
    }
    outError.clear();
    return true;
}

bool World::AttachParticleSystem(const std::string& name,
    const std::string& targetKind, const std::string& targetName,
    const std::string& componentName,
    const Eigen::Vector3f& localPosition,
    const Eigen::Vector3f& localRotation,
    const Eigen::Vector3f& localScale)
{
    if (targetKind != "mesh" && targetKind != "component") return false;
    if (targetName.empty() || targetName.size() > 128 ||
        targetName.find('\0') != std::string::npos ||
        componentName.size() > 64 ||
        componentName.find('\0') != std::string::npos)
        return false;
    if (!localPosition.allFinite() || !localRotation.allFinite() ||
        !ValidParticleScaleAxis(localScale.x()) ||
        !ValidParticleScaleAxis(localScale.y()) ||
        !ValidParticleScaleAxis(localScale.z()))
        return false;
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    ParticleSystemAttachment attachment;
    attachment.targetKind = targetKind;
    attachment.targetName = targetName;
    attachment.componentName = componentName;
    attachment.localPosition = localPosition;
    attachment.localRotation = localRotation;
    attachment.localScale = localScale;
    attachment.resolved = false;
    systemIt->second.attachment = std::move(attachment);
    return true;
}

bool World::DetachParticleSystem(const std::string& name)
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    systemIt->second.attachment = ParticleSystemAttachment{};
    return true;
}

bool World::SetParticleSystemRendererTelemetry(const std::string& name,
    uint32_t aliveParticles,
    const std::vector<uint32_t>& emitterAliveParticles,
    uint32_t lastFrameBudget)
{
    if (emitterAliveParticles.size() > kMaxParticleEmitters) return false;
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return false;
    ParticleSystemTelemetry& telemetry = systemIt->second.telemetry;
    telemetry.available = true;
    telemetry.aliveParticles = aliveParticles;
    telemetry.lastFrameBudget = lastFrameBudget;
    for (auto& emitter : telemetry.emitters)
    {
        emitter.aliveParticles = 0;
        emitter.aliveAvailable = false;
    }
    for (size_t index = 0;
         index < emitterAliveParticles.size() &&
         index < telemetry.emitters.size();
         ++index)
    {
        telemetry.emitters[index].aliveParticles =
            emitterAliveParticles[index];
        telemetry.emitters[index].aliveAvailable = true;
    }
    return true;
}

const ParticleSystemTelemetry* World::GetParticleSystemTelemetry(
    const std::string& name) const
{
    const auto systemIt = particleSystems.find(name);
    return systemIt == particleSystems.end()
        ? nullptr
        : &systemIt->second.telemetry;
}

nlohmann::json World::DescribeParticleSystemTelemetry(
    const std::string& name) const
{
    const auto systemIt = particleSystems.find(name);
    if (systemIt == particleSystems.end()) return nlohmann::json();
    const WorldParticleSystem& system = systemIt->second;
    const ParticleSystemBudget budget = GetEffectiveParticleBudget(name);
    bool attachmentResolved = false;
    (void)ResolveParticleSystemModel(system, attachmentResolved);
    nlohmann::json result{
        {"name", name},
        {"emitterCount", system.telemetry.emitterCount},
        {"activeEmitterCount", system.telemetry.activeEmitterCount},
        {"enabledEmitterCount", system.telemetry.enabledEmitterCount},
        {"visibleEmitterCount", system.telemetry.visibleEmitterCount},
        {"authoredCapacity", system.telemetry.authoredCapacity},
        {"budgetedCapacity", system.telemetry.budgetedCapacity},
        {"aliveAvailable", system.telemetry.available},
        {"aliveParticles", system.telemetry.aliveParticles},
        {"lastFrameBudget", system.telemetry.lastFrameBudget},
        {"budget", {
            {"maxTotalParticles", budget.maxTotalParticles},
            {"emitterBudgetScale", budget.emitterBudgetScale},
            {"effectiveCapacity", budget.effectiveCapacity}}},
        {"attachment", {
            {"kind", system.attachment.targetKind},
            {"target", system.attachment.targetName},
            {"component", system.attachment.componentName},
            {"resolved", attachmentResolved}}}};
    result["emitters"] = nlohmann::json::array();
    for (const auto& emitter : system.telemetry.emitters)
        result["emitters"].push_back({
            {"index", emitter.stableIndex},
            {"name", emitter.name},
            {"active", emitter.active},
            {"enabled", emitter.enabled},
            {"restartEpoch", emitter.restartEpoch},
            {"maxParticles", emitter.maxParticles},
            {"budgetedMaxParticles", emitter.budgetedMaxParticles},
            {"aliveAvailable", emitter.aliveAvailable},
            {"aliveParticles", emitter.aliveParticles}});
    return result;
}

bool World::QueueParticleRuntimeEvent(const WorldParticleRuntimeEvent& event)
{
    if (event.systemName.empty() || event.systemName.size() > 128 ||
        event.systemName.find('\0') != std::string::npos ||
        event.emitterName.size() > 128 ||
        event.emitterName.find('\0') != std::string::npos ||
        (event.kind != "spawn" && event.kind != "death" &&
            event.kind != "collision") ||
        !std::isfinite(event.time) || !event.position.allFinite() ||
        !event.normal.allFinite())
        return false;
    if (particleSystems.find(event.systemName) == particleSystems.end())
        return false;
    if (particleRuntimeEvents.size() >= kMaxParticleRuntimeEvents)
        return false;
    particleRuntimeEvents.push_back(event);
    return true;
}

std::vector<WorldParticleRuntimeEvent> World::TakeParticleRuntimeEvents()
{
    auto result = std::move(particleRuntimeEvents);
    particleRuntimeEvents.clear();
    return result;
}

bool World::SetParticleEventCaptureInterest(
    const std::string& owner, bool enabled)
{
    if (owner.empty() || owner.size() > 128 ||
        owner.find('\0') != std::string::npos)
        return false;
    if (enabled)
        particleEventCaptureOwners.insert(owner);
    else
        particleEventCaptureOwners.erase(owner);
    return true;
}

Eigen::Matrix4f World::ResolveParticleSystemModel(
    const WorldParticleSystem& system, bool& resolved) const
{
    resolved = true;
    if (!system.attachment.IsEnabled()) return system.model;

    SceneNode local;
    local.SetTransform(
        system.attachment.localPosition,
        system.attachment.localRotation,
        system.attachment.localScale);
    Eigen::Matrix4f target = Eigen::Matrix4f::Identity();
    bool found = false;
    if (system.attachment.targetKind == "mesh")
    {
        std::vector<std::string> matchingKeys;
        for (const auto& [key, object] : meshObjects)
            if (MeshSceneObjectIdentity(object, key) ==
                    system.attachment.targetName ||
                key == system.attachment.targetName)
                matchingKeys.push_back(key);
        if (!matchingKeys.empty())
        {
            std::sort(matchingKeys.begin(), matchingKeys.end());
            target = meshObjects.at(matchingKeys.front()).model;
            found = true;
        }
    }
    else if (system.attachment.targetKind == "component")
    {
        std::string identity = system.attachment.targetName;
        if (identity.rfind("__blueprint/", 0) != 0)
            identity = "__blueprint/" + identity;
        if (!system.attachment.componentName.empty())
            identity += "/" + system.attachment.componentName;
        const auto matrix = scriptComponentMatrices.find(identity);
        if (matrix != scriptComponentMatrices.end())
        {
            target = matrix->second;
            found = true;
        }
        else if (!system.attachment.componentName.empty())
        {
            // 组件矩阵表尚未同步时，回退到冻结的运行时状态，避免出生帧
            // 附着目标短暂掉回作者原点。
            for (const auto& actor : scriptBlueprintStates)
            {
                if (actor.value("id", std::string()) !=
                    system.attachment.targetName)
                    continue;
                const auto components =
                    actor.value("components", nlohmann::json::array());
                for (size_t index = 0; index < components.size(); ++index)
                {
                    if (components[index].at("type") != "mesh" ||
                        ScriptComponentId(components[index]) !=
                            system.attachment.componentName)
                        continue;
                    target = ScriptComponentMatrix(actor, index);
                    found = true;
                    break;
                }
                if (found) break;
            }
        }
    }
    if (!found)
    {
        resolved = false;
        return system.model;
    }
    return target * local.GetModelMatrix();
}

bool World::SetSquareAreaLight(const std::string& name, const WorldSquareAreaLight& light)
{
    const auto it = squareAreaLights.find(name);
    if (it == squareAreaLights.end()) return false;
    it->second = light;
    return true;
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

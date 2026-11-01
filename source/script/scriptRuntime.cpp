#include "script/scriptRuntime.h"
#include "script/scriptDebugDraw.h"

#include "engine/diagnosticsSubsystem.h"
#include "input/inputSubsystem.h"
#include "script/scriptScene.h"
#include "script/scriptBlueprintAsset.h"
#include "script/scriptPhysics.h"
#include "script/scriptPhysicsMeshGeometry.h"
#include "script/scriptPropertySchema.h"
#include "script/scriptCompleteDeclaration.h"
#include "core/logStore.h"
#include "commonFunction.h"
#include "particles/particleAsset.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <filesystem>
#include "world/world.h"

namespace VL
{
namespace
{
void IndexScriptStates(ScriptWorldContext& context, const nlohmann::json& states)
{
    if (context.stateIndices.size() == states.size()) return;
    context.stateIndices.clear();
    context.physicsIndices.clear();
    for (size_t index = 0; index < states.size(); ++index)
    {
        const auto id = states[index].at("id").get<std::string>();
        context.stateIndices.emplace(id, index);
        if (context.declarations.at(id).contains("physics")) context.physicsIndices.push_back(index);
    }
}

bool HasScriptCamera(const nlohmann::json& state)
{
    if (!state.contains("components")) return false;
    for (const auto& node : state.at("components"))
        if (node.at("type") == "camera") return true;
    return false;
}

bool HasScriptSpringArm(const nlohmann::json& states)
{
    for (const auto& state : states)
        if (state.contains("components"))
            for (const auto& component : state.at("components"))
                if (component.at("type") == "springArm") return true;
    return false;
}

[[noreturn]] void ThrowParticleCommand(const std::string& detail)
{
    throw std::runtime_error("Script.InvalidParticleCommand: " + detail);
}

std::string ParticleCommandString(const nlohmann::json& command, const char* field,
    const std::string& operation)
{
    if (!command.contains(field) || !command.at(field).is_string())
        ThrowParticleCommand(operation + " requires string field '" + field + "'");
    const std::string value = command.at(field).get<std::string>();
    if (value.empty() || value.find('\0') != std::string::npos)
        ThrowParticleCommand(operation + " requires a non-empty string field '" + field + "'");
    return value;
}

float ParticleCommandNumber(const nlohmann::json& command, const char* field,
    const std::string& operation)
{
    if (!command.contains(field) || !command.at(field).is_number())
        ThrowParticleCommand(operation + " requires numeric field '" + field + "'");
    const double number = command.at(field).get<double>();
    if (!std::isfinite(number) ||
        std::abs(number) > static_cast<double>(std::numeric_limits<float>::max()))
        ThrowParticleCommand(operation + " field '" + field + "' must be finite");
    return static_cast<float>(number);
}

Eigen::Vector3f ParticleCommandVector3(const nlohmann::json& command, const char* field,
    const std::string& operation)
{
    if (!command.contains(field) || !command.at(field).is_array() ||
        command.at(field).size() != 3)
        ThrowParticleCommand(operation + " requires vec3 field '" + field + "'");
    Eigen::Vector3f result;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        const auto& value = command.at(field).at(axis);
        if (!value.is_number())
            ThrowParticleCommand(operation + " field '" + field + "' requires finite numbers");
        const double number = value.get<double>();
        if (!std::isfinite(number) ||
            std::abs(number) > static_cast<double>(std::numeric_limits<float>::max()))
            ThrowParticleCommand(operation + " field '" + field + "' requires finite numbers");
        result[static_cast<Eigen::Index>(axis)] = static_cast<float>(number);
    }
    return result;
}

bool ParticleCommandBool(const nlohmann::json& command, const char* field,
    const std::string& operation)
{
    if (!command.contains(field) || !command.at(field).is_boolean())
        ThrowParticleCommand(operation + " requires boolean field '" + field + "'");
    return command.at(field).get<bool>();
}

nlohmann::json BuildParticleTelemetryJson(const World& world)
{
    nlohmann::json result = nlohmann::json::array();
    for (const auto& entry : world.GetParticleSystems())
        result.push_back(
            world.DescribeParticleSystemTelemetry(entry.first));
    return result;
}

nlohmann::json BuildParticleEventsJson(
    std::vector<WorldParticleRuntimeEvent> events)
{
    nlohmann::json result = nlohmann::json::array();
    for (const auto& event : events)
    {
        result.push_back({
            {"system", event.systemName},
            {"emitter", event.emitterName},
            {"kind", event.kind},
            {"time", event.time},
            {"position", {event.position.x(), event.position.y(), event.position.z()}},
            {"normal", {event.normal.x(), event.normal.y(), event.normal.z()}}});
    }
    return result;
}

nlohmann::json BuildParticleDataChannelsJson(const World& world)
{
    // 排序后写入 JSON，避免 unordered_map 迭代顺序让每次 tick 的 payload
    // 字节发生变化，也便于 C#/诊断按稳定顺序比较。
    std::map<std::string, float> ordered(
        world.GetParticleDataChannels().begin(),
        world.GetParticleDataChannels().end());
    nlohmann::json result = nlohmann::json::object();
    for (const auto& [name, value] : ordered)
        result[name] = value;
    return result;
}

bool SameScriptVisuals(const nlohmann::json& before, const nlohmann::json& after)
{
    for (const char* field : {"position", "rotation", "scale", "components", "enabled", "failed", "destroyed"})
        if (!before.contains(field) || before.at(field) != after.at(field)) return false;
    return true;
}
}

ScriptWorldContext::~ScriptWorldContext() { Release(2); }

nlohmann::json ScriptWorldContext::Call(const char* operation, nlohmann::json fields)
{
    if (released) throw std::runtime_error("Script.StaleContext");
    fields["op"] = operation;
    fields["context"] = id;
    return host->Invoke(std::move(fields));
}

void ScriptWorldContext::Release(int reason) noexcept
{
    if (released) return;
    try { Call("release", {{"reason", reason}}); }
    catch (...) { /* 清理边界不能把托管异常带进 World/renderer 析构。 */ }
    released = true;
}

void ScriptRuntimeSubsystem::Initialize(bool enabled, const std::string& runtimeDirectory,
    const std::string& assembly)
{
    if (!enabled) return;
    if (host) throw std::runtime_error("Script runtime is already initialized");
    auto candidate = std::make_shared<ScriptHost>();
    candidate->Initialize(runtimeDirectory);
    auto catalog = candidate->Invoke({{"op", "initialize"}, {"assembly", assembly}});
    classes = catalog.at("result").at("classes");
    componentClasses = catalog.at("result").value("componentClasses", nlohmann::json::array());
    assemblyGeneration = catalog.at("result").at("generation");
    assemblyPath = assembly;
    host = std::move(candidate);
}

std::shared_ptr<ScriptWorldContext> ScriptRuntimeSubsystem::CreateContext(const nlohmann::json& blueprints,
    const nlohmann::json& templates, uint64_t generation, const nlohmann::json& animationCatalog,
    const nlohmann::json& animationAssets)
{
    if (!host) throw std::runtime_error("Script.Disabled: scene Blueprints require enabled C# hosting");
    const uint64_t id = nextContext++;
    auto context = std::make_shared<ScriptWorldContext>(host, id);
    // 先取得原生 RAII owner，prepare 或作者声明复制失败也会清理已登记的托管上下文。
    host->Invoke({{"op", "prepare"}, {"context", id}, {"blueprints", blueprints}, {"templates", templates},
        {"generation", generation ? generation : assemblyGeneration}, {"animationCatalog", animationCatalog},
        {"animationAssets", animationAssets}});
    for (const auto& blueprint : blueprints) context->declarations[blueprint.at("id").get<std::string>()] = blueprint;
    for (const auto& item : templates.items()) context->permanentAssetPaths[item.key()] = true;
    return context;
}

void ScriptRuntimeSubsystem::PrepareWorld(World& world, const nlohmann::json& scene,
    const nlohmann::json& authoredScene)
{
    world.SetMotionResourceRoot(resourceRoot);
    auto blueprints = nlohmann::json::array();
    auto sources = nlohmann::json::array();
    auto templates = nlohmann::json::object();
    auto resolved = nlohmann::json::array();
    for (const auto& object : (authoredScene.is_null() ? scene : authoredScene).at("objects"))
        if (object.at("type") == "blueprintInstance") sources.push_back(object);
    for (const auto& object : scene.at("objects"))
        if (object.at("type") == "blueprintInstance")
        {
            resolved.push_back(object);
            auto blueprint = CompleteBlueprintDeclaration(object, classes, componentClasses);
            if (blueprint.contains("__templatePath"))
            {
                templates[blueprint.at("__templatePath").get<std::string>()] = blueprint;
                world.ApplyScriptBlueprintVisuals(blueprint, false);
            }
            else
            {
                world.ApplyScriptBlueprintVisuals(blueprint);
                blueprints.push_back(std::move(blueprint));
            }
        }
    if (std::filesystem::path(world.GetScenePath()).filename() != "SC_model_preview.json")
        ValidateScriptPhysicsReferences(blueprints, templates);
    world.SetScriptBlueprints(blueprints);
    world.SetScriptResolvedBlueprints(std::move(resolved));
    world.PrepareScriptComponentMeshTemplates();
    world.SetScriptBlueprintSources(std::move(sources));
    world.SetScriptTemplates(templates);
    world.SetScriptAssetDigests(scene.value("__scriptAssets", nlohmann::json::object()));
    // 所有可 Spawn 类/属性也在候选准备中验证，不将坏模板延迟到游戏开始才发现。
    for (const auto& item : templates.items()) ValidateBlueprint(item.value());
    // 资产预览只发布作者几何；OnCreate 也可能移动/隐藏组件，不能在编辑预览中执行。
    if (!blueprints.empty() && std::filesystem::path(world.GetScenePath()).filename() != "SC_model_preview.json")
        world.SetScriptContext(CreateContext(blueprints, templates, 0, world.ScriptAnimationCatalog(),
            world.ScriptAnimationAssets()));
}

void ScriptRuntimeSubsystem::ValidateBlueprint(const nlohmann::json& blueprint)
{
    ValidateScriptBlueprint(blueprint);
    ValidateScriptPropertySchema(blueprint, classes);
    ValidateScriptLogicPropertySchema(blueprint, componentClasses);
    auto candidate = CompleteBlueprintDeclaration(blueprint, classes, componentClasses);
    candidate["enabled"] = blueprint.value("enabled", true);
    CreateContext(nlohmann::json::array({candidate}));
}

void ScriptRuntimeSubsystem::SynchronizeWorld(const std::shared_ptr<World>& world)
{
    if (activeWorld == world) return;
    // World 已经整体提交；先停止旧回调，再允许新 World 在下一个显式 Play 中执行。
    if (activeWorld)
    {
        activeWorld->GetDebugDraw().Clear();
        if (activeWorld->GetScriptContext()) activeWorld->GetScriptContext()->Release(1);
    }
    activeWorld = world;
    physics.reset();
    observedAssets = world ? world->GetScriptAssetDigests().dump() : "";
    pendingAssets.clear();
    assetPollTime = 0;
    state = ScriptPlayState::Editing;
    const double startup = time.startup;
    time = {};
    time.startup = startup;
    performance = nlohmann::json::object();
    performanceDetailElapsed = 0;
}

void ScriptRuntimeSubsystem::Stop(World& world)
{
    world.GetDebugDraw().Clear();
    // Play 上下文释放后不保留任何 GPU 事件回读兴趣，避免 Stop 后 EngineLoop
    // 继续在设备安全点轮询旧订阅。
    world.ClearParticleEventCaptureInterest();
    if (world.GetScriptContext()) world.GetScriptContext()->Release(0);
    if (world.GetScriptContext())
        for (const auto& item : world.GetScriptContext()->declarations.items())
            if (!item.value().value("__spawnPath", std::string()).empty())
                world.RemoveScriptBlueprintMeshes(item.key());
    for (const auto& blueprint : world.GetScriptBlueprints())
    {
        world.SynchronizeScriptComponentMeshes(blueprint);
        world.ApplyScriptBlueprintVisuals(blueprint);
    }
    world.SetScriptContext(nullptr);
    world.SetScriptBlueprintStates(nlohmann::json::array());
    world.ClearScriptCamera();
    world.ResetScriptAnimations();
    state = ScriptPlayState::Editing;
    physics.reset();
    const double startup = time.startup;
    time = {};
    time.startup = startup;
    performance = nlohmann::json::object();
    performanceDetailElapsed = 0;
}

bool ScriptRuntimeSubsystem::Control(World& world, ScriptPlayState requested, std::string& error)
{
    error.clear();
    const bool starting = state == ScriptPlayState::Editing && requested == ScriptPlayState::Playing;
    try
    {
        if (!host) throw std::runtime_error("C# hosting is disabled");
        if (requested != ScriptPlayState::Editing &&
            std::filesystem::path(world.GetScenePath()).filename() == "SC_model_preview.json")
            throw std::runtime_error("Asset preview does not run gameplay");
        if (requested != ScriptPlayState::Editing && requested != ScriptPlayState::Playing &&
            requested != ScriptPlayState::Paused) throw std::runtime_error("Unknown script play state");
        if (world.GetScriptBlueprints().empty()) throw std::runtime_error("This scene has no C# Blueprints");
        if (requested == ScriptPlayState::Editing) Stop(world);
        else if (requested == ScriptPlayState::Paused)
        {
            if (state != ScriptPlayState::Playing) throw std::runtime_error("Pause requires a playing scene");
            state = requested;
        }
        else
        {
            if (state == ScriptPlayState::Editing)
            {
                // 每次 Play 重建实例；Gameplay 静态字段属于程序集代，仅手动重载后重置。
                if (world.GetScriptContext()) world.GetScriptContext()->Release(0);
                world.ResetScriptAnimations();
                auto context = CreateContext(world.GetScriptBlueprints(), world.GetScriptTemplates(), 0,
                    world.ScriptAnimationCatalog(), world.ScriptAnimationAssets());
                context->Call("set_time", {{"time", ContextTime()}});
                // BeginPlay 之前先把 World 通道交给托管侧，保证新 Play 上下文
                // 在 BeginPlay 内读取到上一上下文/Console 留下的全局数据。
                const auto response = context->Call("start",
                    {{"particleDataChannels",
                        BuildParticleDataChannelsJson(world)}});
                world.SetScriptContext(std::move(context));
                physics = std::make_shared<ScriptPhysicsWorld>();
                physics->SetMeshGeometry(BuildScriptPhysicsMeshGeometry(world));
                Apply(world, response);
                  world.TickScriptAnimations(0);
                StepPhysics(world, 0);
                RefreshPerformance(world);
            }
            state = requested;
        }
        feedback.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        if (starting) Stop(world);
        feedback = error = exception.what();
        return false;
    }
}

void ScriptRuntimeSubsystem::CollectDiagnostics(const nlohmann::json& response, const World* world)
{
    for (const auto& diagnostic : response.at("diagnostics"))
    {
        auto entry = diagnostic;
        entry["assembly"] = assemblyPath;
        entry["assemblyGeneration"] = assemblyGeneration;
        if (world)
        {
            entry["scenePath"] = world->GetScenePath();
            entry["worldGeneration"] = world->GetGeneration();
        }
        pendingDiagnostics.push_back({LogSeverity::Error, entry.dump(), {}});
    }
    // 新桥接保留兼容文本列表，但有结构日志时只领取一次，避免同一条 Info 重复进入面板。
    if (response.contains("logEntries"))
        for (const auto& entry : response.at("logEntries"))
        {
            const auto severity = entry.value("severity", "Info");
            const auto level = severity == "Error" ? LogSeverity::Error : severity == "Warning" ? LogSeverity::Warning :
                severity == "Debug" ? LogSeverity::Debug : LogSeverity::Info;
            pendingDiagnostics.push_back({level, entry.at("message").get<std::string>(),
                {entry.value("file", std::string()), entry.value("line", 0u), entry.value("column", 0u)}});
        }
    else
        for (const auto& message : response.at("logs"))
            pendingDiagnostics.push_back({LogSeverity::Info, message.get<std::string>(), {}});
}

void ScriptRuntimeSubsystem::Apply(World& world, const nlohmann::json& response)
{
    const auto started = std::chrono::steady_clock::now();
    CollectDiagnostics(response, &world);
    // ID 集合由候选场景决定，不按返回顺序或显示名字猜测原生对象。
    const auto& updates = response.at("result");
    const bool delta = response.value("delta", false);
    auto& declarations = world.GetScriptContext()->declarations;
    if (updates.size() > 4096) throw std::runtime_error("Script.InvalidResponse: Blueprint budget exceeded");
    std::set<std::string> seen;
    for (const auto& update : updates)
    {
        const auto id = update.at("id").get<std::string>();
        if (!seen.insert(id).second) throw std::runtime_error("Script.InvalidResponse: Duplicate Blueprint identity");
        if (!declarations.contains(id))
        {
            const auto path = update.at("template").get<std::string>();
            if (!world.GetScriptTemplates().contains(path) || id.rfind("~spawn.", 0) != 0)
                throw std::runtime_error("Script.InvalidResponse: Unknown spawn template");
        }
    }
    if (!delta)
        for (const auto& item : declarations.items())
            if (!seen.count(item.key())) throw std::runtime_error("Script.InvalidResponse: Missing Blueprint identity");
    auto& states = world.ScriptBlueprintStatesForUpdate();
    auto& context = *world.GetScriptContext();
    IndexScriptStates(context, states);
    bool cameraChanged = false;
    // 本批取消先于申请领取预算；同一回调释放旧 lease 后可立即加载新资产，不受对象导出顺序影响。
    auto& assetContext = *world.GetScriptContext();
    for (const auto& update : updates)
        for (const auto& cancellation : update.value("assetCancels", nlohmann::json::array()))
        {
            const auto key = std::to_string(cancellation.get<uint64_t>());
            const auto found = assetContext.assetLoads.find(key);
            if (found == assetContext.assetLoads.end()) continue;
            if (found->at("owner") != update.at("id")) throw std::runtime_error("Script.InvalidAssetOwner");
            assetContext.assetLoads.erase(found);
            assetContext.assetCancellations.push_back(cancellation);
        }
    // 完整身份集合校验通过后才发布；托管回调边界已验证有限值与非零 scale。
    for (const auto& update : updates)
    {
        const auto id = update.at("id").get<std::string>();
        for (const auto& request : update.value("assetRequests", nlohmann::json::array()))
        {
            if (request.at("owner") != id) throw std::runtime_error("Script.InvalidAssetOwner");
            const auto key = std::to_string(request.at("id").get<uint64_t>());
            if (context.assetLoads.size() >= 256 || context.assetLoads.contains(key))
                throw std::runtime_error("Script.InvalidAssetRequest");
            context.assetLoads[key] = request;
            context.assetRequests.push_back(request);
        }
        for (const auto& ray : update.value("raycasts", nlohmann::json::array()))
            world.GetScriptContext()->raycasts.push_back(ray);
        if (!declarations.contains(id))
        {
            const auto path = update.at("template").get<std::string>();
            auto declaration = world.GetScriptTemplates().at(path);
            const auto templateId = declaration.at("id").get<std::string>();
            declaration["id"] = id; declaration["__spawnPath"] = path;
            declaration["__physicsTemplateId"] = templateId;
            for (const char* field : {"position", "rotation", "scale", "components"})
                declaration[field] = update.at(field);
            declarations[id] = declaration;
            world.CloneScriptBlueprintMeshes(templateId, declaration);
        }
        const auto found = context.stateIndices.find(id);
        const bool inserted = found == context.stateIndices.end();
        const size_t index = inserted ? states.size() : found->second;
        const auto& declaration = declarations.at(id);
        const bool componentsChanged = inserted || (update.contains("components") &&
            update.at("components") != states[index].at("components"));
        bool changed = inserted;
        const bool hadCamera = !inserted && HasScriptCamera(states[index]);
        if (delta && !inserted)
        {
            // 已验证的增量直接写 GT 状态槽；不会复制作者属性、组件或其它未变化字段。
            for (const char* field : {"position", "rotation", "scale", "components", "enabled", "failed", "destroyed"})
                if (update.contains(field) && update.at(field) != states[index].at(field)) changed = true;
            for (const auto& field : update.items()) states[index][field.key()] = field.value();
        }
        else
        {
            auto next = update;
            for (const char* field : {"class", "name", "properties", "physics", "__spawnPath"})
                if (declaration.contains(field)) next[field] = declaration.at(field);
            changed = inserted || !SameScriptVisuals(states[index], next);
            if (!inserted)
            {
                // 速度/着地由 Jolt 拥有；托管只发送新指令。
                for (const char* field : {"bodyVelocity", "grounded"})
                    if (states[index].contains(field)) next[field] = states[index].at(field);
                states[index] = std::move(next);
            }
            else states.push_back(std::move(next));
        }
        if (inserted)
        {
            context.stateIndices.emplace(id, index);
            if (declaration.contains("physics")) context.physicsIndices.push_back(index);
        }
        auto& current = states[index];
        // disabled owner 的订阅仍保留；C# 只在 CanRun 时派发。不要在禁停时
        // 清 native 兴趣，否则重新 Enable 后已存在的订阅无法自行恢复回读。
        if (current.value("failed", false) ||
            current.value("destroyed", false))
            world.SetParticleEventCaptureInterest(id, false);
        if (changed) cameraChanged = cameraChanged || hadCamera || HasScriptCamera(current);
        if (changed && current.at("destroyed").get<bool>())
        {
            if (!declaration.value("__spawnPath", std::string()).empty()) world.RemoveScriptBlueprintMeshes(id);
            else
            {
                // 作者对象保留隐藏原件供 Stop 恢复；运行时添加的动态网格不再持有待卸载资源。
                world.SynchronizeScriptComponentMeshes(declaration);
                world.ApplyScriptBlueprintVisuals(declaration, false);
            }
        }
        else if (changed)
        {
            if (componentsChanged) world.SynchronizeScriptComponentMeshes(current);
            world.ApplyScriptBlueprintVisuals(current);
        }
        if (update.contains("animationCommands"))
            world.ApplyScriptAnimationRequests(current, update.at("animationCommands"));
        try
        {
            if (update.contains("debugCommands") || (changed && (current.value("failed", false) ||
                current.value("destroyed", false) || !current.value("enabled", true))))
                ApplyScriptDebugDraw(world.GetDebugDraw(), current, update.value("debugCommands", nlohmann::json::array()));
        }
        catch (const std::invalid_argument& error)
        {
            // 公共绘图预算失败只拒绝该 owner 的整批图形，不让调试工具停止其它 gameplay。
            pendingDiagnostics.push_back({LogSeverity::Warning, "DrawDebug batch rejected for " + id + ": " + error.what(), {}});
        }
        // 托管粒子指令在 GT 命令点同步消费；任何非法指令都会抛出并停止本上下文，
        // 避免后续帧继续对错误对象写入运行时覆盖。
        if (update.contains("particleCommands"))
            ApplyParticleCommands(world, id, update.at("particleCommands"));
        // 请求队列已有独立 owner，快照不保留已领取的一次性资源/查询命令。
        for (const char* field : {"raycasts", "assetRequests", "assetCancels", "animationCommands",
            "debugCommands", "particleCommands"})
            if (current.contains(field) && !current.at(field).empty()) current[field] = nlohmann::json::array();
    }
    if (cameraChanged) world.UpdateScriptCamera(states);
    appliedUpdates += updates.size();
    applyMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void ScriptRuntimeSubsystem::ApplyParticleCommands(World& world, const std::string& owner,
    const nlohmann::json& commands)
{
    if (!commands.is_array())
        ThrowParticleCommand("particleCommands must be an array");
    if (commands.size() > 256)
        ThrowParticleCommand("particleCommands exceeds the 256-command budget");

    for (const auto& command : commands)
    {
        if (!command.is_object())
            ThrowParticleCommand("particle command must be an object");

        const std::string commandOwner = ParticleCommandString(command, "owner", "particle command");
        if (commandOwner != owner)
            ThrowParticleCommand("owner mismatch: expected '" + owner + "', got '" + commandOwner + "'");

        const std::string operation = ParticleCommandString(command, "op", "particle command");
        // 事件订阅兴趣是 owner 级命令，不需要粒子实例 id；它只改变
        // EngineLoop 是否在安全点回读 GPU 事件，不产生任何渲染资源副作用。
        if (operation == "events")
        {
            if (!world.SetParticleEventCaptureInterest(
                    owner,
                    ParticleCommandBool(command, "enabled", operation)))
                ThrowParticleCommand("events interest was rejected");
            continue;
        }
        // 数据通道是 owner 校验后的 World 全局写入口，不需要粒子实例 id；
        // 与逐实例 userParameters 分开存储，避免把全局值写进某个系统快照。
        if (operation == "data" || operation == "clearData")
        {
            const std::string key =
                ParticleCommandString(command, "key", operation);
            const bool applied = operation == "data"
                ? world.SetParticleDataChannel(
                    key,
                    ParticleCommandNumber(command, "value", operation))
                : world.ClearParticleDataChannel(key);
            if (!applied)
                ThrowParticleCommand(
                    operation + " was rejected for data channel '" +
                    key + "'");
            continue;
        }
        const std::string id = ParticleCommandString(command, "id", operation);

        if (operation == "spawn")
        {
            const std::string path = ParticleCommandString(command, "path", operation);
            if (world.FindParticleSystem(id) != nullptr)
                throw std::runtime_error("Script.InvalidParticleSpawn: duplicate id: " + id);

            const Eigen::Vector3f position = ParticleCommandVector3(command, "position", operation);
            const Eigen::Vector3f rotation = ParticleCommandVector3(command, "rotation", operation);
            const Eigen::Vector3f scale = ParticleCommandVector3(command, "scale", operation);
            if (std::abs(scale.x()) < 1e-6f || std::abs(scale.y()) < 1e-6f ||
                std::abs(scale.z()) < 1e-6f)
            {
                ThrowParticleCommand("spawn scale axes must have absolute value >= 1e-6");
            }

            ParticleSystemAsset asset;
            try
            {
                asset = ParticleAssetLoader::Prepare(path);
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error("Script.InvalidParticleSpawn: failed to load particle asset '" +
                    path + "': " + error.what());
            }

            Eigen::Matrix4f scaleMatrix = Eigen::Matrix4f::Identity();
            scaleMatrix(0, 0) = scale.x();
            scaleMatrix(1, 1) = scale.y();
            scaleMatrix(2, 2) = scale.z();
            Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
            translation.block<3, 1>(0, 3) = position;

            WorldParticleSystem system;
            system.name = id;
            system.assetPath = path;
            system.asset = std::move(asset);
            system.scenePosition = position;
            system.sceneRotation = rotation;
            system.sceneScale = scale;
            system.visible = true;
            system.model = translation * CommonFunction::RotationToMatrix(rotation) * scaleMatrix;
            world.AddParticleSystem(id, std::move(system));
            continue;
        }

        if (operation == "destroy")
        {
            // EndPlay 清理与显式销毁可能落在同一批指令里；不存在时保持幂等。
            if (world.FindParticleSystem(id) != nullptr) world.RemoveParticleSystem(id);
            continue;
        }

        if (operation == "spawnEmitter")
        {
            if (!command.contains("source") ||
                !command.at("source").is_number_integer())
                ThrowParticleCommand(
                    "spawnEmitter requires integer field 'source'");
            const int64_t source = command.at("source").get<int64_t>();
            if (source < 0 || source >
                static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                ThrowParticleCommand("spawnEmitter field 'source' is out of range");
            std::string name;
            if (command.contains("emitter"))
                name = ParticleCommandString(command, "emitter", operation);
            uint32_t stableIndex = 0;
            if (!world.SpawnParticleEmitter(
                    id, static_cast<uint32_t>(source), name, stableIndex))
                throw std::runtime_error(
                    "Script.InvalidParticleEmitter: spawn was rejected for '" +
                    id + "'");
            continue;
        }

        if (operation == "destroyEmitter" || operation == "restartEmitter")
        {
            uint32_t emitterIndex = 0;
            if (command.contains("index") &&
                command.at("index").is_number_integer())
            {
                const int64_t index = command.at("index").get<int64_t>();
                if (index < 0 ||
                    index > static_cast<int64_t>(
                        std::numeric_limits<uint32_t>::max()))
                    ThrowParticleCommand(
                        operation + " field 'index' is out of range");
                emitterIndex = static_cast<uint32_t>(index);
            }
            else
            {
                const std::string emitter =
                    ParticleCommandString(command, "emitter", operation);
                if (!world.FindParticleEmitterIndex(
                        id, emitter, emitterIndex))
                    throw std::runtime_error(
                        "Script.InvalidParticleEmitter: " + emitter);
            }
            const bool applied = operation == "destroyEmitter"
                ? world.DestroyParticleEmitter(id, emitterIndex)
                : world.RestartParticleEmitter(id, emitterIndex);
            if (!applied)
                throw std::runtime_error(
                    "Script.InvalidParticleEmitter: " + operation +
                    " was rejected for '" + id + "'");
            continue;
        }

        if (operation == "attach" || operation == "detach")
        {
            bool applied = false;
            if (operation == "detach")
            {
                applied = world.DetachParticleSystem(id);
            }
            else
            {
                const std::string kind =
                    ParticleCommandString(command, "targetKind", operation);
                const std::string target =
                    ParticleCommandString(command, "targetName", operation);
                std::string component;
                if (command.contains("component") &&
                    !command.at("component").is_null())
                    component = ParticleCommandString(
                        command, "component", operation);
                applied = world.AttachParticleSystem(
                    id,
                    kind,
                    target,
                    component,
                    ParticleCommandVector3(
                        command, "position", operation),
                    ParticleCommandVector3(
                        command, "rotation", operation),
                    ParticleCommandVector3(
                        command, "scale", operation));
            }
            if (!applied)
                ThrowParticleCommand(
                    operation + " was rejected for particle system '" + id +
                    "'");
            continue;
        }

        if (operation == "budget")
        {
            if (!command.contains("maxTotal") ||
                !command.at("maxTotal").is_number_integer())
                ThrowParticleCommand("budget requires integer field 'maxTotal'");
            const int64_t maxTotal = command.at("maxTotal").get<int64_t>();
            if (maxTotal < 0 ||
                maxTotal > 1000000)
                ThrowParticleCommand("budget field 'maxTotal' is out of range");
            if (!world.SetParticleSystemBudget(
                    id,
                    static_cast<uint32_t>(maxTotal),
                    ParticleCommandNumber(command, "emitterScale", operation)))
                ThrowParticleCommand(
                    "budget was rejected for particle system '" + id + "'");
            continue;
        }

        if (operation == "userParam" || operation == "clearUserParam")
        {
            const std::string key =
                ParticleCommandString(command, "key", operation);
            const bool applied = operation == "userParam"
                ? world.SetParticleSystemUserParameter(
                    id, key, ParticleCommandNumber(command, "value", operation))
                : world.ClearParticleSystemUserParameter(id, key);
            if (!applied)
                ThrowParticleCommand(
                    operation + " was rejected for particle system '" + id +
                    "'");
            continue;
        }

        if (operation != "visible" && operation != "spawnRateScale" &&
            operation != "colorTint" && operation != "emitter" &&
            operation != "reset")
            ThrowParticleCommand("unsupported particle command '" + operation + "'");

        if (world.FindParticleSystem(id) == nullptr)
            throw std::runtime_error("Script.InvalidParticleHandle: " + id);

        bool applied = false;
        if (operation == "visible")
        {
            applied = world.SetParticleSystemVisible(id, ParticleCommandBool(command, "value", operation));
        }
        else if (operation == "spawnRateScale")
        {
            applied = world.SetParticleSystemSpawnRateScale(
                id, ParticleCommandNumber(command, "value", operation));
        }
        else if (operation == "colorTint")
        {
            const auto& value = command.value("value", nlohmann::json());
            Eigen::Vector4f colorTint;
            if (!value.is_array() || value.size() != 4)
                ThrowParticleCommand("colorTint requires vec4 field 'value'");
            for (size_t channel = 0; channel < 4; ++channel)
            {
                const auto& number = value.at(channel);
                if (!number.is_number())
                    ThrowParticleCommand("colorTint field 'value' requires finite numbers");
                const double channelValue = number.get<double>();
                if (!std::isfinite(channelValue) ||
                    std::abs(channelValue) > static_cast<double>(std::numeric_limits<float>::max()))
                    ThrowParticleCommand("colorTint field 'value' requires finite numbers");
                colorTint[static_cast<Eigen::Index>(channel)] = static_cast<float>(channelValue);
            }
            applied = world.SetParticleSystemColorTint(id, colorTint);
        }
        else if (operation == "emitter")
        {
            uint32_t index = 0;
            if (command.contains("index") &&
                command.at("index").is_number_integer())
            {
                int64_t value = 0;
                try
                {
                    value = command.at("index").get<int64_t>();
                }
                catch (const std::exception&)
                {
                    ThrowParticleCommand("emitter field 'index' is out of range");
                }
                if (value < 0 ||
                    value > static_cast<int64_t>(
                        std::numeric_limits<uint32_t>::max()))
                    ThrowParticleCommand("emitter field 'index' is out of range");
                index = static_cast<uint32_t>(value);
            }
            else
            {
                const std::string emitter =
                    ParticleCommandString(command, "emitter", operation);
                if (!world.FindParticleEmitterIndex(id, emitter, index))
                    throw std::runtime_error(
                        "Script.InvalidParticleEmitter: " + emitter);
            }
            applied = world.SetParticleSystemEmitterEnabled(
                id, index, ParticleCommandBool(command, "enabled", operation));
        }
        else if (operation == "reset")
        {
            applied = world.ResetParticleSystemRuntimeOverrides(id);
        }
        if (!applied)
            ThrowParticleCommand(operation + " was rejected for particle system '" + id + "'");
    }
}

void ScriptRuntimeSubsystem::StepPhysics(World& world, float dt, bool collectQueries)
{
    const auto started = std::chrono::steady_clock::now();
    auto& context = *world.GetScriptContext();
    auto& allStates = world.ScriptBlueprintStatesForUpdate();
    IndexScriptStates(context, allStates);
    // 只有物理 owner 进入 Jolt 同步；无刚体的逻辑蓝图不会随物理帧复制。
    // dt 是本帧时间（已乘 timeScale）：角色按帧推进，刚体在物理世界内部按 1/60 子步追赶；
    // dt=0 的指令同步帧只同步状态/指令并收集接触，不移动任何实体。
    auto states = nlohmann::json::array();
    for (const auto index : context.physicsIndices) states.push_back(allStates.at(index));
    if (!states.empty()) physics->Step(states, context.declarations, dt);
    bool cameraChanged = false;
    for (auto& current : states)
    {
        auto& previous = allStates.at(context.stateIndices.at(current.at("id").get<std::string>()));
        if (!SameScriptVisuals(previous, current) && !current.value("destroyed", false))
        {
            world.ApplyScriptBlueprintVisuals(current);
            cameraChanged = cameraChanged || HasScriptCamera(current);
        }
        nlohmann::json synchronization = {{"id", current.at("id")}};
        for (const char* field : {"position", "rotation", "scale", "bodyVelocity", "grounded"})
            if (current.contains(field)) synchronization[field] = current.at(field);
        context.physicsUpdates.push_back(std::move(synchronization));
        previous = std::move(current);
    }
    auto queries = nlohmann::json::array();
    if (collectQueries)
    {
        queries = std::move(context.raycasts);
        context.raycasts = nlohmann::json::array();
    }
    for (auto& query : queries)
        if (query.value("localSpace", false))
        {
            const auto owner = query.at("owner").get<std::string>();
            const auto found = context.stateIndices.find(owner);
            if (found != context.stateIndices.end())
            {
                const auto& current = allStates.at(found->second);
                // 查询端点必须使用刚刚完成的物理位姿；相机不能以 Tick 前的旧根位置避障。
                const Eigen::Matrix4f matrix = ScriptTransformMatrix(current);
                for (const char* field : {"from", "to"})
                {
                    const Eigen::Vector3f point = (matrix * ScriptVector(query, field).homogeneous()).head<3>();
                    query[field] = {point.x(), point.y(), point.z()};
                }
            }
        }
    for (const auto& hit : physics->Raycast(queries)) context.rayHits.push_back(hit);
    if (cameraChanged) world.UpdateScriptCamera(allStates);
    physicsMilliseconds += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
}

void ScriptRuntimeSubsystem::Tick(const std::shared_ptr<World>& world, float dt,
    const InputActionState& input, const DiagnosticsSubsystem& diagnostics)
{
    SynchronizeWorld(world);
    time.startup += dt;
    time.delta = 0;
    time.unscaledDelta = 0;
    physicsMilliseconds = 0;
    applyMilliseconds = 0; snapshotMilliseconds = 0; appliedUpdates = 0;
    detailSamples = 0;
    if (host) host->ResetMetrics();
    if (world && state == ScriptPlayState::Playing)
    {
        try
        {
            auto& context = *world->GetScriptContext();
            time.unscaledDelta = dt;
            time.delta = dt * time.scale;
            world->GetDebugDraw().AdvanceGameTime(static_cast<float>(time.delta));
            time.play += time.delta;
            time.unscaledPlay += dt;
            ++time.frame;
            // 每帧一次脚本时间轴：没有 accumulator，也没有 fixed 回调；dt 直接是变帧间隔。
            auto assetResults = std::move(context.assetResults), assetUnloads = std::move(context.assetUnloads);
            context.assetResults = nlohmann::json::array(); context.assetUnloads = nlohmann::json::array();
            Apply(*world, world->GetScriptContext()->Call("tick", {{"dt", time.delta}, {"time", ContextTime()}, {"input",
                {int(input.moveRight) - int(input.moveLeft), int(input.moveForward) - int(input.moveBackward),
                 int(input.moveUp) - int(input.moveDown)}}, {"deferLate", true},
                 {"inputMap", input.gameplay}, {"animationFeedback", world->ScriptAnimationFeedback()},
                 {"assetResults", assetResults}, {"assetUnloads", assetUnloads},
                 {"states", context.TakePhysicsUpdates()},
                 {"particleTelemetry", BuildParticleTelemetryJson(*world)},
                 {"particleEvents", BuildParticleEventsJson(
                     world->TakeParticleRuntimeEvents())},
                 {"particleDataChannels",
                     BuildParticleDataChannelsJson(*world)},
                 {"delta", true}}));
            // 帧物理：Tick 写入的角色/刚体指令在同一次同步里领取，随后角色按帧 dt 移动、
            // 刚体按内部 1/60 子步推进；Tick 发起的查询在本帧物理位姿上执行。
            StepPhysics(*world, time.delta);
            auto rayHits = std::move(world->GetScriptContext()->rayHits);
            world->GetScriptContext()->rayHits = nlohmann::json::array();
            Apply(*world, world->GetScriptContext()->Call("late", {{"dt", time.delta}, {"states", context.TakePhysicsUpdates()},
                {"delta", true}, {"contacts", physics->Contacts()}, {"triggers", physics->Triggers()}, {"rayHits", rayHits}}));
            // LateTick 可能移动根或碰撞体；先用零时步把这些状态同步到 Jolt，
            // 这样动画中的 Foot IK 查询看到的是本帧最终支撑体，而不是旧物理位姿。
            // 零时步只领取一次 LateTick 指令并刷新接触/查询，不推进物理时间。
            StepPhysics(*world, 0);
            // 动画使用同一倍率时钟，在最终物理反馈与 LateTick 决策后求值；Pause 没有此路径。
            // 脚探测借用已同步的这一帧物理世界，不持有后端指针。
            world->TickScriptAnimations(static_cast<float>(time.delta), physics.get());
            if (HasScriptSpringArm(world->GetScriptBlueprintStates()))
            {
                // 相机臂在最终物理位姿上求值；只执行查询，不再推进物理或重放用户 LateTick。
                // LateTick 查询结果仍留在 context.rayHits，保持原有下一帧领取语义。
                Apply(*world, context.Call("camera_prepare", {{"states", context.TakePhysicsUpdates()}, {"delta", true}}));
                // 父臂完成后才生成嵌套子臂的查询，直到本帧挂接链全部收敛；不推进物理时钟。
                do
                {
                    auto cameraQueries = std::move(context.raycasts);
                    context.raycasts = nlohmann::json::array();
                    Apply(*world, context.Call("camera_finish",
                        {{"rayHits", physics->Raycast(cameraQueries)}, {"delta", true}}));
                } while (!context.raycasts.empty());
            }
            // 摘要与时钟每帧发布，回调明细最多 10 Hz；托管故障/生命周期变化可要求立即刷新。
            performanceDetailElapsed += dt;
            RefreshPerformance(*world, performanceDetailElapsed + 1e-8 >= 0.1);
        }
        catch (const std::exception& exception)
        {
            feedback = exception.what();
            auto diagnostic = nlohmann::json::parse(feedback, nullptr, false);
            if (!diagnostic.is_object()) diagnostic = {{"error", feedback}};
            diagnostic["assembly"] = assemblyPath;
            diagnostic["assemblyGeneration"] = assemblyGeneration;
            diagnostic["scenePath"] = world->GetScenePath();
            diagnostic["worldGeneration"] = world->GetGeneration();
            diagnostic["frame"] = time.frame;
            diagnostic["callback"] = "Runtime";
            diagnostic["action"] = "StopContext";
            pendingDiagnostics.push_back({LogSeverity::Error, diagnostic.dump(), {}});
            Stop(*world);
        }
    }
    for (const auto& entry : pendingDiagnostics)
    {
        switch (entry.severity)
        {
        case LogSeverity::Debug: diagnostics.ReportDebug(entry.message, "Script", entry.location); break;
        case LogSeverity::Warning: diagnostics.ReportWarning(entry.message, "Script", entry.location); break;
        case LogSeverity::Error: diagnostics.ReportError(entry.message, "Script", entry.location); break;
        default: diagnostics.ReportInfo(entry.message, "Script", entry.location); break;
        }
    }
    pendingDiagnostics.clear();
}

nlohmann::json ScriptRuntimeSubsystem::TakeAssetRequests(World& world)
{
    if (!world.GetScriptContext()) return nlohmann::json::array();
    auto result = std::move(world.GetScriptContext()->assetRequests);
    world.GetScriptContext()->assetRequests = nlohmann::json::array();
    return result;
}

nlohmann::json ScriptRuntimeSubsystem::TakeAssetCancellations(World& world)
{
    if (!world.GetScriptContext()) return nlohmann::json::array();
    auto result = std::move(world.GetScriptContext()->assetCancellations);
    world.GetScriptContext()->assetCancellations = nlohmann::json::array();
    return result;
}

bool ScriptRuntimeSubsystem::CompleteAssetLoad(World& world, uint64_t id, const std::string& path,
    const nlohmann::json& blueprint, const std::string& error)
{
    const auto context = world.GetScriptContext();
    if (!context || IsEditing()) return false;
    const auto key = std::to_string(id);
    const auto found = context->assetLoads.find(key);
    if (found == context->assetLoads.end() || found->at("path") != path || found->value("completed", false)) return false;
    auto result = nlohmann::json{{"id", id}, {"path", path}, {"error", error}, {"templates", nlohmann::json::array()}};
    if (error.empty())
    {
        if (!world.GetScriptTemplates().contains(path) || blueprint.value("__templatePath", "") != path)
            throw std::invalid_argument("Script.AssetCompletionRequiresPublishedTemplate");
        found->operator[]("completed") = true;
        if (!context->permanentAssetPaths.contains(path)) context->dynamicAssetPaths[path] = blueprint;
        result["templates"].push_back(blueprint);
        // GPU/CPU 候选已经提交；仅补充查询几何表，保留正在运行的刚体和约束。
        if (physics) physics->SetMeshGeometry(BuildScriptPhysicsMeshGeometry(world));
          context->Call("animation_catalog", {{"catalog", world.ScriptAnimationCatalog()},
              {"assets", world.ScriptAnimationAssets()}});
    }
    else context->assetLoads.erase(found);
    context->assetResults.push_back(std::move(result));
    return true;
}

std::vector<std::string> ScriptRuntimeSubsystem::CollectUnusedBlueprintAssets(const World& world) const
{
    std::vector<std::string> result;
    const auto context = world.GetScriptContext();
    if (!context) return result;
    for (const auto& item : context->dynamicAssetPaths.items())
    {
        bool pinned = false;
        for (const auto& load : context->assetLoads.items())
            pinned = pinned || load.value().at("path") == item.key();
        // 活实例和通过 AddComponent 复用的动态网格都持有资源引用；只看 Spawn 数量会提前卸载组件资源。
        for (const auto& state : world.GetScriptBlueprintStates())
        {
            if (state.value("destroyed", false)) continue;
            if (state.value("__spawnPath", "") == item.key()) pinned = true;
            for (const auto& component : state.value("components", nlohmann::json::array()))
                if (component.value("type", "") == "mesh")
                    for (const auto& source : item.value().value("components", nlohmann::json::array()))
                        if (source.value("type", "") == "mesh" &&
                            source.at("modelPath") == component.at("modelPath") &&
                            source.value("materialOverride", "") == component.value("materialOverride", "")) pinned = true;
        }
        if (!pinned) result.push_back(item.key());
    }
    return result;
}

void ScriptRuntimeSubsystem::ForgetBlueprintAsset(World& world, const std::string& path)
{
    const auto context = world.GetScriptContext();
    if (!context || !context->dynamicAssetPaths.contains(path)) return;
    context->dynamicAssetPaths.erase(path);
    context->assetUnloads.push_back(path);
    if (physics) physics->SetMeshGeometry(BuildScriptPhysicsMeshGeometry(world));
}

void ScriptRuntimeSubsystem::Shutdown() noexcept
{
    if (activeWorld) activeWorld->GetDebugDraw().Clear();
    if (activeWorld && activeWorld->GetScriptContext()) activeWorld->GetScriptContext()->Release(2);
    activeWorld.reset();
    physics.reset();
    state = ScriptPlayState::Editing;
    if (host)
    {
        try { host->Invoke({{"op", "unload"}, {"generation", assemblyGeneration}}); }
        catch (...) { /* shutdown 不能传播托管清理异常。 */ }
        host.reset();
    }
    assemblyGeneration = 0;
    assemblyPath.clear();
    componentClasses = nlohmann::json::array();
    performance = nlohmann::json::object();
    performanceDetailElapsed = 0;
    time = {};
}

bool ScriptRuntimeSubsystem::ReloadAssembly(World& world, std::string& error, const std::string& candidateAssembly)
{
    error.clear();
    if (!IsEditing()) return ReloadPlayingAssembly(world, error, candidateAssembly);
    uint64_t candidateGeneration = 0;
    try
    {
        if (!host) throw std::runtime_error("C# hosting is disabled");
        if (std::filesystem::path(world.GetScenePath()).filename() == "SC_model_preview.json")
            throw std::runtime_error("Return to the level before reloading C#");
        auto nextAssemblyPath = candidateAssembly.empty() ? assemblyPath : candidateAssembly;
        auto loaded = host->Invoke({{"op", "initialize"}, {"assembly", nextAssemblyPath}});
        candidateGeneration = loaded.at("result").at("generation");
        auto candidateClasses = loaded.at("result").at("classes");
        auto candidateComponentClasses = loaded.at("result").at("componentClasses");
        auto blueprints = nlohmann::json::array();
        auto templates = nlohmann::json::object();
        for (const auto& source : world.GetScriptResolvedBlueprints())
        {
            auto blueprint = CompleteBlueprintDeclaration(source, candidateClasses, candidateComponentClasses);
            if (blueprint.contains("__templatePath"))
            {
                CreateContext(nlohmann::json::array({blueprint}), nlohmann::json::object(), candidateGeneration);
                const auto path = blueprint.at("__templatePath").get<std::string>();
                templates[path] = std::move(blueprint);
            }
            else blueprints.push_back(std::move(blueprint));
        }
        auto candidateContext = CreateContext(blueprints, templates, candidateGeneration,
            world.ScriptAnimationCatalog(), world.ScriptAnimationAssets());
        std::string successMessage = "C# assembly reloaded";
        // 所有类、权限、属性与 OnCreate 已通过；只交换脚本配置，不重新准备 GPU 资源。
        world.SwapScriptConfiguration(blueprints, templates, candidateContext);
        classes.swap(candidateClasses);
        componentClasses.swap(candidateComponentClasses);
        const auto previousGeneration = assemblyGeneration;
        assemblyGeneration = candidateGeneration;
        assemblyPath.swap(nextAssemblyPath);
        candidateGeneration = 0;
        if (candidateContext) candidateContext->Release(0);
        candidateContext.reset();
        try { host->Invoke({{"op", "unload"}, {"generation", previousGeneration}}); }
        catch (...) { /* 提交已完成；清理诊断不能把成功换代重新报告为候选失败。 */ }
        feedback.swap(successMessage);
        return true;
    }
    catch (const std::exception& exception)
    {
        if (candidateGeneration)
            try { host->Invoke({{"op", "unload"}, {"generation", candidateGeneration}}); } catch (...) {}
        feedback = error = exception.what();
        return false;
    }
}

bool ScriptRuntimeSubsystem::PollBlueprintAssetChanges(const World& world, float dt)
{
    if (!host || !IsEditing() || resourceRoot.empty() || world.GetScriptAssetDigests().empty()) return false;
    assetPollTime += dt;
    if (assetPollTime < 0.5f) return false;
    assetPollTime = 0;
    auto signatures = nlohmann::json::object();
    for (const auto& item : world.GetScriptAssetDigests().items())
    {
        try
        {
            const auto path = ResolveScriptAssetPath(resourceRoot, item.key());
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 4 * 1024 * 1024)
                signatures[item.key()] = "missing-or-oversize";
            else signatures[item.key()] = ContentHasher::HashFile(path).ToHex();
        }
        catch (const std::exception& error) { signatures[item.key()] = error.what(); }
    }
    const auto signature = signatures.dump();
    if (signature == observedAssets) { pendingAssets.clear(); return false; }
    // 两次相同的内容采样才触发；失败内容也记为已观察，修正文件后才重试，避免每帧报错。
    if (signature != pendingAssets) { pendingAssets = signature; return false; }
    observedAssets = signature;
    pendingAssets.clear();
    return true;
}

ScriptRuntimeView ScriptRuntimeSubsystem::View(uint64_t generation) const
{
    ScriptRuntimeView result{host != nullptr, state, generation, classes, feedback};
    result.componentClasses = componentClasses;
    result.timeScale = time.scale;
    result.performance = performance;
    if (!result.performance.contains("blueprints")) result.performance["blueprints"] = nlohmann::json::array();
    if (!result.performance.contains("pending"))
        result.performance["pending"] = {{"timers", 0}, {"subscriptions", 0}, {"events", 0}, {"rays", 0},
            {"assets", 0}, {"commands", 0}, {"componentCommands", 0}};
    result.performance["time"] = ContextTime();
    result.performance["physicsMs"] = physicsMilliseconds;
    result.performance["detailsAgeSeconds"] = performanceDetailElapsed;
    // HUD 是 GT 运行值的冻结副本；UI 只消费文本，不获得托管对象、委托或 DOM 权限。
    if (activeWorld && state != ScriptPlayState::Editing && activeWorld->GetGeneration() == generation)
        for (const auto& blueprint : activeWorld->GetScriptBlueprintStates())
            if (blueprint.contains("hud") && !blueprint.value("destroyed", false) &&
                !blueprint.value("failed", false) && blueprint.value("enabled", true))
                for (const auto& line : blueprint.at("hud"))
                    result.hudLines.push_back({blueprint.at("id").get<std::string>(),
                        line.at("component").get<std::string>(), line.at("key").get<std::string>(),
                        line.at("text").get<std::string>()});
    const auto metrics = host ? host->Metrics() : ScriptBridgeMetrics{};
    result.performance["pipeline"] = {{"serializeMs", metrics.serializeMs}, {"managedMs", metrics.managedMs},
        {"parseMs", metrics.parseMs}, {"applyMs", applyMilliseconds}, {"snapshotMs", snapshotMilliseconds},
        {"requestBytes", metrics.requestBytes}, {"responseBytes", metrics.responseBytes},
        {"calls", metrics.calls}, {"updates", appliedUpdates}};
    result.performance["pipeline"]["detailSamples"] = detailSamples;
    return result;
}

bool ScriptRuntimeSubsystem::SetTimeScale(float value, std::string& error)
{
    error.clear();
    if (!host || !std::isfinite(value) || value < 0 || value > 100)
    {
        error = "TimeScale requires an enabled host and a finite value in [0,100]";
        return false;
    }
    time.scale = value;
    return true;
}

nlohmann::json ScriptRuntimeSubsystem::ContextTime() const
{
    // 每帧只有一条时间轴：稳定帧索引 + 变长 dt；没有固定步计数，也没有丢弃步数。
    return {{"deltaTime", time.delta}, {"unscaledDeltaTime", time.unscaledDelta},
        {"timeScale", time.scale}, {"timeSinceStartup", time.startup}, {"timeSincePlay", time.play},
        {"unscaledTimeSincePlay", time.unscaledPlay}, {"frameIndex", time.frame}};
}

void ScriptRuntimeSubsystem::RefreshPerformance(World& world, bool details)
{
    const auto started = std::chrono::steady_clock::now();
    auto snapshot = world.GetScriptContext()->Call("runtime_snapshot", {{"details", details}}).at("result");
    if (snapshot.contains("blueprints"))
    {
        performanceDetailElapsed = 0;
        ++detailSamples;
    }
    // 缺省字段保留最近完整明细，UI 得到冻结值和采样年龄，不读取 live CLR。
    for (auto& field : snapshot.items()) performance[field.key()] = std::move(field.value());
    snapshotMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}
} // namespace VL

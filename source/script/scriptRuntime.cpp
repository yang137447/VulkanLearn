#include "script/scriptRuntime.h"

#include "engine/diagnosticsSubsystem.h"
#include "input/inputSubsystem.h"
#include "script/scriptScene.h"
#include "script/scriptBlueprintAsset.h"
#include "script/scriptPhysics.h"
#include "script/scriptPhysicsMeshGeometry.h"
#include "script/scriptPropertySchema.h"
#include "script/scriptCompleteDeclaration.h"
#include "core/logStore.h"
#include <chrono>
#include <cmath>
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
    const nlohmann::json& templates, uint64_t generation)
{
    if (!host) throw std::runtime_error("Script.Disabled: scene Blueprints require enabled C# hosting");
    const uint64_t id = nextContext++;
    auto context = std::make_shared<ScriptWorldContext>(host, id);
    // 先取得原生 RAII owner，prepare 或作者声明复制失败也会清理已登记的托管上下文。
    host->Invoke({{"op", "prepare"}, {"context", id}, {"blueprints", blueprints}, {"templates", templates},
        {"generation", generation ? generation : assemblyGeneration}});
    for (const auto& blueprint : blueprints) context->declarations[blueprint.at("id").get<std::string>()] = blueprint;
    for (const auto& item : templates.items()) context->permanentAssetPaths[item.key()] = true;
    return context;
}

void ScriptRuntimeSubsystem::PrepareWorld(World& world, const nlohmann::json& scene,
    const nlohmann::json& authoredScene)
{
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
        world.SetScriptContext(CreateContext(blueprints, templates));
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
    if (activeWorld && activeWorld->GetScriptContext())
        activeWorld->GetScriptContext()->Release(1);
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
                auto context = CreateContext(world.GetScriptBlueprints(), world.GetScriptTemplates());
                context->Call("set_time", {{"time", ContextTime()}});
                const auto response = context->Call("start");
                world.SetScriptContext(std::move(context));
                physics = std::make_shared<ScriptPhysicsWorld>();
                physics->SetMeshGeometry(BuildScriptPhysicsMeshGeometry(world));
                Apply(world, response);
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
        // 请求队列已有独立 owner，快照不保留已领取的一次性资源/查询命令。
        for (const char* field : {"raycasts", "assetRequests", "assetCancels"})
            if (current.contains(field) && !current.at(field).empty()) current[field] = nlohmann::json::array();
    }
    if (cameraChanged) world.UpdateScriptCamera(states);
    appliedUpdates += updates.size();
    applyMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void ScriptRuntimeSubsystem::StepPhysics(World& world, bool advance, bool collectQueries)
{
    const auto started = std::chrono::steady_clock::now();
    auto& context = *world.GetScriptContext();
    auto& allStates = world.ScriptBlueprintStatesForUpdate();
    IndexScriptStates(context, allStates);
    // 只有物理 owner 进入 Jolt 同步；无刚体的逻辑蓝图不会随物理子步复制。
    auto states = nlohmann::json::array();
    for (const auto index : context.physicsIndices) states.push_back(allStates.at(index));
    if (!states.empty()) physics->StepFixed(states, context.declarations, advance);
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
    time.fixedSteps = 0;
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
            time.play += time.delta;
            time.unscaledPlay += dt;
            ++time.frame;
            constexpr double fixedStep = 1.0 / 60;
            time.accumulated += time.delta;
            const double requestedSteps = std::floor((time.accumulated + 1e-9) / fixedStep);
            time.fixedSteps = unsigned(std::min(requestedSteps, 8.0));
            // 追赶最多八步，丢弃完整多余步长而保留不足一步的余量；脚本与 Jolt 共用这个决定。
            const auto dropped = uint64_t(requestedSteps - time.fixedSteps);
            time.droppedSteps += dropped;
            time.droppedTime += dropped * fixedStep;
            time.accumulated = std::max(0.0, time.accumulated - requestedSteps * fixedStep);
            auto assetResults = std::move(context.assetResults), assetUnloads = std::move(context.assetUnloads);
            context.assetResults = nlohmann::json::array(); context.assetUnloads = nlohmann::json::array();
            Apply(*world, world->GetScriptContext()->Call("tick", {{"dt", time.delta}, {"time", ContextTime()}, {"input",
                {int(input.moveRight) - int(input.moveLeft), int(input.moveForward) - int(input.moveBackward),
                 int(input.moveUp) - int(input.moveDown)}}, {"deferLate", true},
                 {"inputMap", input.gameplay},
                 {"assetResults", assetResults}, {"assetUnloads", assetUnloads},
                 {"states", context.TakePhysicsUpdates()}, {"delta", true}}));
            // 先领取 Tick 的一次性冲量，查询留到该帧最后一个物理子步，避免使用旧镜头/刚体位姿。
            StepPhysics(*world, false, time.fixedSteps == 0);
            for (unsigned step = 0; step < time.fixedSteps; ++step)
            {
                ++time.fixedFrame;
                Apply(*world, context.Call("fixed", {{"dt", float(fixedStep)}, {"fixedFrameIndex", time.fixedFrame},
                    {"states", context.TakePhysicsUpdates()}, {"delta", true}}));
                StepPhysics(*world, true, step + 1 == time.fixedSteps);
            }
            auto rayHits = std::move(world->GetScriptContext()->rayHits);
            world->GetScriptContext()->rayHits = nlohmann::json::array();
            Apply(*world, world->GetScriptContext()->Call("late", {{"states", context.TakePhysicsUpdates()}, {"delta", true},
                {"contacts", physics->Contacts()}, {"triggers", physics->Triggers()}, {"rayHits", rayHits}}));
            StepPhysics(*world, 0);
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
        auto candidateContext = CreateContext(blueprints, templates, candidateGeneration);
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
    return {{"deltaTime", time.delta}, {"unscaledDeltaTime", time.unscaledDelta},
        {"timeScale", time.scale}, {"timeSinceStartup", time.startup}, {"timeSincePlay", time.play},
        {"unscaledTimeSincePlay", time.unscaledPlay}, {"frameIndex", time.frame},
        {"fixedFrameIndex", time.fixedFrame}, {"fixedSteps", time.fixedSteps},
        {"droppedSteps", time.droppedSteps}, {"droppedTime", time.droppedTime},
        {"accumulator", time.accumulated}};
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

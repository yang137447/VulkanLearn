#include "engine/script/engineScriptBuild.h"

#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "engine/blueprintAssetLoadSession.h"
#include "engine/subsystemCollection.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "renderGraph.h"
#include "renderSystem.h"
#include "script/scriptBuildDiagnostics.h"
#include "script/scriptBuildService.h"
#include "script/scriptRuntime.h"
#include "world/world.h"

namespace VL
{

void TickEngineScriptBuild(
    const EngineScriptBuildContext& context,
    float deltaTime)
{
    if (!context.scriptBuild) return;
    SubsystemCollection& subsystems = *context.subsystems;
    ScriptBuildService* const& scriptBuild = context.scriptBuild;
    bool& uiViewModelScriptBuildPending =
        *context.uiViewModelScriptBuildPending;
    std::string& scriptBuildOutput = *context.scriptBuildOutput;
    std::optional<std::filesystem::path>& pendingBuiltAssembly =
        *context.pendingBuiltAssembly;
    uint64_t& pendingBuildWorldGeneration =
        *context.pendingBuildWorldGeneration;
    bool& autoCompileScripts = *context.autoCompileScripts;
    auto& scripts = subsystems.GetScriptRuntime();
    const auto& world = subsystems.GetWorldManager().GetActiveWorld();
    bool newCandidate = false;
    if (auto result = scriptBuild->Poll())
    {
        // 编译结果会改变编辑器脚本页的输出/状态文本：绕过 10 Hz 节拍立即重建。
        uiViewModelScriptBuildPending = true;
        scriptBuildOutput = std::move(result->log);
        auto& diagnostics = subsystems.GetDiagnosticsSubsystem();
        for (const auto& entry : ParseScriptBuildDiagnostics(scriptBuildOutput))
        {
            if (entry.severity == LogSeverity::Error)
                diagnostics.ReportError(entry.message, "C# Compiler", entry.location);
            else if (entry.severity == LogSeverity::Warning)
                diagnostics.ReportWarning(entry.message, "C# Compiler", entry.location);
            else diagnostics.ReportInfo(entry.message, "C# Compiler", entry.location);
        }
        if (result->succeeded)
        {
            pendingBuiltAssembly = std::move(result->assembly);
            pendingBuildWorldGeneration = result->worldGeneration;
            newCandidate = true;
            scripts.SetFeedback("Gameplay compiled; validating reload candidate");
        }
        else scripts.SetFeedback(result->cancelled ? "Gameplay build cancelled" : "Gameplay build failed; see Log");
    }
    if (!world) return;
    if (pendingBuiltAssembly && world->GetGeneration() != pendingBuildWorldGeneration)
    {
        pendingBuiltAssembly.reset();
        scripts.SetFeedback("Compiled candidate discarded because the World changed");
    }
    const bool level = !RenderSystem::GetInstance().IsOfflineMode() &&
        std::filesystem::path(world->GetScenePath()).filename() != "SC_model_preview.json";
    if (pendingBuiltAssembly && level && (newCandidate || scripts.IsEditing()))
    {
        // 构建完成不等于加载成功；原有候选类/属性/OnCreate事务仍是唯一程序集发布入口。
        std::string error;
        const bool reloaded = scripts.ReloadAssembly(*world, error, pendingBuiltAssembly->string());
        // 在途 lease 可暂时阻止迁移：保留候选供 Reload 或 Stop 后重试，不每帧重复执行钩子。
        if (reloaded || scripts.IsEditing()) pendingBuiltAssembly.reset();
        auto& diagnostics = subsystems.GetDiagnosticsSubsystem();
        if (reloaded) diagnostics.ReportInfo("Gameplay assembly reloaded", "Script");
        else diagnostics.ReportWarning("Reload rejected: " + error, "Script");
    }
    if (autoCompileScripts && level)
    {
        try
        {
            if (scriptBuild->ObserveChanges(deltaTime))
            {
                // 工作从 idle 进入 running：Busy 状态是可见回馈，立即刷新一次。
                uiViewModelScriptBuildPending = true;
                std::string error;
                if (!scriptBuild->Request(world->GetGeneration(), error)) scripts.SetFeedback(error);
                else scripts.SetFeedback("Source changed; compiling Gameplay...");
            }
        }
        catch (const std::exception& exception) { scripts.SetFeedback(exception.what()); }
    }
}

void TickEngineBlueprintAssets(
    const EngineScriptBuildContext& buildContext)
{
    SubsystemCollection& subsystems = *buildContext.subsystems;
    std::unique_ptr<BlueprintAssetLoadSession>& blueprintAssetLoad =
        *buildContext.blueprintAssetLoad;
    nlohmann::json& blueprintAssetRequests = *buildContext.blueprintAssetRequests;
    nlohmann::json& activeBlueprintAssetRequests =
        *buildContext.activeBlueprintAssetRequests;
    std::map<std::string,
             std::unique_ptr<BlueprintAssetLoadSession>>&
        loadedBlueprintAssets = *buildContext.loadedBlueprintAssets;
    std::weak_ptr<ScriptWorldContext>& blueprintAssetContext =
        *buildContext.blueprintAssetContext;
    uint64_t& blueprintAssetWorldGeneration =
        *buildContext.blueprintAssetWorldGeneration;
    RendererBackendVulkan* const& rendererBackend = buildContext.rendererBackend;
    PipelineFactory* const& pipelineFactory = buildContext.pipelineFactory;
    auto& scripts = subsystems.GetScriptRuntime();
    const auto& world = subsystems.GetWorldManager().GetActiveWorld();
    const auto context = world ? world->GetScriptContext() : nullptr;
    const uint64_t generation = world ? world->GetGeneration() : 0;
    auto& diagnostics = subsystems.GetDiagnosticsSubsystem();
    if (generation != blueprintAssetWorldGeneration || context != blueprintAssetContext.lock() || scripts.IsEditing())
    {
        if (blueprintAssetLoad) blueprintAssetLoad->Cancel();
        blueprintAssetLoad.reset();
        blueprintAssetRequests.clear();
        activeBlueprintAssetRequests.clear();
        for (auto& item : loadedBlueprintAssets)
            if (item.second && world)
            {
                item.second->Release(*world);
                scripts.ForgetBlueprintAsset(*world, item.first);
            }
        loadedBlueprintAssets.clear();
        blueprintAssetContext = context;
        blueprintAssetWorldGeneration = generation;
    }
    if (!world) return;
    for (const auto& request : scripts.TakeAssetRequests(*world))
    {
        if (blueprintAssetLoad && blueprintAssetLoad->Path() == request.at("path"))
            activeBlueprintAssetRequests.push_back(request);
        else blueprintAssetRequests.push_back(request);
    }
    for (const auto& cancellation : scripts.TakeAssetCancellations(*world))
    {
        for (auto* requests : {&blueprintAssetRequests, &activeBlueprintAssetRequests})
            for (auto it = requests->begin(); it != requests->end();)
                if (it->at("id") == cancellation) it = requests->erase(it); else ++it;
    }
    if (blueprintAssetLoad && activeBlueprintAssetRequests.empty()) blueprintAssetLoad->Cancel();
    if (blueprintAssetLoad && blueprintAssetLoad->Step(*world))
    {
        const auto path = blueprintAssetLoad->Path();
        const bool success = blueprintAssetLoad->Error().empty();
        bool accepted = false;
        for (const auto& request : activeBlueprintAssetRequests)
            accepted = scripts.CompleteAssetLoad(*world, request.at("id").get<uint64_t>(), path,
                blueprintAssetLoad->Template(), blueprintAssetLoad->Error()) || accepted;
        if (success && accepted)
        {
            // entry在Begin前已分配；提交之后只移动会话owner，避免成功资源失去卸载入口。
            loadedBlueprintAssets.at(path) = std::move(blueprintAssetLoad);
            diagnostics.ReportInfo("Blueprint asset ready: " + path);
        }
        else
        {
            if (success) blueprintAssetLoad->Release(*world);
            else if (accepted) diagnostics.ReportWarning("Blueprint asset load failed: " + blueprintAssetLoad->Error());
            blueprintAssetLoad.reset();
            loadedBlueprintAssets.erase(path);
        }
        activeBlueprintAssetRequests.clear();
    }
    for (const auto& path : scripts.CollectUnusedBlueprintAssets(*world))
    {
        const auto session = loadedBlueprintAssets.find(path);
        if (session != loadedBlueprintAssets.end() && session->second)
        {
            session->second->Release(*world);
            scripts.ForgetBlueprintAsset(*world, path);
            loadedBlueprintAssets.erase(session);
            diagnostics.ReportInfo("Blueprint asset released: " + path);
        }
    }
    if (!blueprintAssetLoad && !blueprintAssetRequests.empty() && !scripts.IsEditing())
    {
        const auto path = blueprintAssetRequests.front().at("path").get<std::string>();
        for (auto it = blueprintAssetRequests.begin(); it != blueprintAssetRequests.end();)
            if (it->at("path") == path)
            { activeBlueprintAssetRequests.push_back(*it); it = blueprintAssetRequests.erase(it); }
            else ++it;
        if (world->GetScriptTemplates().contains(path))
        {
            for (const auto& request : activeBlueprintAssetRequests)
                scripts.CompleteAssetLoad(*world, request.at("id").get<uint64_t>(), path,
                    world->GetScriptTemplates().at(path));
            activeBlueprintAssetRequests.clear();
        }
        else
        {
            loadedBlueprintAssets.try_emplace(path);
            blueprintAssetLoad = std::make_unique<BlueprintAssetLoadSession>(
                *rendererBackend, *pipelineFactory, RenderGraph::GetInstance());
            try
            {
                blueprintAssetLoad->Begin(*world, activeBlueprintAssetRequests.front().at("id").get<uint64_t>(),
                    path, scripts.Classes());
            }
            catch (const std::exception& exception)
            {
                for (const auto& request : activeBlueprintAssetRequests)
                    scripts.CompleteAssetLoad(*world, request.at("id").get<uint64_t>(), path, nullptr, exception.what());
                blueprintAssetLoad.reset();
                loadedBlueprintAssets.erase(path);
                activeBlueprintAssetRequests.clear();
            }
        }
    }
}

} // namespace VL

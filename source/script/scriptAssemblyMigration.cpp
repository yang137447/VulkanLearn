#include "script/scriptRuntime.h"

#include <filesystem>
#include <map>
#include "script/scriptCompleteDeclaration.h"
#include "script/scriptLogicComponents.h"
#include "world/world.h"

namespace VL
{
namespace
{
using Json = nlohmann::json;

bool SameRuntimeValue(const Json& before, const Json& after)
{
    // 原生与 CLR 均以 float 执行 TRS/灯光；JSON 两端的最短十进制写法可能不同。
    if (before.is_number() && after.is_number()) return before.get<float>() == after.get<float>();
    if (before.is_array() && after.is_array())
    {
        if (before.size() != after.size()) return false;
        for (size_t index = 0; index < before.size(); ++index)
            if (!SameRuntimeValue(before[index], after[index])) return false;
        return true;
    }
    if (before.is_object() && after.is_object())
    {
        if (before.size() != after.size()) return false;
        for (const auto& item : before.items())
            if (!after.contains(item.key()) || !SameRuntimeValue(item.value(), after.at(item.key()))) return false;
        return true;
    }
    return before == after;
}

Json MigrationLogicDeclarations(const Json& current, const Json& previous, const Json& candidate)
{
    auto result = Json::array();
    for (const auto& state : current.value("scriptComponents", Json::array()))
    {
        const auto id = state.at("id");
        bool authored = false;
        for (const auto& node : previous.value("scriptComponents", Json::array()))
            if (node.at("id") == id) authored = true;
        Json declaration;
        for (const auto& node : candidate.value("scriptComponents", Json::array()))
            if (node.at("id") == id) declaration = node;
        if (!authored)
            declaration = {{"id", id}, {"class", state.at("class")}, {"enabled", state.at("enabled")},
                {"properties", state.at("authoredProperties")}};
        if (declaration.is_null())
            throw std::runtime_error("Live reload removed an authored script component");
        // properties 必须从新作者配置重建；运行时值只能经过显式 [Migrate] 恢复。
        result.push_back(std::move(declaration));
    }
    return result;
}

void ValidateMigrationValues(const Json& before, const Json& after)
{
    std::map<std::string, const Json*> states;
    for (const auto& value : before) states.emplace(value.at("id").get<std::string>(), &value);
    if (states.size() != after.size()) throw std::runtime_error("OnReload cannot change World structure");
    for (const auto& value : after)
    {
        const auto found = states.find(value.at("id").get<std::string>());
        if (found == states.end()) throw std::runtime_error("OnReload cannot change Blueprint identity");
        for (const char* field : {"position", "rotation", "scale", "destroyed", "enabled", "failed", "components"})
            if (!SameRuntimeValue(value.at(field), found->second->at(field)))
                throw std::runtime_error(std::string("OnReload cannot change runtime World values: ") + field);
        for (const char* field : {"velocity", "impulse"})
            if (value.contains(field)) throw std::runtime_error("OnReload cannot enqueue physics commands");
        for (const char* field : {"bodyVelocity", "walkVelocity", "grounded"})
            if (value.contains(field) && !SameRuntimeValue(value.at(field), found->second->value(field, value.at(field))))
                throw std::runtime_error("OnReload cannot change physics state");
        const auto& oldComponents = found->second->at("scriptComponents");
        const auto& newComponents = value.at("scriptComponents");
        if (oldComponents.size() != newComponents.size())
            throw std::runtime_error("OnReload cannot change script component structure");
        for (size_t index = 0; index < oldComponents.size(); ++index)
            for (const char* field : {"id", "class", "enabled", "failed", "started"})
                if (oldComponents.at(index).at(field) != newComponents.at(index).at(field))
                    throw std::runtime_error("OnReload cannot change script component state");
        if (value.value("jump", false) || !value.value("raycasts", Json::array()).empty() ||
            !value.value("assetRequests", Json::array()).empty() ||
            !value.value("assetCancels", Json::array()).empty() ||
            !value.value("debugCommands", Json::array()).empty())
            throw std::runtime_error("OnReload can register timers/events but cannot issue physics or asset requests");
    }
}
}

bool ScriptRuntimeSubsystem::ReloadPlayingAssembly(World& world, std::string& error,
    const std::string& candidateAssembly)
{
    uint64_t candidateGeneration = 0;
    const char* phase = "preflight";
    try
    {
        const auto previousContext = world.GetScriptContext();
        if (!host || !previousContext) throw std::runtime_error("No live C# context to migrate");
        if (!world.ScriptAnimationFeedback().empty())
            throw std::runtime_error("Stop gameplay before reloading skeletal animation controllers");
        if (std::filesystem::path(world.GetScenePath()).filename() == "SC_model_preview.json")
            throw std::runtime_error("Asset preview does not run gameplay");
        // 在途完成包含原上下文句柄，不能把旧代委托或 lease 偷换到新上下文。
        if (!previousContext->assetLoads.empty() || !previousContext->assetRequests.empty() ||
            !previousContext->assetCancellations.empty() || !previousContext->assetResults.empty() ||
            !previousContext->assetUnloads.empty() || !previousContext->raycasts.empty() || !previousContext->rayHits.empty())
            throw std::runtime_error("Finish or cancel asset/physics requests and release asset leases before live reload");
        auto nextAssemblyPath = candidateAssembly.empty() ? assemblyPath : candidateAssembly;
        phase = "assembly load";
        auto loaded = host->Invoke({{"op", "initialize"}, {"assembly", nextAssemblyPath}});
        candidateGeneration = loaded.at("result").at("generation");
        auto candidateClasses = loaded.at("result").at("classes");
        auto candidateComponentClasses = loaded.at("result").at("componentClasses");
        auto blueprints = Json::array();
        auto templates = Json::object();
        phase = "author configuration";
        for (const auto& source : world.GetScriptResolvedBlueprints())
        {
            auto declaration = CompleteBlueprintDeclaration(source, candidateClasses, candidateComponentClasses);
            if (declaration.contains("__templatePath"))
            {
                const auto path = declaration.at("__templatePath").get<std::string>();
                templates[path] = std::move(declaration);
            }
            else blueprints.push_back(std::move(declaration));
        }
        for (const auto& entry : world.GetScriptTemplates().items())
            if (!templates.contains(entry.key()))
                templates[entry.key()] = CompleteBlueprintDeclaration(entry.value(),
                    candidateClasses, candidateComponentClasses);
        // 未生成的模板也必须先验证，避免成功换代后才发现 Spawn 类已损坏。
        for (const auto& entry : templates.items())
            CreateContext(Json::array({entry.value()}), Json::object(), candidateGeneration);
        phase = "state capture";
        auto snapshot = previousContext->Call("migration_capture").at("result");
        snapshot["states"] = world.GetScriptBlueprintStates();
        auto runtimeBlueprints = Json::array();
        for (const auto& current : world.GetScriptBlueprintStates())
        {
            const auto id = current.at("id").get<std::string>();
            auto declaration = previousContext->declarations.at(id);
            const auto previousDeclaration = declaration;
            const auto physicsTemplateId = declaration.value("__physicsTemplateId", std::string());
            bool found = false;
            for (const auto& authored : blueprints)
                if (authored.at("id") == id) { declaration = authored; found = true; break; }
            if (!found)
            {
                const auto path = declaration.value("__spawnPath", std::string());
                if (path.empty() || !templates.contains(path)) throw std::runtime_error("Live reload lost a Spawn template");
                declaration = templates.at(path);
                declaration["id"] = id;
                declaration["__spawnPath"] = path;
                if (!physicsTemplateId.empty()) declaration["__physicsTemplateId"] = physicsTemplateId;
            }
            declaration["scriptComponents"] = MigrationLogicDeclarations(current, previousDeclaration, declaration);
            for (const char* field : {"position", "rotation", "scale", "components"})
                if (current.contains(field)) declaration[field] = current.at(field);
            declaration["id"] = id;
            runtimeBlueprints.push_back(std::move(declaration));
        }
        phase = "candidate construction";
        auto candidateContext = CreateContext(runtimeBlueprints, templates, candidateGeneration,
            world.ScriptAnimationCatalog(), world.ScriptAnimationAssets());
        candidateContext->Call("set_time", {{"time", ContextTime()}});
        phase = "state restoration";
        const auto restored = candidateContext->Call("migration_restore", {{"snapshot", snapshot}});
        auto response = candidateContext->Call("migration_export");
        phase = "World comparison";
        ValidateMigrationValues(world.GetScriptBlueprintStates(), response.at("result"));
        // 导出已建立新托管代的增量基线，原生快照必须同时接纳它；无帧回调的组件不会再自行补发。
        // 先准备完整值，提交仅 swap，Pause 下也立即反映新默认值与组件代际。
        auto candidateStates = world.GetScriptBlueprintStates();
        std::map<std::string, const Json*> exportedStates;
        for (const auto& current : response.at("result"))
            exportedStates.emplace(current.at("id").get<std::string>(), &current);
        for (auto& current : candidateStates)
        {
            const auto id = current.at("id").get<std::string>();
            const auto& declaration = candidateContext->declarations.at(id);
            for (const char* field : {"class", "name", "properties", "physics", "__spawnPath"})
                if (declaration.contains(field)) current[field] = declaration.at(field);
            current["scriptComponents"] = exportedStates.at(id)->at("scriptComponents");
            current["hud"] = exportedStates.at(id)->at("hud");
            // 已逐 float 比对的空间值保留原生表示，避免 JSON 最短十进制往返改变 Jolt 的精确位姿。
        }
        candidateContext->permanentAssetPaths = previousContext->permanentAssetPaths;
        candidateContext->dynamicAssetPaths = previousContext->dynamicAssetPaths;
        // 一切可失败的恢复与比对均已完成；这里只交换脚本 owner，物理、World 和 GPU 保持身份。
        std::string message = "C# assembly reloaded with explicit state transfer";
        world.SwapScriptConfiguration(blueprints, templates, candidateContext);
        world.ScriptBlueprintStatesForUpdate().swap(candidateStates);
        for (const auto& current : world.GetScriptBlueprintStates())
            world.GetDebugDraw().ClearOwner("Script/" + current.at("id").get<std::string>());
        classes.swap(candidateClasses);
        componentClasses.swap(candidateComponentClasses);
        const auto previousGeneration = assemblyGeneration;
        assemblyGeneration = candidateGeneration;
        assemblyPath.swap(nextAssemblyPath);
        candidateGeneration = 0;
        if (candidateContext) candidateContext->Release(0);
        candidateContext.reset();
        try { host->Invoke({{"op", "unload"}, {"generation", previousGeneration}}); }
        catch (...) { /* 已提交换代，旧代清理不能反向改变成功结果。 */ }
        feedback.swap(message);
        error.clear();
        // 成功后的只读诊断失败不反转已提交的 owner 交换；Pause 下也尽量立即显示新代。
        try { CollectDiagnostics(restored, &world); RefreshPerformance(world); } catch (...) {}
        return true;
    }
    catch (const std::exception& exception)
    {
        if (candidateGeneration)
            try { host->Invoke({{"op", "unload"}, {"generation", candidateGeneration}}); } catch (...) {}
        feedback = error = std::string("Live reload ") + phase + ": " + exception.what();
        return false;
    }
}
} // namespace VL

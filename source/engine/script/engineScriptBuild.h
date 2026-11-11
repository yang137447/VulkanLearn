#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

class PipelineFactory;

namespace VL
{

class BlueprintAssetLoadSession;
class RendererBackendVulkan;
class ScriptBuildService;
class ScriptWorldContext;
class SubsystemCollection;

// EngineLoop 的脚本构建/蓝图资产轮询状态视图。所有字段都是 EngineLoop 现有
// 成员的指针；模块不持有第二份权威状态，也不缓存 World、脚本或渲染对象。
struct EngineScriptBuildContext
{
    SubsystemCollection* subsystems = nullptr;
    ScriptBuildService* scriptBuild = nullptr;
    RendererBackendVulkan* rendererBackend = nullptr;
    PipelineFactory* pipelineFactory = nullptr;

    bool* autoCompileScripts = nullptr;
    bool* uiViewModelScriptBuildPending = nullptr;
    std::string* scriptBuildOutput = nullptr;
    std::optional<std::filesystem::path>* pendingBuiltAssembly = nullptr;
    uint64_t* pendingBuildWorldGeneration = nullptr;

    std::unique_ptr<BlueprintAssetLoadSession>* blueprintAssetLoad = nullptr;
    nlohmann::json* blueprintAssetRequests = nullptr;
    nlohmann::json* activeBlueprintAssetRequests = nullptr;
    std::map<std::string, std::unique_ptr<BlueprintAssetLoadSession>>*
        loadedBlueprintAssets = nullptr;
    std::weak_ptr<ScriptWorldContext>* blueprintAssetContext = nullptr;
    uint64_t* blueprintAssetWorldGeneration = nullptr;
};

// 轮询脚本构建结果、发布/丢弃候选程序集，并按 --auto-compile 触源码重编译。
void TickEngineScriptBuild(
    const EngineScriptBuildContext& context,
    float deltaTime);

// 轮询蓝图资产的请求/取消/加载完成，并释放不再被引用的资产。
void TickEngineBlueprintAssets(
    const EngineScriptBuildContext& context);

} // namespace VL

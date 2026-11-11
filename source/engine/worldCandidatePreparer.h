#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "engine/preparedWorldTransition.h"

class RenderGraph;

namespace VL
{

class RendererResourceLoadCoordinator;
class ScriptRuntimeSubsystem;
class WorldManager;
struct MaterialDefinitionReloadBatch;

// Frame-visible renderer flags that must be frozen for one candidate build.
// The preparer receives them from its caller instead of reading RenderSystem.
struct WorldCandidatePrepareOptions
{
    bool offlineOnly = false;
    bool prerenderSkin = false;
    // 粒子资产文档随场景一起解析；指针只在本帧候选构建期间有效。
    const std::unordered_map<std::string, nlohmann::json>* particleDocuments =
        nullptr;
};

// Prepares an inactive World candidate from a scene path. Renderer resource
// loaders prepare GPU-facing data first; WorldBuilder then assembles the
// gameplay-facing World from the validated plan and cache. The returned
// candidate is unpublished: WorldGraphTransactionCoordinator owns commit.
class WorldCandidatePreparer
{
public:
    WorldCandidatePreparer(
        WorldManager& worldManager,
        RendererResourceLoadCoordinator& resourceLoadCoordinator,
        float initialCameraAspectRatio,
        uint32_t shadowCascadeCount);

    // `extraSceneObjects` 是并入本次加载的场景对象（JSON 对象数组，可为空）。
    // 模型编辑器用它把"当前打开的模型"放进固定的中性预览环境，而不必生成临时场景文件。
    // 放在末尾并给默认值，是为了让不关心注入的既有调用点保持原样。
    RuntimeResult<PreparedWorldTransition> Prepare(
        const std::string& scenePath,
        RenderGraph& pipelineContractGraph,
        const MaterialDefinitionReloadBatch* materialDefinitionReload = nullptr,
        const nlohmann::json& extraSceneObjects = nlohmann::json::array(),
        const nlohmann::json& sceneDocument = nullptr,
        WorldCandidatePrepareOptions options = {});
    void SetScriptRuntime(ScriptRuntimeSubsystem* scripts) { scriptRuntime = scripts; }

private:
    WorldManager& worldManager;
    RendererResourceLoadCoordinator& resourceLoadCoordinator;
    float initialCameraAspectRatio = 1.0f;
    uint32_t shadowCascadeCount = 4;
    ScriptRuntimeSubsystem* scriptRuntime = nullptr;
};

} // namespace VL

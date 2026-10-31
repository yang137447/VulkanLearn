#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "pipeline/pipelineFactory.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "world/worldManager.h"

class MaterialInstance;
class RenderGraph;

namespace VL
{

class RendererResourceLoadCoordinator;
class ScriptRuntimeSubsystem;
struct OfflineWorldAssets;
struct MaterialDefinitionReloadBatch;

enum class WorldTransitionState
{
    Idle,
    Validating,
    Loading,
    Active,
    Failed
};

struct WorldTransitionResult
{
    WorldHandle world;
    WorldTransitionState finalState = WorldTransitionState::Idle;
};

struct PreparedWorldTransition
{
    std::shared_ptr<World> world;
    PreparedWorldActivation activation;
    std::shared_ptr<RendererResourceCache> resourceCache;
    std::unordered_map<
        std::string,
        std::shared_ptr<MaterialInstance>>
        passMaterialBindings;
    std::vector<PendingGeneratedResourceFile> pendingGeneratedFiles;
    PipelineFactory::GraphicsCandidateState
        graphicsCandidateState;
    std::shared_ptr<OfflineWorldAssets> offlineWorldAssets;
    // 绝对输入路径与捕获摘要；在候选提交前复核，不能在活动 owner 交换后补验。
    std::vector<std::pair<std::string, std::string>> placementSourceInputs;
};

// Coordinates the world-load transaction boundary. Renderer resource loaders
// prepare GPU-facing data first; WorldBuilder then assembles the gameplay-facing
// World from the validated plan and cache.
class WorldTransitionCoordinator
{
public:
    WorldTransitionCoordinator(
        WorldManager& worldManager,
        RendererResourceLoadCoordinator& resourceLoadCoordinator,
        float initialCameraAspectRatio,
        uint32_t shadowCascadeCount);

    // `extraSceneObjects` 是并入本次加载的场景对象（JSON 对象数组，可为空）。
    // 模型编辑器用它把"当前打开的模型"放进固定的中性预览环境，而不必生成临时场景文件。
    // 放在末尾并给默认值，是为了让不关心注入的既有调用点保持原样。
    RuntimeResult<PreparedWorldTransition> PrepareWorldLoad(
        const std::string& scenePath,
        RenderGraph& pipelineContractGraph,
        const MaterialDefinitionReloadBatch* materialDefinitionReload = nullptr,
        const nlohmann::json& extraSceneObjects = nlohmann::json::array(),
        const nlohmann::json& sceneDocument = nullptr);
    void SetScriptRuntime(ScriptRuntimeSubsystem* scripts) { scriptRuntime = scripts; }

private:
    WorldManager& worldManager;
    RendererResourceLoadCoordinator& resourceLoadCoordinator;
    float initialCameraAspectRatio = 1.0f;
    uint32_t shadowCascadeCount = 4;
    WorldTransitionState state = WorldTransitionState::Idle;
    ScriptRuntimeSubsystem* scriptRuntime = nullptr;
};

} // namespace VL

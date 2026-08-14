#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "core/runtimeResult.h"
#include "pipeline/pipelineFactory.h"
#include "render/resource/rendererResourceCache.h"
#include "world/worldManager.h"

class MaterialInstance;
class RenderGraph;

namespace VL
{

class RendererResourceLoadCoordinator;
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
    PipelineFactory::GraphicsCandidateState
        graphicsCandidateState;
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
        float initialCameraAspectRatio);

    RuntimeResult<PreparedWorldTransition> PrepareWorldLoad(
        const std::string& scenePath,
        RenderGraph& pipelineContractGraph,
        const MaterialDefinitionReloadBatch* materialDefinitionReload = nullptr);

private:
    WorldManager& worldManager;
    RendererResourceLoadCoordinator& resourceLoadCoordinator;
    float initialCameraAspectRatio = 1.0f;
    WorldTransitionState state = WorldTransitionState::Idle;
};

} // namespace VL

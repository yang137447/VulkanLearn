#include "world/loading/worldTransitionCoordinator.h"

#include <exception>
#include <string>
#include <string_view>

#include "render/resource/rendererResourceLoadCoordinator.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/rendererResourceCache.h"
#include "renderGraph.h"
#include "sceneNode.h"
#include "world/loading/worldBuilder.h"
#include "world/loading/worldLoader.h"

namespace VL
{

namespace
{

bool ContainsText(std::string_view value, std::string_view token)
{
    return value.find(token) != std::string_view::npos;
}

std::string ClassifyRendererResourceLoadError(std::string_view message)
{
    if (ContainsText(message, "Texture") ||
        ContainsText(message, "texture") ||
        ContainsText(message, "T_*.json"))
    {
        return "Texture.LoadFailed";
    }
    if (ContainsText(message, "Material") ||
        ContainsText(message, "material") ||
        ContainsText(message, "parameter") ||
        ContainsText(message, "renderMode") ||
        ContainsText(message, "cullMode"))
    {
        return "Material.LoadFailed";
    }
    if (ContainsText(message, "Shader") ||
        ContainsText(message, "shader"))
    {
        return "Shader.LoadFailed";
    }
    if (ContainsText(message, "Mesh") ||
        ContainsText(message, "mesh") ||
        ContainsText(message, "model importer"))
    {
        return "Mesh.LoadFailed";
    }

    return "WorldTransition.LoadFailed";
}

void ConfigureInitialCamera(Camera& camera, float aspectRatio)
{
    // Current camera activation for the single-view runtime. The future
    // World/Controller path should own this policy before snapshots are built.
    camera.SetCamera(camera.GetPosition(), Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Vector3f(0.0f, 1.0f, 0.0f));
    camera.SetProjection(
        camera.GetHFOV(),
        aspectRatio,
        camera.GetClipNear(),
        camera.GetClipFar());
}

} // namespace

WorldTransitionCoordinator::WorldTransitionCoordinator(
    WorldManager& worldManager,
    RendererResourceLoadCoordinator& resourceLoadCoordinator,
    float initialCameraAspectRatio)
    : worldManager(worldManager)
    , resourceLoadCoordinator(resourceLoadCoordinator)
    , initialCameraAspectRatio(initialCameraAspectRatio)
{
}

RuntimeResult<PreparedWorldTransition>
WorldTransitionCoordinator::PrepareWorldLoad(
    const std::string& scenePath,
    RenderGraph& pipelineContractGraph,
    const MaterialDefinitionReloadBatch* materialDefinitionReload)
{
    if (scenePath.empty())
    {
        state = WorldTransitionState::Failed;
        return RuntimeResult<PreparedWorldTransition>::Failure(
            MakeRuntimeError(
                "WorldTransition.EmptyScenePath",
                "Cannot load a world from an empty scene path."));
    }

    state = WorldTransitionState::Validating;
    WorldLoader worldLoader;
    auto worldBuildPlanResult = worldLoader.Load(scenePath);
    if (worldBuildPlanResult.IsFailure())
    {
        state = WorldTransitionState::Failed;
        return RuntimeResult<PreparedWorldTransition>::Failure(
            worldBuildPlanResult.Error());
    }
    WorldBuildPlan worldBuildPlan =
        std::move(worldBuildPlanResult.Value());

    try
    {
        state = WorldTransitionState::Loading;
        const uint64_t worldGeneration =
            worldManager.GetNextWorldGeneration();
        RendererResourceCache& activeCache =
            RendererResourceCache::GetInstance();

        PreparedWorldTransition prepared;
        prepared.resourceCache =
            std::make_shared<RendererResourceCache>(
                activeCache.BeginCandidate(worldGeneration));
        RendererResourceLoadContext loadContext{
            *prepared.resourceCache,
            pipelineContractGraph};
        loadContext.graphicsCandidateState =
            &prepared.graphicsCandidateState;
        loadContext.previousWorldResources =
            activeCache.CaptureActiveWorldLocalResources();
        loadContext.materialDefinitionReload =
            materialDefinitionReload;
        loadContext.passMaterialBindings =
            &prepared.passMaterialBindings;

        RendererWorldResourceLoadResult resourceLoadResult =
            resourceLoadCoordinator.LoadRendererResources(
                worldBuildPlan,
                loadContext);
        worldBuildPlan.meshObjectPlans =
            std::move(resourceLoadResult.meshObjectPlans);
        worldBuildPlan.speedTreeWindProfiles =
            std::move(resourceLoadResult.speedTreeWindProfiles);

        WorldBuilder worldBuilder;
        auto worldResult =
            worldBuilder.BuildFromLoadedScene(
                worldGeneration,
                worldBuildPlan,
                *prepared.resourceCache);
        if (worldResult.IsFailure())
        {
            state = WorldTransitionState::Failed;
            return RuntimeResult<PreparedWorldTransition>::Failure(
                worldResult.Error());
        }

        prepared.world = std::move(worldResult.Value());
        ConfigureInitialCamera(
            *prepared.world->GetCamera(),
            initialCameraAspectRatio);
        prepared.activation =
            worldManager.PrepareActivation(prepared.world);
        state = WorldTransitionState::Active;
        return RuntimeResult<PreparedWorldTransition>::Success(
            std::move(prepared));
    }
    catch (const std::exception& exception)
    {
        state = WorldTransitionState::Failed;
        const std::string errorMessage = exception.what();
        return RuntimeResult<PreparedWorldTransition>::Failure(
            MakeRuntimeError(
                ClassifyRendererResourceLoadError(errorMessage),
                errorMessage,
                scenePath));
    }
}

} // namespace VL

#include "world/loading/worldTransitionCoordinator.h"

#include <exception>
#include <string>
#include <string_view>

#include "render/resource/rendererResourceLoadCoordinator.h"
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

RuntimeResult<WorldTransitionResult> WorldTransitionCoordinator::LoadInitialWorld(
    const std::string& scenePath)
{
    if (worldManager.HasActiveWorld())
    {
        state = WorldTransitionState::Failed;
        return RuntimeResult<WorldTransitionResult>::Failure(MakeRuntimeError(
            "WorldTransition.InitialWorldAlreadyLoaded",
            "Cannot load an initial world because an active world already exists.",
            scenePath));
    }

    return LoadWorldThroughResourceCoordinator(scenePath);
}

RuntimeResult<WorldTransitionResult> WorldTransitionCoordinator::RequestWorldLoad(
    const std::string& scenePath)
{
    return LoadWorldThroughResourceCoordinator(scenePath);
}

RuntimeResult<WorldTransitionResult> WorldTransitionCoordinator::LoadWorldThroughResourceCoordinator(
    const std::string& scenePath)
{
    if (scenePath.empty())
    {
        state = WorldTransitionState::Failed;
        return RuntimeResult<WorldTransitionResult>::Failure(MakeRuntimeError(
            "WorldTransition.EmptyScenePath",
            "Cannot load a world from an empty scene path."));
    }

    state = WorldTransitionState::Validating;
    WorldLoader worldLoader;
    auto worldBuildPlanResult = worldLoader.Load(scenePath);
    if (worldBuildPlanResult.IsFailure())
    {
        state = WorldTransitionState::Failed;
        return RuntimeResult<WorldTransitionResult>::Failure(worldBuildPlanResult.Error());
    }
    WorldBuildPlan worldBuildPlan = std::move(worldBuildPlanResult.Value());

    RendererResourceCache& resourceCache = RendererResourceCache::GetInstance();
    RendererResourceCache::WorldLocalResourceSnapshot resourceSnapshot =
        resourceCache.CaptureWorldLocalResources();
    auto passMaterialSnapshot = RenderGraph::GetInstance().CapturePassMaterialInstances();

    try
    {
        state = WorldTransitionState::Loading;
        const uint64_t worldGeneration = worldManager.GetNextWorldGeneration();
        // Load renderer resources into the world-local cache before activating
        // the new World. If any later step fails, the snapshots above restore
        // the old active world's bindings.
        RendererWorldResourceLoadResult resourceLoadResult =
            resourceLoadCoordinator.LoadRendererResources(worldBuildPlan, worldGeneration);
        worldBuildPlan.meshObjectPlans = std::move(resourceLoadResult.meshObjectPlans);
        worldBuildPlan.speedTreeWindProfiles = std::move(resourceLoadResult.speedTreeWindProfiles);

        WorldBuilder worldBuilder;
        auto worldResult = worldBuilder.BuildFromLoadedScene(
            worldGeneration,
            worldBuildPlan,
            RendererResourceCache::GetInstance());
        if (worldResult.IsFailure())
        {
            resourceCache.RestoreWorldLocalResources(std::move(resourceSnapshot));
            RenderGraph::GetInstance().RestorePassMaterialInstances(passMaterialSnapshot);
            state = WorldTransitionState::Failed;
            return RuntimeResult<WorldTransitionResult>::Failure(worldResult.Error());
        }
        ConfigureInitialCamera(*worldResult.Value()->GetCamera(), initialCameraAspectRatio);

        WorldTransitionResult result;
        result.world = worldManager.ActivateLoadedWorld(worldResult.Value());
        state = WorldTransitionState::Active;
        result.finalState = state;
        return RuntimeResult<WorldTransitionResult>::Success(result);
    }
    catch (const std::exception& exception)
    {
        resourceCache.RestoreWorldLocalResources(std::move(resourceSnapshot));
        RenderGraph::GetInstance().RestorePassMaterialInstances(passMaterialSnapshot);
        state = WorldTransitionState::Failed;
        const std::string errorMessage = exception.what();
        return RuntimeResult<WorldTransitionResult>::Failure(MakeRuntimeError(
            ClassifyRendererResourceLoadError(errorMessage),
            errorMessage,
            scenePath));
    }
}

} // namespace VL

#include "world/loading/worldTransitionCoordinator.h"

#include <exception>

#include "render/resource/rendererResourceLoadCoordinator.h"
#include "render/resource/rendererResourceCache.h"
#include "renderGraph.h"
#include "sceneObject.h"
#include "world/loading/worldBuilder.h"
#include "world/loading/worldLoader.h"

namespace VL
{

namespace
{

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
    auto worldBuildPlan = worldLoader.Load(scenePath);
    if (worldBuildPlan.IsFailure())
    {
        state = WorldTransitionState::Failed;
        return RuntimeResult<WorldTransitionResult>::Failure(worldBuildPlan.Error());
    }

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
        resourceLoadCoordinator.LoadRendererResources(worldBuildPlan.Value(), worldGeneration);

        WorldBuilder worldBuilder;
        auto worldResult = worldBuilder.BuildFromLoadedScene(
            worldGeneration,
            worldBuildPlan.Value(),
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
        return RuntimeResult<WorldTransitionResult>::Failure(MakeRuntimeError(
            "WorldTransition.LoadFailed",
            exception.what(),
            scenePath));
    }
}

} // namespace VL

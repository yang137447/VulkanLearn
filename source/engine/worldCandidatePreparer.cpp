#include "engine/worldCandidatePreparer.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "commonFunction.h"
#include "render/resource/rendererResourceLoadCoordinator.h"
#include "render/offline/offlineWorldAssets.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/rendererResourceCache.h"
#include "renderGraph.h"
#include "sceneNode.h"
#include "world/loading/worldBuilder.h"
#include "world/loading/worldLoader.h"
#include "world/worldBuildPlan.h"
#include "script/scriptRuntime.h"

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
    if (ContainsText(message, "Hair") ||
        ContainsText(message, "hair"))
    {
        return "Hair.LoadFailed";
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
    // 旧场景没有显式 look_at，继续保留“启动时看向世界原点”的兼容行为；
    // 新验证场景可以冻结作者机位，避免启动后再依赖交互命令修正视线。
    if (!camera.HasInitialLookAt())
    {
        camera.SetCamera(
            camera.GetPosition(),
            Eigen::Vector3f(0.0f, 0.0f, 0.0f),
            Eigen::Vector3f(0.0f, 1.0f, 0.0f));
    }
    camera.SetProjection(
        camera.GetHFOV(),
        aspectRatio,
        camera.GetClipNear(),
        camera.GetClipFar());
}

} // namespace

WorldCandidatePreparer::WorldCandidatePreparer(
    WorldManager& worldManager,
    RendererResourceLoadCoordinator& resourceLoadCoordinator,
    float initialCameraAspectRatio,
    uint32_t shadowCascadeCount)
    : worldManager(worldManager)
    , resourceLoadCoordinator(resourceLoadCoordinator)
    , initialCameraAspectRatio(initialCameraAspectRatio)
    , shadowCascadeCount(shadowCascadeCount)
{
}

RuntimeResult<PreparedWorldTransition>
WorldCandidatePreparer::Prepare(
    const std::string& scenePath,
    RenderGraph& pipelineContractGraph,
    const MaterialDefinitionReloadBatch* materialDefinitionReload,
    const nlohmann::json& extraSceneObjects,
    const nlohmann::json& sceneDocument,
    WorldCandidatePrepareOptions options)
{
    if (scenePath.empty())
    {
        return RuntimeResult<PreparedWorldTransition>::Failure(
            MakeRuntimeError(
                "WorldTransition.EmptyScenePath",
                "Cannot load a world from an empty scene path."));
    }

    WorldLoader worldLoader;
    auto worldBuildPlanResult = worldLoader.Load(scenePath, extraSceneObjects, sceneDocument);
    if (worldBuildPlanResult.IsFailure())
    {
        return RuntimeResult<PreparedWorldTransition>::Failure(
            worldBuildPlanResult.Error());
    }
    WorldBuildPlan worldBuildPlan =
        std::move(worldBuildPlanResult.Value());

    try
    {
        const uint64_t worldGeneration =
            worldManager.GetNextWorldGeneration();
        RendererResourceCache& activeCache =
            RendererResourceCache::GetInstance();

        PreparedWorldTransition prepared;
        for (const auto& object : worldBuildPlan.sceneAssetPlan.objectPlans)
            if (object.meshLoadRequest && object.meshLoadRequest->tressfxResources)
                for (const auto& input : object.meshLoadRequest->tressfxResources->sourceInputs)
                    prepared.placementSourceInputs.emplace_back(
                        CommonFunction::Path(input.first), input.second);
        prepared.offlineWorldAssets = std::make_shared<OfflineWorldAssets>();
        prepared.offlineWorldAssets->worldGeneration = worldGeneration;
        prepared.offlineWorldAssets->scenePath = scenePath;
        prepared.offlineWorldAssets->sceneJson = worldBuildPlan.sceneJson;
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
        loadContext.offlineWorldAssets =
            prepared.offlineWorldAssets.get();
        loadContext.offlineOnly = options.offlineOnly;
        // 预蒙皮是启动级开关：候选 World 的材质变体与对象 buffer 必须与
        // RenderSystem 后续帧录制使用同一个值。
        loadContext.prerenderSkin = options.prerenderSkin;
        prepared.offlineWorldAssets->realtimePrepared = !loadContext.offlineOnly;

        WorldAssemblyAssets assemblyAssets =
            resourceLoadCoordinator.LoadRendererResources(
                worldBuildPlan,
                loadContext);
        // gameplay 计划已在 WorldLoader 中产出；这里只合并渲染器派生的
        // 网格/风计划与材质绑定。不得把 gameplay 参数写回加载计划。

        WorldBuilder worldBuilder;
        auto worldResult =
            worldBuilder.BuildFromLoadedScene(
                worldGeneration,
                worldBuildPlan,
                std::move(assemblyAssets),
                shadowCascadeCount);
        if (worldResult.IsFailure())
        {
            return RuntimeResult<PreparedWorldTransition>::Failure(
                worldResult.Error());
        }

        prepared.world = std::move(worldResult.Value());
        if (scriptRuntime) scriptRuntime->PrepareWorld(*prepared.world, worldBuildPlan.sceneJson,
            worldBuildPlan.authoredSceneJson);
        else
            for (const auto& object : worldBuildPlan.sceneJson.at("objects"))
                if (object.at("type") == "blueprintInstance")
                    throw std::runtime_error("Script.Unavailable: Actor scene has no script runtime");
        ConfigureInitialCamera(
            *prepared.world->GetCamera(),
            initialCameraAspectRatio);
        prepared.pendingGeneratedFiles =
            std::move(loadContext.pendingGeneratedFiles);
        prepared.activation =
            worldManager.PrepareActivation(prepared.world);
        return RuntimeResult<PreparedWorldTransition>::Success(
            std::move(prepared));
    }
    catch (const std::exception& exception)
    {
        const std::string errorMessage = exception.what();
        return RuntimeResult<PreparedWorldTransition>::Failure(
            MakeRuntimeError(
                ClassifyRendererResourceLoadError(errorMessage),
                errorMessage,
                scenePath));
    }
}

} // namespace VL

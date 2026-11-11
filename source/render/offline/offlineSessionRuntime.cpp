#include "renderSystem.h"

#include "core/runtimeResult.h"
#include "pipeline/pipelineFactory.h"
#include "render/offline/offlineDisplayExposure.h"
#include "render/offline/offlineGroomAsset.h"
#include "render/offline/offlineRenderCoordinator.h"
#include "render/offline/offlineSceneAdapter.h"
#include "render/rendergraph/preparedRenderGraphState.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/resourceRetireQueue.h"
#include "scene/sceneAssetTypes.h"
#include "renderGraph.h"
#include "world/worldSnapshotBuilder.h"

#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
bool RenderSystem::SetOfflineMode(bool enabled, std::string& message)
{
    if (enabled == offlineMode) return true;
    try
    {
        if (!activeWorld || !activeOfflineWorldAssets)
            throw std::runtime_error("No active frozen World");
        // Graph/cache/runtime-binding ownership changes while RT may still be
        // recording. Serialize the complete mode switch against RT.
        WaitForRendererMutationSafePoint();
        if (enabled)
        {
            VL::WorldSnapshotBuilder builder;
            VL::WorldSnapshotBuildDesc desc;
            desc.worldGeneration = activeWorld->GetGeneration();
            const auto snapshot = builder.Build(*activeWorld, desc);
            if (snapshot.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(snapshot.Error()));
            auto settings = offlineSettings;
            settings.width = GetSceneRenderExtent().width;
            settings.height = GetSceneRenderExtent().height;
            const auto candidate = VL::PreflightOfflineRenderInput(
                snapshot.Value(), *activeOfflineWorldAssets, settings);
            if (candidate.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(candidate.Error()));
            offlineMode = true;
            offlineCoordinator->Restart();
            offlineCameraMotionPolicy.Reset();
            message = "Offline mode";
            return true;
        }
        if (!activeWorld->GetSquareAreaLights().empty())
            throw std::runtime_error("Realtime does not support squareAreaLight");
        for (const auto& [path, definition] : activeOfflineWorldAssets->materialDefinitions)
            if (!definition.value("realtimeSupported", true))
                throw std::runtime_error("Realtime implementation unavailable: " + path);
        if (!activeOfflineWorldAssets->realtimePrepared)
        {
            // 在新图/缓存候选中准备全部 GPU 资源；失败不写 active cache/World。
            auto assets = std::make_shared<VL::OfflineWorldAssets>(*activeOfflineWorldAssets);
            auto graph = std::make_shared<VL::PreparedRenderGraphState>(*rendererBackend);
            graph->Load(RenderGraph::GetInstance().GetSourceJson());
            auto cache = VL::RendererResourceCache::GetInstance().BeginCandidate(
                activeWorld->GetGeneration());
            PipelineFactory::GraphicsCandidateState graphics;
            VL::RendererResourceLoadContext context{cache, graph->GetGraph()};
            context.graphicsCandidateState = &graphics;
            context.offlineWorldAssets = assets.get();
            VL::RendererMaterialLoader materialLoader(*pipelineFactory, *rendererBackend, context);
            // pass MI 按图合同复用；冻结的仅是场景 MI。相同 shadow MI 被四个 pass
            // 共用，逐 pass 重建会使前几个 Renderpass 的 weak_ptr 立即过期。
            materialLoader.LoadPassMaterials();
            context.useFrozenMaterials = true;
            VL::RendererMeshLoader meshLoader(*pipelineFactory, *rendererBackend, context);
            for (const auto& object : assets->sceneJson.at("objects"))
            {
                if (object.at("type") != "mesh") continue;
                const auto imported = assets->meshImports.at(object.at("modelPath").get<std::string>());
                MeshAssetLoadRequest request;
                request.buildPlan = imported->buildPlan;
                meshLoader.LoadMeshObject(object, request);
            }
            auto runtime = PrepareRuntimeBinding(activeWorld, cache, graph->GetGraph(), assets, false);
            auto pipeline = pipelineFactory->PrepareCandidateCommit(graphics);
            // runtime 持有同一个候选包；下面的 no-throw swap 会一并发布此能力位。
            // 准备失败时 activeOfflineWorldAssets 仍保留旧值和离线模式。
            assets->realtimePrepared = true;
            auto& retireQueue = VL::ResourceRetireQueue::GetInstance();
            auto retirements = retireQueue.PrepareRetirements({
                {"OfflineMode:Resources", activeWorld->GetGeneration(), retireQueue.GetLastSubmittedEpoch(), {}},
                {"OfflineMode:Graph", activeWorld->GetGeneration(), retireQueue.GetLastSubmittedEpoch(), {}},
                {"OfflineMode:LightBuffer", activeWorld->GetGeneration(), retireQueue.GetLastSubmittedEpoch(), {}}});
            pipelineFactory->CommitPreparedCandidate(std::move(pipeline));
            auto oldCache = VL::RendererResourceCache::GetInstance().CommitCandidate(std::move(cache));
            RenderGraph::GetInstance().SwapState(graph->GetGraph());
            auto oldLight = CommitPreparedRuntimeBinding(std::move(runtime));
            retirements.GetAdditionalResource(0).resource = std::static_pointer_cast<void>(oldCache);
            retirements.GetAdditionalResource(1).resource = std::static_pointer_cast<void>(graph);
            retirements.GetAdditionalResource(2).resource = std::move(oldLight);
            retireQueue.CommitPreparedRetirements(std::move(retirements));
        }
        offlineMode = false;
        offlineCoordinator->Stop();
        offlineCameraMotionPolicy.Reset();
        message = "Realtime mode";
        return true;
    }
    catch (const std::exception& exception)
    {
        message = exception.what();
        return false;
    }
}

bool RenderSystem::SetOfflineSetting(const std::string& name, int value, std::string& message)
{
    if ((name == "samples" && value > 0 && value <= 1000000) ||
        (name == "bounces" && value >= 0 && value <= 64) ||
        (name == "seed" && value >= 0) ||
        (name == "lightSamples" && value > 0 && value <= 64))
    {
        if (name == "samples")
        {
            if (value < offlineSettings.samples && offlineCoordinator)
                offlineCoordinator->Restart();
            offlineSettings.samples = value;
        }
        else if (name == "bounces") offlineSettings.bounces = value;
        else if (name == "seed") offlineSettings.seed = static_cast<uint32_t>(value);
        else offlineSettings.lightSamples = value;
        return true;
    }
    message = "Invalid offline setting: " + name;
    return false;
}

bool RenderSystem::SetOfflineExposure(float exposure, std::string& message)
{
    if (!std::isfinite(exposure) || exposure <= 0)
    {
        message = "Offline exposure must be finite and positive";
        return false;
    }
    offlineExposure = exposure;
    return true;
}

void RenderSystem::ControlOfflineSession(int command)
{
    if (!offlineCoordinator) return;
    if (command == 0) offlineCoordinator->Pause();
    else if (command == 1) offlineCoordinator->Resume();
    else if (command == 2) offlineCoordinator->Stop();
    else if (command == 3)
    {
        offlineCoordinator->Restart();
        offlineCameraMotionPolicy.Reset();
    }
}

VL::Offline::SessionState RenderSystem::GetOfflineState() const
{
    return offlineCoordinator ? offlineCoordinator->State() : VL::Offline::SessionState::Idle;
}

std::string RenderSystem::GetOfflineError() const
{
    return offlineCoordinator ? offlineCoordinator->Error() : std::string();
}

std::shared_ptr<const VL::OfflinePublishedResult> RenderSystem::CaptureOfflineResult() const
{
    return offlineCoordinator ? offlineCoordinator->Capture() : nullptr;
}

bool RenderSystem::ExportOffline(const VL::OfflinePublishedResult& result,
    const std::filesystem::path& directory, std::string& message) const
{
    try
    {
        VL::ExportOfflineResult(
            result,
            directory,
            VL::ResolveOfflineDisplayExposure(
                &result,
                offlineExposure,
                currentRenderScene.camera));
        message = "Offline output: " + directory.string();
        return true;
    }
    catch (const std::exception& exception)
    {
        message = exception.what();
        return false;
    }
}

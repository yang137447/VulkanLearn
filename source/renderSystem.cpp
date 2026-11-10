#include "renderSystem.h"
#include "render/preview/materialPreviewDraftParsing.h"
#include "render/preview/materialPreviewRuntime.h"
#include "render/offline/offlineDisplayExposure.h"
#include "passBarrier.h"
#include "render/offline/offlineWorldAssets.h"
#include "render/offline/offlineGroomAsset.h"
#include "shader/build/contentHash.h"
#include "shader/reload/shaderReloadCoordinator.h"
#include <cmath>
#include <limits>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include "core/runtimeResult.h"
#include "core/logStore.h"
#include "sceneNode.h"
#include "materialInstance.h"
#include "material.h"
#include "pipeline/pipelineBase.h"
#include "pipeline/pipelineFactory.h"
#include "commonFunction.h"
#include "renderGraph.h"
#include "hair/hairSimulationPass.h"
#include "render/skinning/skinDeformPass.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/placementBindingResolver.h"
#include "render/resource/rendererMaterialLoader.h"
#include "render/resource/rendererMeshLoader.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/resource/terrainPageBakeGpu.h"
#include "render/resource/terrainFeedbackGpu.h"
#include "render/resource/terrainLayerFeedbackGpu.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/rendergraph/preparedRenderGraphState.h"
#include "renderableObject.h"
#include "material/materialAssetUtils.h"
#include "material/loader/materialInstanceResolver.h"
#include "render/backend/rendererBackendVulkan.h"
#include "shaderReflect.h"
#include "profiler.h"
#include "ui/uiRenderSnapshot.h"
#include "vulkanDebug.h"
#include "world/world.h"
#include "scene/sceneAssetTypes.h"
#include "terrain/terrainPageManager.h"
#include "terrain/terrainWeightMap.h"
#include "terrain/terrainLayerTable.h"

namespace
{


void ApplyMaterialOverrideToRenderScene(
    VL::RenderScene& renderScene,
    const std::string& materialInstancePath,
    const std::string& materialKey)
{
    for (VL::RenderDrawPacket& drawPacket : renderScene.drawPackets)
    {
        if (drawPacket.materialInstance.key == materialInstancePath)
        {
            drawPacket.material = VL::ResourceHandle{materialKey, 0};
        }
    }
}

void RebuildRenderSceneMaterialGroupsImpl(VL::RenderScene& renderScene)
{
    renderScene.materialGroups.clear();
    for (size_t drawPacketIndex = 0;
         drawPacketIndex < renderScene.drawPackets.size();
         ++drawPacketIndex)
    {
        const VL::RenderDrawPacket& drawPacket =
            renderScene.drawPackets[drawPacketIndex];
        VL::MaterialDrawGroup* materialGroup = nullptr;
        for (VL::MaterialDrawGroup& candidateGroup :
             renderScene.materialGroups)
        {
            if (candidateGroup.material.key == drawPacket.material.key &&
                candidateGroup.material.generation ==
                    drawPacket.material.generation)
            {
                materialGroup = &candidateGroup;
                break;
            }
        }
        if (materialGroup == nullptr)
        {
            VL::MaterialDrawGroup newGroup;
            newGroup.material = drawPacket.material;
            renderScene.materialGroups.push_back(std::move(newGroup));
            materialGroup = &renderScene.materialGroups.back();
        }

        VL::MaterialInstanceDrawGroup* materialInstanceGroup = nullptr;
        for (VL::MaterialInstanceDrawGroup& candidateGroup :
             materialGroup->materialInstances)
        {
            if (candidateGroup.materialInstance.key ==
                    drawPacket.materialInstance.key &&
                candidateGroup.materialInstance.generation ==
                    drawPacket.materialInstance.generation)
            {
                materialInstanceGroup = &candidateGroup;
                break;
            }
        }
        if (materialInstanceGroup == nullptr)
        {
            VL::MaterialInstanceDrawGroup newGroup;
            newGroup.materialInstance = drawPacket.materialInstance;
            materialGroup->materialInstances.push_back(std::move(newGroup));
            materialInstanceGroup =
                &materialGroup->materialInstances.back();
        }
        materialInstanceGroup->drawPacketIndices.push_back(drawPacketIndex);
    }
}


} // namespace


RenderSystem::~RenderSystem()
{
    ShutdownRenderObject();
}

void RenderSystem::SetUiOverlayShaderPaths(std::string vertexPath, std::string fragmentPath)
{
    uiVertexShaderPath = std::move(vertexPath);
    uiFragmentShaderPath = std::move(fragmentPath);
}

void RenderSystem::SetCsmSettings(const VL::CsmSettings& settings)
{
    csmRuntime.SetSettings(settings);
}

VL::CsmSettings RenderSystem::GetCsmSettings() const
{
    return csmRuntime.GetSettings();
}

void RenderSystem::SnapshotFrameCsmSettings()
{
    csmRuntime.SnapshotFrameSettings();
}

void RenderSystem::SetRendererMutationSafePoint(std::function<void()> safePoint)
{
    std::lock_guard<std::mutex> lock(rendererMutationSafePointMutex);
    rendererMutationSafePoint = std::move(safePoint);
}

void RenderSystem::WaitForRendererMutationSafePoint()
{
    std::function<void()> safePoint;
    {
        std::lock_guard<std::mutex> lock(rendererMutationSafePointMutex);
        safePoint = rendererMutationSafePoint;
    }
    if (safePoint)
    {
        safePoint();
    }
}

bool RenderSystem::SetCsmCastShadows(bool enabled, std::string& outMessage)
{
    return csmRuntime.SetCastShadows(enabled, outMessage);
}

bool RenderSystem::SetCsmDynamicShadowDistance(
    float distance,
    std::string& outMessage)
{
    return csmRuntime.SetDynamicShadowDistance(distance, outMessage);
}

bool RenderSystem::SetCsmCascadeCount(
    uint32_t cascadeCount,
    std::string& outMessage)
{
    return csmRuntime.SetCascadeCount(cascadeCount, outMessage);
}

bool RenderSystem::SetCsmCascadeDistributionExponent(
    float exponent,
    std::string& outMessage)
{
    return csmRuntime.SetCascadeDistributionExponent(exponent, outMessage);
}

bool RenderSystem::SetCsmCascadeTransitionFraction(
    float fraction,
    std::string& outMessage)
{
    return csmRuntime.SetCascadeTransitionFraction(fraction, outMessage);
}

bool RenderSystem::SetCsmShadowDistanceFadeoutFraction(
    float fraction,
    std::string& outMessage)
{
    return csmRuntime.SetShadowDistanceFadeoutFraction(fraction, outMessage);
}

bool RenderSystem::SetCsmShadowBias(
    float value,
    std::string& outMessage)
{
    return csmRuntime.SetShadowBias(value, outMessage);
}

bool RenderSystem::SetCsmShadowSlopeBias(
    float value,
    std::string& outMessage)
{
    return csmRuntime.SetShadowSlopeBias(value, outMessage);
}

bool RenderSystem::SetCsmShadowCascadeBiasDistribution(
    float value,
    std::string& outMessage)
{
    return csmRuntime.SetShadowCascadeBiasDistribution(value, outMessage);
}

VL::EnvironmentUpdateDiagnostics RenderSystem::GetEnvironmentUpdateDiagnostics() const
{
    return environmentRuntime.GetDiagnostics();
}

ComputeShaderArtifact RenderSystem::GetActiveComputeShaderArtifact(
    const std::string& shaderName) const
{
    if (shaderName == exposureHistogramReloadParticipant.GetShaderName())
        return exposureHistogramReloadParticipant.GetActiveArtifact();
    if (shaderName == exposureAdaptReloadParticipant.GetShaderName())
        return exposureAdaptReloadParticipant.GetActiveArtifact();
    if (shaderName == "generator/skyToCubemap")
    {
        return proceduralSkyCubeGenerator.GetActiveArtifact();
    }
    if (shaderName == "generator/skySHGenerate")
    {
        return environmentIblBaker
            .GetActiveSkySHGenerateArtifact();
    }
    if (shaderName == "generator/prefilterEnvMap")
    {
        return environmentIblBaker
            .GetActivePrefilterEnvMapArtifact();
    }
    throw std::runtime_error(
        "No live compute reload participant for shader: " +
        shaderName);
}

void RenderSystem::SetActiveWorld(std::shared_ptr<const VL::World> world)
{
    // World 换代先释放旧 preview session，避免旧 MI shared_ptr 延长已退休
    // package 的生命周期；新的连接必须重新按 generation/path 捕获目标。
    materialInstancePreviewAdapter.Reset();
    activeWorld = std::move(world);
    // 显式清空/关闭 World 时，CPU 包也不再由 RenderSystem 留存。
    activeOfflineWorldAssets.reset();
    offlineCameraMotionPolicy.Reset();
    worldSnapshotQueue.Clear();
    worldSnapshotBuilder.Reset();
    currentRenderScene = VL::RenderScene();
    currentResolvedRenderScene = VL::ResolvedRenderScene();
    pickingRuntime.ResetPickingScene();
    hasMaterialInstancePreviewMaterialOverride = false;
    materialInstancePreviewOverridePath.clear();
    materialInstancePreviewOverrideMaterialKey.clear();
    hasRenderScene = false;
    ClearSelectedMaterialInstance();
    currentWindTimeSeconds = 0.0;
    windClockInitialized = false;
    windPendingDeltaSeconds.store(0.0f, std::memory_order_relaxed);
    windTickAccumulator = 0.0;
    sceneWindRuntime.Reset();
    initializedRenderWorldGeneration = 0;
    nextSnapshotFrameIndex = 0;
    simulationDeltaSeconds = 0.0f;
    gameSimulationDeltaSeconds = 0.0f;
    hairSimulationDeltaSeconds = 0.0f;
    // The cascade cache is RT-owned inside CsmRuntime. The next frame invalidates
    // it via the CSM revision or world-generation check; GT must not touch it here.
    if (activeWorld)
    {
        windResponseProfiles.Configure(activeWorld->GetWindResponseProfiles());
    }
    else
    {
        windResponseProfiles.Reset();
    }
}

bool RenderSystem::SetToneMappingMode(int mode, std::string& outMessage)
{
    return postProcessControls.SetToneMappingMode(mode, outMessage);
}

bool RenderSystem::SetBloomStrength(float value, std::string& outMessage)
{
    return postProcessControls.SetBloomStrength(value, outMessage);
}

bool RenderSystem::SetBloomThreshold(float value, std::string& outMessage)
{
    return postProcessControls.SetBloomThreshold(value, outMessage);
}

bool RenderSystem::SetBloomKnee(float value, std::string& outMessage)
{
    return postProcessControls.SetBloomKnee(value, outMessage);
}

bool RenderSystem::SetBloomClamp(float value, std::string& outMessage)
{
    return postProcessControls.SetBloomClamp(value, outMessage);
}

void RenderSystem::InitializeWorldTransactionResources()
{
    PROFILE_FUNCTION();
    RenderInitialize();
}

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

void RenderSystem::FinalizeInitialRenderObjectInitialization()
{
    PROFILE_FUNCTION();
    offlineDisplay.Initialize(*rendererBackend);
    offlineCoordinator = std::make_unique<VL::OfflineRenderCoordinator>();
    if (uiRenderSnapshotQueue != nullptr && !uiVertexShaderPath.empty() && !uiFragmentShaderPath.empty())
    {
        uiOverlayRenderer.Initialize(
            *rendererBackend,
            uiVertexShaderPath,
            uiFragmentShaderPath);
        ShaderVariantKey uiVariant;
        uiVariant.shaderName = "uiOverlay";
        uiOverlayRenderer.SetActiveShaderArtifact(
            pipelineFactory->PrepareGraphicsShaderVariant(
                uiVariant));
        if (shaderReloadCoordinator != nullptr)
        {
            shaderReloadCoordinator->SetUiOverlayParticipant(
                &uiOverlayRenderer);
        }
    }
}

void RenderSystem::Render()
{
    PROFILE_SCOPE("RenderSystem::Render");
    // Single-thread compatibility path: GT publishes the same immutable
    // snapshot that the optional RT consumes, then renders it immediately.
    // EngineLoop 的 Tick 为了给 CPU 阶段分别计时，直接调用下面两步而不是本函数；
    // 这里保留组合入口给需要一次性完成 publish+render 的调用方。
    PublishSnapshotFromActiveWorld();
    RenderLatestSnapshotOrLastGood();
}

void RenderSystem::RequestScreenshot(std::string path)
{
    std::lock_guard<std::mutex> lock(pendingScreenshotPathMutex);
    pendingScreenshotPath = std::move(path);
}

void RenderSystem::RequestTerrainPageBakeParity(int tileX, int tileZ)
{
    terrainFeedbackRuntime.RequestTerrainPageBakeParity(tileX, tileZ);
}

void RenderSystem::RunTerrainPageBakeParityIfRequested()
{
    terrainFeedbackRuntime.RunTerrainPageBakeParityIfRequested();
}

void RenderSystem::ConsumeTerrainFeedback(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming,
    uint64_t completedEpoch,
    uint32_t frameIndex)
{
    terrainFeedbackRuntime.ConsumeTerrainFeedback(
        streaming, completedEpoch, frameIndex);
}

void RenderSystem::PrepareTerrainFeedbackPass(uint8_t frameTag)
{
    terrainFeedbackRuntime.PrepareTerrainFeedbackPass(
        frameTag, GetSceneRenderExtent());
}

void RenderSystem::RequestTerrainResidentCapacity(uint32_t capacity)
{
    terrainFeedbackRuntime.RequestTerrainResidentCapacity(capacity);
}

void RenderSystem::RequestTerrainLayerResidentCapacity(uint32_t capacity)
{
    terrainFeedbackRuntime.RequestTerrainLayerResidentCapacity(capacity);
}

void RenderSystem::RequestTerrainLayerPreload()
{
    terrainFeedbackRuntime.RequestTerrainLayerPreload();
}

void RenderSystem::RequestTerrainLayerForceMip(int forceMip)
{
    terrainFeedbackRuntime.RequestTerrainLayerForceMip(forceMip);
}

void RenderSystem::ApplyTerrainLayerForceMip(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming)
{
    terrainFeedbackRuntime.ApplyTerrainLayerForceMip(streaming);
}

void RenderSystem::UpdateTerrainCameraPrefetch(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming)
{
    terrainFeedbackRuntime.UpdateTerrainCameraPrefetch(
        streaming,
        currentRenderScene.camera.position,
        currentRenderScene.camera.forward);
}

void RenderSystem::PublishTerrainStreamingStats(
    const std::shared_ptr<VL::TerrainStreamingState>& streaming,
    uint64_t completedEpoch)
{
    terrainFeedbackRuntime.PublishTerrainStreamingStats(
        streaming, completedEpoch);
}

void RenderSystem::ReportTerrainFeedbackStatistics() const
{
    terrainFeedbackRuntime.ReportTerrainFeedbackStatistics();
}

void RenderSystem::ReportTerrainStatistics() const
{
    VL::TerrainFeedbackRuntime::ReportTerrainStatistics();
}

void RenderSystem::ReportTerrainLayerStatistics() const
{
    VL::TerrainFeedbackRuntime::ReportTerrainLayerStatistics(
        GetSceneRenderExtent());
}

void RenderSystem::ReportTerrainLodStatistics() const
{
    VL::TerrainFeedbackRuntime::ReportTerrainLodStatistics(
        worldSnapshotBuilder.GetTerrainLodStats());
}

void RenderSystem::ReportTerrainChunkStatistics() const
{
    VL::TerrainFeedbackRuntime::ReportTerrainChunkStatistics(
        worldSnapshotBuilder.GetTerrainLodStats());
}

void RenderSystem::RequestHairTrace(std::string path, uint32_t frames,
    uint32_t guideStride)
{
    hairRuntime.RequestHairTrace(std::move(path), frames, guideStride);
}

void RenderSystem::RequestHairSimulationReset(std::string objectName)
{
    hairRuntime.RequestHairSimulationReset(std::move(objectName));
}

void RenderSystem::RequestHairSimulationParameter(
    const VL::Hair::HairLiveParameterRequest& request)
{
    hairRuntime.RequestHairSimulationParameter(request);
}

void RenderSystem::RequestTressfxBrushPreview(
    const VL::Hair::TressfxBrushPreviewRequest& request)
{
    hairRuntime.RequestTressfxBrushPreview(request);
}

void RenderSystem::RequestTressfxGroupBindingPreview(
    const VL::Hair::TressfxGroupBindingPreviewRequest& request)
{
    hairRuntime.RequestTressfxGroupBindingPreview(request);
}

void RenderSystem::ApplyTressfxGroupBindingPreviewRequests()
{
    hairRuntime.ApplyTressfxGroupBindingPreviewRequests(
        currentRenderScene, currentResolvedRenderScene);
}

void RenderSystem::ApplyTressfxBrushPreviewRequests(uint32_t swapchainImageIndex)
{
    hairRuntime.ApplyTressfxBrushPreviewRequests(
        currentRenderScene,
        currentResolvedRenderScene,
        swapchainImageIndex);
}


void RenderSystem::PrintGpuPassStats()
{
    // 控制台按需启用采集；Snapshot 只读最近一次读回的窗口，不等待渲染线程。
    gpuPassTimer.RequestCollection();
    const VL::GpuPassStatsSnapshot snapshot = gpuPassTimer.Snapshot();
    if (!snapshot.gpuTimingSupported)
    {
        std::cout << "[GPU] pass timing is unavailable on this device "
                     "(timestamp queries unsupported or timestampPeriod == 0)."
                  << std::endl;
        return;
    }
    if (!snapshot.collecting || snapshot.passes.empty())
    {
        // 首次启用采集时还没有可打印的表；让渲染线程在第一次读回后补打一张，
        // 这样单次 'gpustats' 命令也能在接下来一两帧后给出数据。
        gpuPassTimer.RequestPrint();
        std::cout << "[GPU] pass timing collection enabled; no completed samples "
                     "have been read back yet. A table will be logged after the next readback."
                  << std::endl;
        return;
    }

    const std::string table = VL::FormatGpuPassTimingTable(
        VL::ToGpuPassTimingTable(snapshot));
    VL::ProcessLog().Append(VL::LogSeverity::Info, "GPU", table);
    std::cout << "[Diagnostics][Info] " << table << std::endl;
}

void RenderSystem::PrintPerformanceJson()
{
    // 与 Performance 页消费同一个 GpuPassTimer 快照：面板、perf json 和
    // --perf-jsonl 只是同一份值的三个消费者，没有第二套计时或序列化规则。
    gpuPassTimer.RequestCollection();
    std::cout << VL::FormatPerformanceJson(gpuPassTimer.Snapshot()) << std::endl;
}

void RenderSystem::SetSceneViewport(const VL::UiViewportRect& rect, bool enabled)
{
    // GT/RT 并发：请求由互斥量保护；隐藏视口时保留最后有效尺寸，不触发离线重启。
    std::lock_guard<std::mutex> lock(sceneViewportMutex);
    sceneViewportRequested = enabled;
    if (rect.width != 0 && rect.height != 0) requestedSceneViewport = rect;
}

vk::Extent2D RenderSystem::GetSceneRenderExtent() const
{
    std::lock_guard<std::mutex> lock(sceneViewportMutex);
    if (sceneViewportRequested && requestedSceneViewport.width != 0 && requestedSceneViewport.height != 0)
        return {requestedSceneViewport.width, requestedSceneViewport.height};
    return rendererBackend->GetSwapchainExtent();
}

void RenderSystem::ReleaseSwapchainDependentResources()
{
    if (offlineCoordinator) offlineCoordinator->Stop();
    offlineDisplay.Shutdown();
    uiOverlayRenderer.ReleaseSwapchainDependentResources();
    VL::RendererResourceCache::GetInstance().ShutdownSwapchainDependentWorldResources();
    ShutdownFrameResources();
    initializedRenderWorldGeneration = 0;
}

void RenderSystem::RebuildSwapchainDependentResources()
{
    RenderGraph& renderGraph = RenderGraph::GetInstance();

    RefreshRenderSceneFromActiveWorld();
    if (!offlineMode) BuildResolvedRenderScene();
    RenderInitialize();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    if (!offlineMode)
    {
        renderGraph.InitializeAutoExposure(*pipelineFactory, *rendererBackend);
        renderGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
        if (prerenderSkinEnabled)
            renderGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
        renderGraph.RenderInitialize(*rendererBackend, descriptorContext);
        InitializeCurrentRenderSceneResources();
    }
    offlineDisplay.Initialize(*rendererBackend);
    if (offlineCoordinator) offlineCoordinator->Restart();
    offlineCameraMotionPolicy.Reset();
    if (uiOverlayRenderer.IsInitialized())
    {
        uiOverlayRenderer.RebuildSwapchainDependentResources();
    }
}

void RenderSystem::RebuildRenderGraphDependentResources()
{
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem renderer backend is not set");
    }

    RenderGraph& renderGraph = RenderGraph::GetInstance();

    RefreshRenderSceneFromActiveWorld();
    BuildResolvedRenderScene();
    RenderInitialize();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    renderGraph.InitializeAutoExposure(*pipelineFactory, *rendererBackend);
    renderGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
    if (prerenderSkinEnabled)
        renderGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
    renderGraph.RenderInitialize(*rendererBackend, descriptorContext);
    InitializeCurrentRenderSceneResources();
}

void RenderSystem::RefreshResolvedSceneAfterShaderReload()
{
    if (hasRenderScene && !offlineMode)
    {
        BuildResolvedRenderScene();
    }
}

VL::PreparedRuntimeBinding RenderSystem::PrepareRuntimeBinding(
    std::shared_ptr<const VL::World> world,
    VL::RendererResourceCache& candidateCache,
    RenderGraph& candidateGraph,
    std::shared_ptr<const VL::OfflineWorldAssets> offlineWorldAssets,
    bool prepareOffline)
{
    if (!world)
    {
        throw std::runtime_error(
            "Cannot prepare runtime binding without a candidate World");
    }
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error(
            "Cannot prepare runtime binding without a renderer backend");
    }

    VL::PreparedRuntimeBinding prepared;
    prepared.world = std::move(world);
    if (!offlineWorldAssets ||
        offlineWorldAssets->worldGeneration != prepared.world->GetGeneration())
        throw std::runtime_error("Offline CPU package generation mismatch");
    prepared.offlineWorldAssets = std::move(offlineWorldAssets);
    prepared.csmSettings =
        prepared.world->GetCsmSettings();

    VL::WorldSnapshotBuildDesc snapshotDesc;
    snapshotDesc.worldGeneration =
        prepared.world->GetGeneration();
    snapshotDesc.frameIndex = nextSnapshotFrameIndex;
    snapshotDesc.debugViewMode = debugViewMode;
    snapshotDesc.environmentIntensity =
        environmentIntensity;
    snapshotDesc.hasSelectedDraw =
        pickingRuntime.HasSelectedDraw() &&
        pickingRuntime.SelectedWorldGeneration() ==
            prepared.world->GetGeneration();
    snapshotDesc.selectedAllMaterialSlots =
        pickingRuntime.SelectedAllMaterialSlots() &&
        pickingRuntime.SelectedWorldGeneration() ==
            prepared.world->GetGeneration();
    snapshotDesc.selectedObjectId = pickingRuntime.SelectedObjectId();
    snapshotDesc.selectedMaterialSlotIndex =
        pickingRuntime.SelectedMaterialSlotIndex();
    auto snapshotResult =
        prepared.snapshotBuilder.Build(
            *prepared.world,
            snapshotDesc);
    if (snapshotResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(snapshotResult.Error()));
    }

    auto renderSceneResult =
        rendererFrontend.BuildRenderScene(
            snapshotResult.Value());
    if (renderSceneResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(
                renderSceneResult.Error()));
    }
    prepared.renderScene =
        std::move(renderSceneResult.Value());
    if (prepareOffline)
    {
        VL::OfflineRenderSettings settings = offlineSettings;
        settings.width = GetSceneRenderExtent().width;
        settings.height = GetSceneRenderExtent().height;
        auto offlineInput = VL::PreflightOfflineRenderInput(
            snapshotResult.Value(), *prepared.offlineWorldAssets, settings);
        if (offlineInput.IsFailure())
            throw std::runtime_error(VL::FormatRuntimeError(offlineInput.Error()));
        prepared.lightCapacity = frameResources.PrepareLightCapacity(0, *rendererBackend);
        prepared.nextSnapshotFrameIndex = nextSnapshotFrameIndex + 1;
        return prepared;
    }
    // 离线加载的 World 持有逻辑 M_ key；实时准备仅在渲染视图中绑定真正 GPU key，
    // 不重建 World 或改相机/对象作者状态。
    for (auto& draw : prepared.renderScene.drawPackets)
    {
        const auto instance = candidateCache.GetMaterialInstance(draw.materialInstance.key);
        if (instance)
            draw.material.key = instance->GetBaseMaterial().lock()->GetMaterialKey();
    }
    RebuildRenderSceneMaterialGroups(prepared.renderScene);

    auto resolvedSceneResult =
        resolvedRenderSceneBuilder.Build(
            prepared.renderScene,
            candidateCache);
    if (resolvedSceneResult.IsFailure())
    {
        throw std::runtime_error(
            VL::FormatRuntimeError(
                resolvedSceneResult.Error()));
    }
    prepared.resolvedRenderScene =
        std::move(resolvedSceneResult.Value());

    prepared.lightCapacity =
        frameResources.PrepareLightCapacity(
            prepared.renderScene.lights.size(),
            *rendererBackend);
    PrepareEnvironmentResources(
        prepared.renderScene,
        candidateCache);

    const std::vector<vk::DescriptorBufferInfo>&
        candidateLightBufferInfos =
            prepared.lightCapacity.GetBufferInfos(
                frameResources);
    const VL::RendererDescriptorContext
        descriptorContext =
            BuildRendererDescriptorContext(
                candidateCache,
                candidateGraph,
                candidateLightBufferInfos);
    candidateGraph.InitializeAutoExposure(*pipelineFactory, *rendererBackend);
    candidateGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
    if (prerenderSkinEnabled)
        candidateGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
    candidateGraph.RenderInitialize(
        *rendererBackend,
        descriptorContext);

    VL::RendererObjectResourceRegistry
        objectResourceRegistry;
    objectResourceRegistry.InitializeResolvedSceneResources(
        *rendererBackend,
        descriptorContext,
        prepared.renderScene,
        prepared.resolvedRenderScene,
        candidateCache,
        prerenderSkinEnabled);

    prepared.windResponseProfiles.Configure(
        prepared.world->GetWindResponseProfiles());
    prepared.nextSnapshotFrameIndex =
        nextSnapshotFrameIndex + 1;
    return prepared;
}

std::shared_ptr<void>
RenderSystem::CommitPreparedRuntimeBinding(
    VL::PreparedRuntimeBinding prepared) noexcept
{
    activeWorld.swap(prepared.world);
    activeOfflineWorldAssets.swap(prepared.offlineWorldAssets);
    worldSnapshotBuilder.Swap(
        prepared.snapshotBuilder);

    using std::swap;
    swap(
        currentRenderScene.worldGeneration,
        prepared.renderScene.worldGeneration);
    swap(
        currentRenderScene.frameIndex,
        prepared.renderScene.frameIndex);
    swap(
        currentRenderScene.hairSimulationDeltaSeconds,
        prepared.renderScene.hairSimulationDeltaSeconds);
    swap(
        currentRenderScene.camera,
        prepared.renderScene.camera);
    currentRenderScene.drawPackets.swap(
        prepared.renderScene.drawPackets);
    currentRenderScene.materialGroups.swap(
        prepared.renderScene.materialGroups);
    currentRenderScene.lights.swap(
        prepared.renderScene.lights);
    swap(
        currentRenderScene.environment,
        prepared.renderScene.environment);
    // 场景作者目标与场级解析结果随同一代 RenderScene 提交；头发消费 resolvedWind，
    // 植被消费 wind 目标并在每 profile 内维护自己的响应/gust 状态。
    swap(currentRenderScene.wind, prepared.renderScene.wind);
    swap(currentRenderScene.resolvedWind, prepared.renderScene.resolvedWind);
    swap(currentRenderScene.windTimeSeconds, prepared.renderScene.windTimeSeconds);
    swap(
        currentRenderScene.debugViewMode,
        prepared.renderScene.debugViewMode);
    currentResolvedRenderScene.materialGroups.swap(
        prepared.resolvedRenderScene.materialGroups);
    windResponseProfiles.Swap(
        prepared.windResponseProfiles);
    csmRuntime.SetSettings(prepared.csmSettings);

    std::shared_ptr<void> retiredLightBuffer =
        frameResources.CommitPreparedLightCapacity(
            std::move(prepared.lightCapacity));
    hasRenderScene = true;
    currentWindTimeSeconds = 0.0;
    windClockInitialized = false;
    windPendingDeltaSeconds.store(0.0f, std::memory_order_relaxed);
    windTickAccumulator = 0.0;
    sceneWindRuntime.Reset();
    initializedRenderWorldGeneration =
        currentRenderScene.worldGeneration;
    nextSnapshotFrameIndex =
        prepared.nextSnapshotFrameIndex;
    ClearSelectedMaterialInstance();
    environmentRuntime.Reset();
    // The old World package keeps its HDRI alive until the prepared retire
    // epoch. Drop descriptor-source mirrors now so they cannot outlive that
    // package; the next IBL use rewrites the acquired image's descriptor.
    environmentIblBaker.InvalidateEnvironmentCubeBindings();
    return retiredLightBuffer;
}

void RenderSystem::ClearPendingWorldSnapshots() noexcept
{
    worldSnapshotQueue.Clear();
}

void RenderSystem::PrepareRenderGraphReload(
    RenderGraph& candidateGraph)
{
    if (!activeWorld || !hasRenderScene)
    {
        throw std::runtime_error(
            "Cannot prepare a RenderGraph reload without an active runtime binding");
    }
    const VL::RendererDescriptorContext
        descriptorContext =
            BuildRendererDescriptorContext(
                VL::RendererResourceCache::GetInstance(),
                candidateGraph,
                GetLightBufferInfo());
    candidateGraph.InitializeAutoExposure(*pipelineFactory, *rendererBackend);
    candidateGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
    if (prerenderSkinEnabled)
        candidateGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
    candidateGraph.RenderInitialize(
        *rendererBackend,
        descriptorContext);
}

std::string RenderSystem::GetResolvedShaderGenerationFingerprint() const
{
    std::string fingerprint;
    for (const VL::ResolvedMaterialGroup& materialGroup :
         currentResolvedRenderScene.materialGroups)
    {
        if (!materialGroup.material)
        {
            continue;
        }
        fingerprint += materialGroup.material->GetMaterialKey();
        fingerprint += ":";
        fingerprint +=
            materialGroup.material
                ->GetSurfaceShaderArtifact()
                .artifactGenerationKey;
        if (materialGroup.material->GetShadowShaderArtifact())
        {
            fingerprint += ":";
            fingerprint +=
                materialGroup.material
                    ->GetShadowShaderArtifact()
                    ->artifactGenerationKey;
        }
        fingerprint += ";";
    }
    return fingerprint;
}

uint64_t RenderSystem::GetActiveWorldGeneration() const noexcept
{
    return activeWorld
        ? activeWorld->GetGeneration()
        : 0;
}

void RenderSystem::SetPickingSceneEnabled(bool enabled) noexcept
{
    pickingRuntime.SetSceneEnabled(enabled);
}

std::optional<VL::MaterialInstanceSelection>
RenderSystem::PickMaterialInstanceAt(
    float mouseX,
    float mouseY,
    uint32_t viewportWidth,
    uint32_t viewportHeight) const
{
    if (!activeWorld)
    {
        return std::nullopt;
    }
    return pickingRuntime.Pick(
        activeWorld->GetScenePath(),
        activeWorld->GetGeneration(),
        mouseX,
        mouseY,
        viewportWidth,
        viewportHeight);
}

std::vector<VL::MaterialInstanceModelContext>
RenderSystem::GetMaterialInstanceModelContexts() const
{
    if (!activeWorld)
    {
        return {};
    }
    return pickingRuntime.GetModelContexts(
        activeWorld->GetScenePath(),
        activeWorld->GetGeneration());
}

void RenderSystem::SetSelectedMaterialInstanceModel(
    const VL::MaterialInstanceModelContext& model)
{
    pickingRuntime.SetSelectedMaterialInstanceModel(
        model,
        activeWorld ? activeWorld->GetGeneration() : 0,
        activeWorld != nullptr);
}

void RenderSystem::SetSelectedMaterialInstance(
    const VL::MaterialInstanceSelection& selection)
{
    pickingRuntime.SetSelectedMaterialInstance(
        selection,
        activeWorld ? activeWorld->GetGeneration() : 0,
        activeWorld != nullptr);
}

void RenderSystem::ClearSelectedMaterialInstance() noexcept
{
    pickingRuntime.ClearSelectedMaterialInstance();
}


std::unique_ptr<VL::IMaterialInstancePreviewAdapter>
RenderSystem::CreateMaterialInstancePreviewAdapter()
{
    VL::IRendererMaterialInstancePreviewOwner& owner = *this;
    return std::make_unique<
        VL::RendererOwnedMaterialInstancePreviewAdapter>(owner);
}

std::optional<VL::MaterialInstancePreviewWorldIdentity>
RenderSystem::GetActiveWorldIdentity() const
{
    if (!activeWorld)
    {
        return std::nullopt;
    }

    VL::MaterialInstancePreviewWorldIdentity identity;
    identity.generation = activeWorld->GetGeneration();
    identity.scenePath = activeWorld->GetScenePath();
    return VL::NormalizeWorldIdentity(std::move(identity));
}

VL::MaterialInstancePreviewAdapterResult
RenderSystem::CaptureMaterialInstancePreviewSession(
    const VL::MaterialInstancePreviewAdapterCommand& command,
    std::shared_ptr<
        VL::IRendererMaterialInstancePreviewSession>&
        outSession)
{
    using namespace VL;

    outSession.reset();
    MaterialInstancePreviewAdapterResult result;
    result.failureStage = PreviewAdapterFailureStage::Prepare;

    if (!command.world.IsValid() ||
        !command.materialInstancePath.IsValid())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Renderer owner received an invalid World identity or MI path.";
        return result;
    }

    const std::optional<MaterialInstancePreviewWorldIdentity> activeWorldIdentity =
        GetActiveWorldIdentity();
    if (!activeWorldIdentity.has_value())
    {
        result.status = PreviewAdapterOperationStatus::Unavailable;
        result.message =
            "Renderer owner cannot capture a preview target without an active World.";
        return result;
    }
    if (*activeWorldIdentity != command.world)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Renderer owner rejected a preview target from an obsolete World generation.";
        return result;
    }
    if (offlineMode)
    {
        if (!activeOfflineWorldAssets)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = "Frozen offline World assets are missing";
            return result;
        }
        const auto found = activeOfflineWorldAssets->materialInstanceSources.find(
            command.materialInstancePath.value);
        if (found == activeOfflineWorldAssets->materialInstanceSources.end())
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = "Frozen offline MI is missing";
            return result;
        }
        MaterialInstancePreviewDraft baseline;
        baseline.materialInstancePath = command.materialInstancePath.value;
        baseline.documentRevision = command.documentRevision.value_or(0);
        baseline.serializedWorkingDraft = found->second.dump();
        outSession = VL::MakeOfflineMaterialInstancePreviewSession(
            *this, command.world, command.materialInstancePath, std::move(baseline));
        result.status = PreviewAdapterOperationStatus::Completed;
        result.runtimeResourceGeneration = command.world.generation;
        result.message = "Frozen offline material preview connected";
        return result;
    }

    const std::optional<
        VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
        capture =
            VL::RendererResourceCache::GetInstance()
                .CaptureMaterialInstanceForGeneration(
                    command.world.generation,
                    command.materialInstancePath.value);
    if (!capture.has_value() || !capture->materialInstance)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Active World-local MaterialInstance was not found for the normalized MI path.";
        return result;
    }

    try
    {
        const MaterialInstanceStateSnapshot snapshot =
            capture->materialInstance->CaptureStateSnapshot();
        if (!activeOfflineWorldAssets)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message =
                "Renderer owner has no frozen material assets.";
            return result;
        }
        // Connect 的 baseline 与 World 提交时冻结的源一致；磁盘上后来的
        // MI/M_ 改动不能在 Restore 时悄悄混入当前有效版本。
        const nlohmann::json& materialInstanceJson =
            activeOfflineWorldAssets->materialInstanceSources.at(
                command.materialInstancePath.value);
        const MaterialInstanceResolveResult resolved =
            MaterialInstanceResolver::ResolveWithDefinition(
                command.materialInstancePath.value,
                materialInstanceJson,
                activeOfflineWorldAssets->materialDefinitions.at(
                    command.materialInstancePath.value));
        const VL::PreviewRenderStateValues baselineRenderStates =
            VL::BuildPreviewRenderStateValues(
                resolved.effectiveMaterialInstanceJson);
        VL::MaterialInstancePreviewDraft baselineDraft;
        baselineDraft.materialInstancePath =
            command.materialInstancePath.value;
        baselineDraft.documentRevision =
            command.documentRevision.value_or(0);
        baselineDraft.serializedWorkingDraft =
            materialInstanceJson.dump();
        outSession = VL::MakeMaterialInstancePreviewSession(
            *this,
            command.world,
            command.materialInstancePath,
            capture->ownerGeneration,
            capture->materialInstance,
            snapshot.parameters,
            snapshot.textures,
            baselineRenderStates,
            std::move(baselineDraft));
    }
    catch (const std::exception& exception)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            std::string("Renderer owner failed to capture MI numeric baseline: ") +
            exception.what();
        return result;
    }

    result.status = PreviewAdapterOperationStatus::Completed;
    result.runtimeResourceGeneration = capture->ownerGeneration;
    result.liveSwapCommitted = false;
    result.replacesLiveResource = false;
    result.message =
        "Renderer-owned numeric material instance preview connected.";
    return result;
}

std::shared_ptr<const VL::OfflineWorldAssets>
RenderSystem::PrepareOfflineMaterialPreview(
    const std::string& materialInstancePath,
    const nlohmann::json& materialInstanceJson) const
{
    if (!activeOfflineWorldAssets)
        throw std::runtime_error("Active World has no frozen offline assets");

    const std::string key =
        MaterialAssetUtils::NormalizeAssetPath(materialInstancePath);
    if (activeOfflineWorldAssets->effectiveMaterialInstances.find(key) ==
        activeOfflineWorldAssets->effectiveMaterialInstances.end())
        throw std::runtime_error("Preview MI is absent from the frozen World: " + key);
    // MI 可以覆盖参数，却不能在旧 World 的几何/材质槽下改绑另一份 M_。
    // 这种改绑须经过 World 事务，避免 CPU 与 GPU 候选解释不同的定义。
    const std::string frozenMaterial =
        activeOfflineWorldAssets->materialInstanceSources.at(key)
            .at("material").get<std::string>();
    if (!materialInstanceJson.is_object() ||
        !materialInstanceJson.contains("material") ||
        !materialInstanceJson["material"].is_string() ||
        MaterialAssetUtils::NormalizeAssetPath(
            materialInstanceJson["material"].get<std::string>()) !=
            MaterialAssetUtils::NormalizeAssetPath(frozenMaterial))
        throw std::runtime_error("Preview MI cannot change its frozen material definition: " + key);

    const MaterialInstanceResolveResult resolved =
        MaterialInstanceResolver::ResolveWithDefinition(
            key, materialInstanceJson,
            activeOfflineWorldAssets->materialDefinitions.at(key));
    // 候选只替换一个 MI 的有效值；旧包和正在运行的 CPU 任务始终只读旧版本。
    auto candidate = std::make_shared<VL::OfflineWorldAssets>(
        *activeOfflineWorldAssets);
    candidate->effectiveMaterialInstances.insert_or_assign(
        key, resolved.effectiveMaterialInstanceJson);
    candidate->materialDefinitions.insert_or_assign(key, resolved.materialJson);
    candidate->materialInstanceSources.insert_or_assign(key, materialInstanceJson);
    VL::FreezeOfflineMaterialTextures(*candidate, resolved.effectiveMaterialInstanceJson);

    if (resolved.effectiveMaterialInstanceJson.contains("modelProfile"))
    {
        const std::string reference =
            resolved.effectiveMaterialInstanceJson["modelProfile"].get<std::string>();
        const std::filesystem::path physical = CommonFunction::Path(reference);
        std::ifstream file(physical, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open modelProfile: " + reference);
        const std::string bytes(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>{});
        if (file.bad())
            throw std::runtime_error("Cannot read modelProfile: " + reference);
        candidate->modelProfiles.insert_or_assign(
            reference, nlohmann::json::parse(bytes));
        candidate->modelProfilePaths.insert_or_assign(reference, physical);
        candidate->modelProfileDigests.insert_or_assign(
            reference, VL::ContentHasher::HashString(bytes).ToHex());
    }
    VL::PruneUnusedOfflineMaterialAssets(*candidate);
    return candidate;
}

VL::MaterialInstancePreviewAdapterResult
RenderSystem::CommitMaterialInstancePreviewDraft(
    const VL::MaterialInstancePreviewWorldIdentity& world,
    const VL::NormalizedMaterialInstancePath& materialInstancePath,
    const VL::MaterialInstancePreviewDraft& draft)
{
    using namespace VL;

    MaterialInstancePreviewAdapterResult result;
    result.failureStage = PreviewAdapterFailureStage::Prepare;
    if (offlineMode)
    {
        try
        {
            const auto draftPath = NormalizeMaterialInstancePath(draft.materialInstancePath);
            if (!world.IsValid() || !materialInstancePath.IsValid() ||
                GetActiveWorldIdentity() != world || !activeOfflineWorldAssets ||
                !draftPath.Succeeded() ||
                *draftPath.path != materialInstancePath)
                throw std::runtime_error("Offline preview targets an obsolete World or MI");
            const auto source = nlohmann::json::parse(draft.serializedWorkingDraft);
            auto candidate = PrepareOfflineMaterialPreview(materialInstancePath.value, source);
            VL::WorldSnapshotBuilder builder;
            VL::WorldSnapshotBuildDesc desc;
            desc.worldGeneration = world.generation;
            auto snapshot = builder.Build(*activeWorld, desc);
            if (snapshot.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(snapshot.Error()));
            auto settings = offlineSettings;
            settings.width = GetSceneRenderExtent().width;
            settings.height = GetSceneRenderExtent().height;
            const auto input = VL::PreflightOfflineRenderInput(snapshot.Value(), *candidate, settings);
            if (input.IsFailure())
                throw std::runtime_error(VL::FormatRuntimeError(input.Error()));
            for (const auto& [reference, digest] : candidate->modelProfileDigests)
                if (VL::ContentHasher::HashFile(candidate->modelProfilePaths.at(reference)).ToHex() != digest)
                    throw std::runtime_error("modelProfile changed before preview commit: " + reference);
            VL::ValidateOfflineTexturesStillCurrent(*candidate);
            auto published = std::make_shared<VL::OfflineWorldAssets>(*candidate);
            // GPU 仍是旧版本；回切实时必须按此成功 Apply 的冻结值重建。
            published->realtimePrepared = false;
            WaitForRendererMutationSafePoint();
            activeOfflineWorldAssets = std::move(published);
            result.status = PreviewAdapterOperationStatus::Completed;
            result.failureStage = PreviewAdapterFailureStage::None;
            result.runtimeResourceGeneration = world.generation;
            result.liveSwapCommitted = true;
            result.message = "Offline effective preview committed; realtime pending synchronization";
        }
        catch (const std::exception& exception)
        {
            result.status = PreviewAdapterOperationStatus::Failed;
            result.message = exception.what();
        }
        return result;
    }
    if (!world.IsValid() || !materialInstancePath.IsValid())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Material instance preview replacement received an invalid identity.";
        return result;
    }
    if (rendererBackend == nullptr || pipelineFactory == nullptr)
    {
        result.status = PreviewAdapterOperationStatus::Unavailable;
        result.message =
            "Material instance preview replacement requires initialized renderer services.";
        return result;
    }
    if (!activeWorld || !hasRenderScene ||
        activeWorld->GetGeneration() != world.generation ||
        currentRenderScene.worldGeneration != world.generation)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Material instance preview replacement targets an obsolete or uninitialized World.";
        return result;
    }

    VL::RendererResourceCache& activeCache =
        VL::RendererResourceCache::GetInstance();
    const std::optional<VL::RendererResourceCache::WorldLocalMaterialInstanceCapture>
        activeCapture = activeCache.CaptureMaterialInstanceForGeneration(
            world.generation,
            materialInstancePath.value);
    if (!activeCapture.has_value() || !activeCapture->materialInstance)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "Active World-local MaterialInstance was not found for the preview replacement.";
        return result;
    }

    VL::NumericPreviewDraftParseResult parsed =
        VL::ParseNumericPreviewDraft(
            draft,
            materialInstancePath,
            activeCapture->materialInstance->CaptureStateSnapshot().textures,
            activeOfflineWorldAssets->materialDefinitions.at(
                materialInstancePath.value));
    if (!parsed.serializedJson.has_value())
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message = std::move(parsed.errorMessage);
        return result;
    }

    size_t targetDrawCount = 0;
    for (const VL::RenderDrawPacket& drawPacket : currentRenderScene.drawPackets)
    {
        if (drawPacket.materialInstance.key == materialInstancePath.value)
        {
            ++targetDrawCount;
        }
    }
    if (targetDrawCount == 0)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.message =
            "The preview material instance is not referenced by the active RenderScene.";
        return result;
    }

    try
    {
        std::shared_ptr<const VL::OfflineWorldAssets> preparedOfflineAssets =
            PrepareOfflineMaterialPreview(
                materialInstancePath.value, *parsed.serializedJson);
        VL::RendererResourceCache candidateCache =
            activeCache.BeginActiveWorldCandidate(world.generation);
        RenderGraph& renderGraph = RenderGraph::GetInstance();
        PipelineFactory::GraphicsCandidateState graphicsCandidateState;
        VL::RendererResourceLoadContext loadContext{
            candidateCache,
            renderGraph};
        loadContext.graphicsCandidateState = &graphicsCandidateState;
        // GPU 候选与离线候选消费同一份冻结定义和有效覆写，不能再读磁盘 M_。
        auto frozenGpuAssets = std::make_shared<VL::OfflineWorldAssets>(
            *preparedOfflineAssets);
        loadContext.offlineWorldAssets = frozenGpuAssets.get();
        loadContext.useFrozenMaterials = true;
        // MI 预览候选复用主渲染的预蒙皮选择，避免同一材质在预览里切回混合路径。
        loadContext.prerenderSkin = prerenderSkinEnabled;

        VL::RendererMaterialLoader materialLoader(
            *pipelineFactory,
            *rendererBackend,
            loadContext);
        const std::shared_ptr<MaterialInstance> replacementMaterialInstance =
            materialLoader.LoadSceneMaterialInstance(materialInstancePath.value);
        if (!replacementMaterialInstance)
        {
            throw std::runtime_error(
                "Renderer material loader returned an empty preview MaterialInstance");
        }
        const std::shared_ptr<Material> replacementMaterial =
            replacementMaterialInstance->GetBaseMaterial().lock();
        if (!replacementMaterial)
        {
            throw std::runtime_error(
                "Preview MaterialInstance has no replacement base Material");
        }

        VL::RenderScene candidateRenderScene = currentRenderScene;
        ApplyMaterialOverrideToRenderScene(
            candidateRenderScene,
            materialInstancePath.value,
            replacementMaterial->GetMaterialKey());
        RebuildRenderSceneMaterialGroupsImpl(candidateRenderScene);

        auto resolvedSceneResult = resolvedRenderSceneBuilder.Build(
            candidateRenderScene,
            candidateCache);
        if (resolvedSceneResult.IsFailure())
        {
            throw std::runtime_error(
                VL::FormatRuntimeError(resolvedSceneResult.Error()));
        }
        VL::ResolvedRenderScene candidateResolvedRenderScene =
            std::move(resolvedSceneResult.Value());

        const VL::RendererDescriptorContext descriptorContext =
            BuildRendererDescriptorContext(
                candidateCache,
                renderGraph,
                GetLightBufferInfo());
        VL::RendererObjectResourceRegistry objectResourceRegistry;
        objectResourceRegistry.InitializeResolvedSceneResources(
            *rendererBackend,
            descriptorContext,
            candidateRenderScene,
            candidateResolvedRenderScene,
            candidateCache,
            prerenderSkinEnabled);

        PipelineFactory::PreparedGraphicsCandidateCommit preparedPipelineCommit =
            pipelineFactory->PrepareCandidateCommit(graphicsCandidateState);

        const std::string replacementMaterialKey =
            replacementMaterial->GetMaterialKey();
        std::string replacementPath = materialInstancePath.value;
        std::string replacementKey = replacementMaterialKey;
        VL::RenderScene candidatePickingRenderScene =
            pickingRuntime.CopyPickingScene();
        ApplyMaterialOverrideToRenderScene(
            candidatePickingRenderScene,
            materialInstancePath.value,
            replacementMaterialKey);
        RebuildRenderSceneMaterialGroupsImpl(candidatePickingRenderScene);

        VL::ResourceRetireQueue& retireQueue =
            VL::ResourceRetireQueue::GetInstance();
        std::vector<VL::RetiredResource> retirementResources;
        retirementResources.reserve(1);
        retirementResources.push_back({
            "MaterialInstancePreview:WorldLocalResources",
            world.generation,
            retireQueue.GetLastSubmittedEpoch(),
            {}});
        VL::PreparedResourceRetirements preparedRetirements =
            retireQueue.PrepareRetirements(std::move(retirementResources));

        for (const auto& [reference, captured] :
             preparedOfflineAssets->modelProfileDigests)
        {
            const auto& path =
                preparedOfflineAssets->modelProfilePaths.at(reference);
            if (!std::filesystem::is_regular_file(path) ||
                VL::ContentHasher::HashFile(path).ToHex() != captured)
                throw std::runtime_error(
                    "modelProfile changed before preview commit: " + reference);
        }
        VL::ValidateOfflineTexturesStillCurrent(*preparedOfflineAssets);
        // Candidate resources are isolated; only the publication below touches
        // state that RT reads. Wait exactly at the ownership hand-off.
        WaitForRendererMutationSafePoint();
        pipelineFactory->CommitPreparedCandidate(
            std::move(preparedPipelineCommit));
        VL::RendererResourceCache::WorldLocalResourcePackageHandle retiredPackage =
        activeCache.CommitCandidate(std::move(candidateCache));
        std::swap(currentRenderScene, candidateRenderScene);
        std::swap(currentResolvedRenderScene, candidateResolvedRenderScene);
        pickingRuntime.SwapPickingScene(candidatePickingRenderScene);
        activeOfflineWorldAssets.swap(preparedOfflineAssets);

        hasMaterialInstancePreviewMaterialOverride = true;
        materialInstancePreviewOverridePath.swap(replacementPath);
        materialInstancePreviewOverrideMaterialKey.swap(replacementKey);

        preparedRetirements.GetAdditionalResource(0).resource =
            std::static_pointer_cast<void>(std::move(retiredPackage));
        retireQueue.CommitPreparedRetirements(
            std::move(preparedRetirements));

        result.status = PreviewAdapterOperationStatus::Completed;
        result.failureStage = PreviewAdapterFailureStage::None;
        result.runtimeResourceGeneration = world.generation;
        result.liveSwapCommitted = true;
        result.replacesLiveResource = true;
        result.message =
            "Material instance render-state preview applied with a live resource replacement.";
        return result;
    }
    catch (const std::exception& exception)
    {
        result.status = PreviewAdapterOperationStatus::Failed;
        result.failureStage = PreviewAdapterFailureStage::Prepare;
        result.message =
            std::string("Material instance preview resource replacement failed: ") +
            exception.what();
        return result;
    }
}

void RenderSystem::ApplyMaterialInstancePreviewMaterialOverride(
    VL::RenderScene& renderScene) const
{
    if (!hasMaterialInstancePreviewMaterialOverride ||
        materialInstancePreviewOverridePath.empty() ||
        materialInstancePreviewOverrideMaterialKey.empty())
    {
        return;
    }
    ApplyMaterialOverrideToRenderScene(
        renderScene,
        materialInstancePreviewOverridePath,
        materialInstancePreviewOverrideMaterialKey);
}

void RenderSystem::RebuildRenderSceneMaterialGroups(
    VL::RenderScene& renderScene) const
{
    RebuildRenderSceneMaterialGroupsImpl(renderScene);
}

void RenderSystem::UpdateGlobalUBOForPass(vk::CommandBuffer& commandBuffer)
{
    UpdateUBOGlobal(commandBuffer);
}

void RenderSystem::UpdateShadowGlobalUBOForPass(
    vk::CommandBuffer& commandBuffer,
    uint32_t passWidth,
    uint32_t passHeight,
    uint32_t cascadeIndex)
{
    UpdateUBOGlobalForShadow(commandBuffer, passWidth, passHeight, cascadeIndex);
}

void RenderSystem::UpdateMaterialInstanceUBOForPass(
    const std::shared_ptr<MaterialInstance>& materialInstance)
{
    frameResources.UpdateMaterialInstanceUniformBuffer(swapChainImageIndex, *materialInstance);
}

void RenderSystem::UpdateObjectUBOForPass(
    VL::RendererObjectGpuResources& objectResources,
    const VL::RenderDrawPacket& drawPacket)
{
    // Object transforms come from the frozen RenderDrawPacket. The upload path
    // addresses backend-owned object GPU resources through the resolved draw
    // packet.
    const SpeedTreeWindStateGPU* windState =
        windResponseProfiles.FindGpuState(drawPacket.windResponseProfileKey);
    objectResources.submittedSkin.RetainIfSubmittedAt(submittedFrameSerial);
    frameResources.UpdateObjectUniformBuffer(
        swapChainImageIndex,
        objectResources,
        drawPacket,
        windState,
        // 有 pre-skin 输出 buffer 的对象已在帧前上传完整 palette；没有该 buffer
        // 的蒙皮对象仍按原路径逐 draw 上传，避免 flag 与对象资源不一致时漏写。
        !prerenderSkinEnabled || !objectResources.skinDeform.HasResources());
}

void RenderSystem::UploadLightsForPass(
    uint32_t swapChainImageIndex,
    const std::vector<VL::LightSnapshot>& lights)
{
    frameResources.UpdateLightBuffer(swapChainImageIndex, lights);
}

bool RenderSystem::IsCsmEnabled() const
{
    return csmRuntime.IsEnabled();
}

bool RenderSystem::IsShadowCascadeActive(
    uint32_t cascadeIndex) const
{
    return csmRuntime.IsCascadeActive(cascadeIndex);
}

bool RenderSystem::GetShadowCascadeLightViewProjection(
    uint32_t cascadeIndex,
    Eigen::Matrix4f& outLightViewProjection) const
{
    return csmRuntime.GetCascadeLightViewProjection(
        cascadeIndex,
        outLightViewProjection);
}

bool RenderSystem::GetShadowCascadeCasterReceiverBounds(
    uint32_t cascadeIndex,
    VL::ShadowCasterReceiverBounds& outBounds) const
{
    return csmRuntime.GetCascadeCasterReceiverBounds(
        cascadeIndex,
        outBounds);
}

void RenderSystem::LogShadowCasterCullStats() const
{
    // 统计的是最近一帧的 draw 数，不是窗口平均：级联覆盖是离散的，
    // 平均值会把“某级联恰好 0 draw”这个关键证据抹平。
    std::ostringstream message;
    message << "shadow caster culling (drawn/candidates):";
    const VL::CsmSettings& frameSettings =
        csmRuntime.GetFrameSettings();
    for (uint32_t cascadeIndex = 0;
         cascadeIndex < frameSettings.cascadeCount;
         ++cascadeIndex)
    {
        const VL::ShadowCasterCullStats& stats =
            frameShadowCasterCullStats[cascadeIndex];
        message << " cascade" << cascadeIndex << ' '
                << stats.submittedDraws << '/' << stats.casterDraws;
    }
    const std::string line = message.str();
    VL::ProcessLog().Append(VL::LogSeverity::Info, "GPU", line);
    std::cout << "[Diagnostics][Info] " << line << std::endl;
}

void RenderSystem::DrawEditorGridGeometry(
    vk::CommandBuffer& commandBuffer)
{
    // 管线与描述符集由 pass 流程绑定，这里只提交几何；网格顶点缓冲是静态
    // 世界几何，由 editor 注入的覆盖层负责创建与生命周期。
    if (editorViewportOverlay == nullptr || rendererBackend == nullptr)
    {
        return;
    }
    editorViewportOverlay->EnsureCreated(*rendererBackend);
    editorViewportOverlay->Bind(commandBuffer);
    editorViewportOverlay->Draw(commandBuffer);
}

void RenderSystem::UpdateUBOGlobal(vk::CommandBuffer& commandBuffer)
{
    frameRecorder.UpdateGlobalUBO(
        commandBuffer,
        frameResources,
        swapChainImageIndex,
        currentRenderScene,
        hasRenderScene,
        [this] { RefreshRenderSceneFromActiveWorld(); },
        csmRuntime,
        currentFrame);
}

void RenderSystem::UpdateUBOGlobalWithProjectionTransform(
    vk::CommandBuffer& commandBuffer,
    float projectionScaleX,
    float projectionScaleY,
    float projectionOffsetX,
    float projectionOffsetY)
{
    frameRecorder.UpdateGlobalUBOWithProjectionTransform(
        commandBuffer,
        frameResources,
        swapChainImageIndex,
        currentRenderScene,
        hasRenderScene,
        [this] { RefreshRenderSceneFromActiveWorld(); },
        csmRuntime,
        currentFrame,
        projectionScaleX,
        projectionScaleY,
        projectionOffsetX,
        projectionOffsetY);
}

void RenderSystem::UpdateExposureFrameTiming()
{
    frameRecorder.UpdateExposureFrameTiming(gpuPassTimer);
}

void RenderSystem::UpdateUBOGlobalForShadow(
    vk::CommandBuffer& commandBuffer,
    uint32_t passSizeWidth,
    uint32_t passSizeHeight,
    uint32_t cascadeIndex)
{
    frameRecorder.UpdateShadowGlobalUBO(
        commandBuffer,
        frameResources,
        swapChainImageIndex,
        currentRenderScene,
        hasRenderScene,
        [this] { RefreshRenderSceneFromActiveWorld(); },
        csmRuntime,
        currentFrame,
        passSizeWidth,
        passSizeHeight,
        cascadeIndex);
}

void RenderSystem::BuildShadowCascadeFrameData(uint32_t passSizeWidth, uint32_t passSizeHeight)
{
    csmRuntime.BuildFrameData(
        currentRenderScene,
        currentFrame,
        passSizeWidth,
        passSizeHeight);
}

void RenderSystem::RefreshRenderSceneFromActiveWorld()
{
    PublishSnapshotFromActiveWorld(false);
    if (!ConsumeLatestSnapshotIntoRenderScene())
    {
        throw std::runtime_error("WorldSnapshotQueue did not return the snapshot just published");
    }
}

void RenderSystem::PublishSnapshotFromActiveWorld()
{
    PublishSnapshotFromActiveWorld(true);
}

void RenderSystem::QueueDynamicVertices(
    std::string meshKey,
    std::vector<Vertex> vertices)
{
    dynamicMeshUpdateQueue.QueueVertices(std::move(meshKey), std::move(vertices));
}

void RenderSystem::QueueDynamicMesh(
    std::string meshKey,
    std::vector<Vertex> vertices,
    std::vector<uint32_t> indices)
{
    dynamicMeshUpdateQueue.QueueMesh(
        std::move(meshKey), std::move(vertices), std::move(indices));
}

void RenderSystem::PublishSnapshotFromActiveWorld(bool advanceSimulation)
{
    if (!activeWorld)
    {
        throw std::runtime_error("RenderSystem active World is not set");
    }

    VL::WorldSnapshotBuildDesc buildDesc;
    buildDesc.worldGeneration = activeWorld->GetGeneration();
    buildDesc.frameIndex = nextSnapshotFrameIndex++;
    // resize/图重建只刷新姿态，不拥有新的 GT 时间片；普通帧成功发布后
    // 领取一次 delta，避免组合 Render 或重复发布用新 frameIndex 重放旧时间。
    if (advanceSimulation)
    {
        buildDesc.simulationDeltaSeconds = simulationDeltaSeconds;
        buildDesc.gameSimulationDeltaSeconds = gameSimulationDeltaSeconds;
        buildDesc.hairSimulationDeltaSeconds = hairSimulationDeltaSeconds;
    }
    buildDesc.debugViewMode = debugViewMode;
    buildDesc.environmentIntensity = environmentIntensity;
    buildDesc.hasSelectedDraw =
        pickingRuntime.HasSelectedDraw() &&
        pickingRuntime.SelectedWorldGeneration() ==
            activeWorld->GetGeneration();
    buildDesc.selectedAllMaterialSlots =
        pickingRuntime.SelectedAllMaterialSlots() &&
        pickingRuntime.SelectedWorldGeneration() ==
            activeWorld->GetGeneration();
    buildDesc.selectedObjectId = pickingRuntime.SelectedObjectId();
    buildDesc.selectedMaterialSlotIndex =
        pickingRuntime.SelectedMaterialSlotIndex();

    auto snapshotResult = worldSnapshotBuilder.Build(*activeWorld, buildDesc);
    if (snapshotResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(snapshotResult.Error()));
    }

    VL::WorldSnapshot snapshot = std::move(snapshotResult.Value());
    if (offlineMode && offlineCoordinator)
    {
        currentOfflineSnapshot =
            std::make_shared<const VL::WorldSnapshot>(snapshot);
        offlineSettings.width = GetSceneRenderExtent().width;
        offlineSettings.height = GetSceneRenderExtent().height;
        const auto target = offlineCameraMotionPolicy.Decide(
            snapshot.camera, snapshot.worldGeneration,
            activeOfflineWorldAssets.get(), offlineSettings,
            offlineCoordinator->State(),
            VL::OfflineCameraMotionPolicy::Clock::now());
        if (target)
            offlineCoordinator->Observe(currentOfflineSnapshot,
                activeOfflineWorldAssets, *target);
    }
    if (pickingRuntime.IsSceneEnabled())
    {
        auto pickingRenderSceneResult =
            rendererFrontend.BuildRenderScene(snapshot);
        if (pickingRenderSceneResult.IsFailure())
        {
            throw std::runtime_error(
                VL::FormatRuntimeError(pickingRenderSceneResult.Error()));
        }
        VL::RenderScene pickingRenderScene =
            std::move(pickingRenderSceneResult.Value());
        ApplyMaterialInstancePreviewMaterialOverride(pickingRenderScene);
        RebuildRenderSceneMaterialGroups(pickingRenderScene);
        pickingRuntime.SetPickingScene(std::move(pickingRenderScene));
    }
    else
    {
        pickingRuntime.InvalidatePickingScene();
    }
    worldSnapshotQueue.Publish(std::move(snapshot));
    if (advanceSimulation)
    {
        simulationDeltaSeconds = 0.0f;
        gameSimulationDeltaSeconds = 0.0f;
        hairSimulationDeltaSeconds = 0.0f;
    }
}

bool RenderSystem::ConsumeLatestSnapshotIntoRenderScene()
{
    auto snapshot = worldSnapshotQueue.ConsumeLatest();
    if (!snapshot.has_value())
    {
        return false;
    }

    auto renderSceneResult = rendererFrontend.BuildRenderScene(**snapshot);
    if (renderSceneResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(renderSceneResult.Error()));
    }

    currentRenderScene = std::move(renderSceneResult.Value());
    ApplyMaterialInstancePreviewMaterialOverride(currentRenderScene);
    RebuildRenderSceneMaterialGroups(currentRenderScene);
    hasRenderScene = true;
    return true;
}

void RenderSystem::RenderLatestSnapshotOrLastGood()
{
    PROFILE_SCOPE("RenderSystem::RenderLatestSnapshotOrLastGood");
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem renderer backend is not set");
    }

    // Freeze the GT-owned CSM settings for the complete frame. All shadow
    // cascade/UBO code below reads CsmRuntime's frame snapshot, never the GT field.
    SnapshotFrameCsmSettings();

    // 调试 parity 命令在帧边界执行；内部 WaitIdle + 单次 submit。
    RunTerrainPageBakeParityIfRequested();

    const bool consumedSnapshot = ConsumeLatestSnapshotIntoRenderScene();
    if (consumedSnapshot)
    {
        if (!offlineMode) BuildResolvedRenderScene();
        if (!offlineMode && currentRenderScene.worldGeneration != initializedRenderWorldGeneration)
        {
            InitializeCurrentRenderSceneResources();
        }
        else if (!offlineMode && currentResolvedRenderScene.NeedsObjectResources())
        {
            // 同一 World 的新对象（Spawn、重新显示）也有独立模型 UBO/descriptor。
            // 只补缺失的逐对象包；已有 entry 的 Initialize 是幂等的，不重建图或环境，
            // 不覆盖 GPU 正在读取的旧包，仍由 World-local cache 随 epoch 退休。
            VL::RendererObjectResourceRegistry registry;
            registry.InitializeResolvedSceneResources(*rendererBackend, BuildRendererDescriptorContext(),
                currentRenderScene, currentResolvedRenderScene, VL::RendererResourceCache::GetInstance(),
                prerenderSkinEnabled);
        }
    }
    else if (!hasRenderScene)
    {
        throw std::runtime_error("RenderSystem has no RenderScene to render");
    }

    RecordAndSubmitCurrentRenderScene();
}

void RenderSystem::BuildResolvedRenderScene()
{
    const auto& resourceCache = VL::RendererResourceCache::GetInstance();
    // Hold the cache for the complete resolve: GT candidates/packages may swap
    // concurrently with RT, and the getters below must observe one stable set.
    auto frameLock = resourceCache.LockForFrame();
    for (auto& draw : currentRenderScene.drawPackets)
    {
        const auto instance = resourceCache.GetMaterialInstance(draw.materialInstance.key);
        if (instance)
            draw.material.key = instance->GetBaseMaterial().lock()->GetMaterialKey();
    }
    RebuildRenderSceneMaterialGroups(currentRenderScene);
    // 放置级（per-placement）对象必须在 resolve 之前拥有自己的 RenderableObject，
    // 否则 ResolvedRenderSceneBuilder 会因为 cache 缺键整帧失败。
    EnsurePlacementRenderables(currentRenderScene, VL::RendererResourceCache::GetInstance());
    PrunePlacementRenderables(currentRenderScene);
    auto resolvedSceneResult = resolvedRenderSceneBuilder.Build(currentRenderScene, resourceCache);
    if (resolvedSceneResult.IsFailure())
    {
        throw std::runtime_error(VL::FormatRuntimeError(resolvedSceneResult.Error()));
    }

    currentResolvedRenderScene = std::move(resolvedSceneResult.Value());
}

void RenderSystem::EnsurePlacementRenderables(
    const VL::RenderScene& renderScene, VL::RendererResourceCache& resourceCache)
{
    placementRuntime.EnsurePlacementRenderables(renderScene, resourceCache);
}

void RenderSystem::PrunePlacementRenderables(const VL::RenderScene& renderScene)
{
    placementRuntime.PrunePlacementRenderables(renderScene);
}

void RenderSystem::InitializeCurrentRenderSceneResources()
{
    frameResources.EnsureLightCapacity(currentRenderScene.lights.size(), *rendererBackend);
    environmentRuntime.Reset();
    environmentRuntime.RefreshDiagnostics();

    VL::RendererDescriptorContext descriptorContext = BuildRendererDescriptorContext();
    VL::RendererResourceCache& resourceCache = VL::RendererResourceCache::GetInstance();
    // Hold one frame-stable cache view across environment binding and
    // object-resource creation so GT candidates cannot swap mid-initialize.
    auto frameLock = resourceCache.LockForFrame();

    PrepareEnvironmentResources(); // 环境光照的资源准备

    RenderGraph::GetInstance().RefreshRuntimeDescriptors(*rendererBackend, descriptorContext);

    VL::RendererObjectResourceRegistry objectResourceRegistry;
    objectResourceRegistry.InitializeResolvedSceneResources(
        *rendererBackend,
        descriptorContext,
        currentRenderScene,
        currentResolvedRenderScene,
        resourceCache,
        prerenderSkinEnabled);

    initializedRenderWorldGeneration = currentRenderScene.worldGeneration;
}


void RenderSystem::RecordAndSubmitCurrentRenderScene()
{
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem renderer backend is not set");
    }
    VL::RenderFrameRecorderContext context{
        *rendererBackend,
        passRuntime,
        *this,
        currentRenderScene,
        currentResolvedRenderScene,
        gpuPassTimer,
        uiOverlayRenderer,
        uiRenderSnapshotQueue,
        currentUiRenderSnapshot,
        terrainFeedbackRuntime,
        hairRuntime,
        dynamicMeshUpdateQueue,
        offlineCoordinator.get(),
        offlineDisplay,
        offlineMode,
        offlineAov,
        offlineExposure,
        frameShadowCasterCullStats.data(),
        currentFrame,
        swapChainImageIndex,
        submittedCameraGeneration,
        submittedViewProjection,
        submittedFrameSerial,
        gpuPassStatsLoggingEnabled,
        shadowCasterCullStatsFramesSinceLog,
        pendingScreenshotPath,
        pendingScreenshotPathMutex,
        GetSceneRenderExtent(),
        [this] { AdvanceSceneWind(); },
        [this](vk::CommandBuffer& commandBuffer)
        {
            UpdateUBOGlobal(commandBuffer);
        },
        [this](vk::CommandBuffer& commandBuffer, uint32_t imageIndex)
        {
            RecordEnvironmentIbl(commandBuffer, imageIndex);
        },
        [this](vk::CommandBuffer& commandBuffer, uint32_t imageIndex)
        {
            RecordSkinDeform(commandBuffer, imageIndex);
        },
        [this] { return CaptureOfflineResult(); },
        [this] { LogShadowCasterCullStats(); },
        [this] { RefreshRenderSceneFromActiveWorld(); },
    };
    frameRecorder.RecordAndSubmitCurrentRenderScene(context);
}

void RenderSystem::RenderInitialize()
{
    InitializeFrameResources();
    ValidateFrameResourceDescriptors();
}

VL::RendererDescriptorContext RenderSystem::BuildRendererDescriptorContext() const
{
    return BuildRendererDescriptorContext(
        VL::RendererResourceCache::GetInstance(),
        RenderGraph::GetInstance(),
        GetLightBufferInfo());
}

VL::RendererDescriptorContext
RenderSystem::BuildRendererDescriptorContext(
    const VL::RendererResourceCache& resourceCache,
    RenderGraph& renderGraph,
    const std::vector<vk::DescriptorBufferInfo>&
        lightBufferInfos) const
{
    VL::RendererDescriptorContext descriptorContext;
    descriptorContext.globalUniformBufferInfos = &GetUBOGlobalBufferInfo();
    descriptorContext.lightBufferInfos =
        &lightBufferInfos;
    descriptorContext.resourceCache =
        &resourceCache;
    Renderpass* canonicalShadowPass =
        renderGraph.FindCanonicalShadowPass();
    if (canonicalShadowPass != nullptr)
    {
        std::shared_ptr<MaterialInstance> shadowMaterialInstance =
            canonicalShadowPass->materialInstance.lock();
        if (shadowMaterialInstance)
        {
            descriptorContext.commonOpaqueShadowMaterial =
                shadowMaterialInstance->GetBaseMaterial().lock();
        }
    }
    return descriptorContext;
}

// 风时间按固定 1/60 s 步长推进，步进量来自 EngineLoop 的 RuntimeClock 真实 delta。
// master 的 SpeedTree 风时钟是墙钟；当前分支若每渲染帧都加 1/60，在 240 FPS 下会比
// master 快 4 倍。这里保留固定步长（CWindStateMgr/SceneWindRuntime 的数值稳定），
// 只让“每秒推进多少步”跟随引擎真实时间。
void RenderSystem::AdvanceSceneWindTick()
{
    constexpr double kSceneWindTickSeconds = 1.0 / 60.0;
    // 一步同时推进场级解析（头发）和每 profile 的 CWindStateMgr（植被）。
    // 两个消费者必须看到同一个 currentWindTimeSeconds，否则阵风相位会分叉。
    const VL::ResolvedSceneWind resolvedWind =
        sceneWindRuntime.Advance(currentRenderScene.wind, currentWindTimeSeconds);
    currentRenderScene.windTimeSeconds = currentWindTimeSeconds;
    currentRenderScene.resolvedWind = resolvedWind;
    windResponseProfiles.AdvanceToSceneWind(
        currentWindTimeSeconds, currentRenderScene.wind);
    currentWindTimeSeconds += kSceneWindTickSeconds;
}

void RenderSystem::AdvanceSceneWind()
{
    constexpr double kSceneWindTickSeconds = 1.0 / 60.0;
    if (!windClockInitialized)
    {
        windClockInitialized = true;
        // 首帧只做 time=0 的目标采样；此时 GT 可能已经积累了一帧 delta，清零避免
        // 同一渲染帧里立刻再走一步，导致第一帧强度从目标值跳开。
        windPendingDeltaSeconds.store(0.0f, std::memory_order_relaxed);
        windTickAccumulator = 0.0;
        AdvanceSceneWindTick();
        return;
    }

    // exchange 而不是 load 后清零：GT 可能在读取后立刻追加下一帧 delta；
    // 原子交换保证每一份 GT delta 只被 RT 消费一次。
    const double elapsed = std::max(0.0, static_cast<double>(
        windPendingDeltaSeconds.exchange(0.0f, std::memory_order_relaxed)));
    // 调试断点/长卡顿后最多追 0.25 s（15 步），避免一次补几百步让 gust 连续爆发。
    windTickAccumulator = std::min(windTickAccumulator + elapsed, 0.25);
    // 一帧内可以补多步；严格按时间顺序推进，保持 CWindStateMgr 的 RNG/gust 状态机顺序。
    while (windTickAccumulator >= kSceneWindTickSeconds)
    {
        AdvanceSceneWindTick();
        windTickAccumulator -= kSceneWindTickSeconds;
    }
}

void RenderSystem::ShutdownRenderObject()
{
    // trace 可能还有 1..2 个 frame slot pending；这里显式等一次队列并写盘。
    // 调用方（EngineLoop::Shutdown）已停止渲染线程，且 backend 仍然有效。
    if (const auto& hairSimulation = RenderGraph::GetInstance().GetHairSimulation())
        hairSimulation->FinishTrace(true);
    offlineCoordinator.reset();
    offlineDisplay.Shutdown();
    materialInstancePreviewAdapter.Reset();
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->SetUiOverlayParticipant(nullptr);
    }
    uiOverlayRenderer.Shutdown();
    currentUiRenderSnapshot.reset();
    ShutdownFrameResources();
}

void RenderSystem::InitializeFrameResources()
{
    if (rendererBackend == nullptr)
    {
        throw std::runtime_error("RenderSystem cannot initialize frame resources without a renderer backend");
    }
    if (pipelineFactory == nullptr)
    {
        throw std::runtime_error("RenderSystem cannot initialize procedural sky IBL without a pipeline factory");
    }

    frameResources.Initialize(*rendererBackend);
    frameResources.EnsureLightCapacity(currentRenderScene.lights.size(), *rendererBackend);
    proceduralSkyCubeGenerator.Initialize(
        *pipelineFactory,
        *rendererBackend);
    environmentIblBaker.Initialize(
        *pipelineFactory,
        *rendererBackend,
        frameResources.GetGlobalUniformBufferInfos());
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->RegisterComputeParticipant(
            &proceduralSkyCubeGenerator);
        shaderReloadCoordinator->RegisterComputeParticipant(
            &skyShReloadParticipant);
        shaderReloadCoordinator->RegisterComputeParticipant(
            &prefilterReloadParticipant);
        shaderReloadCoordinator->RegisterComputeParticipant(&exposureHistogramReloadParticipant);
        shaderReloadCoordinator->RegisterComputeParticipant(&exposureAdaptReloadParticipant);
    }
    environmentRuntime.Initialize(*rendererBackend);
    // query pool 在首次采集时才创建；--gpu-stats 关闭时这里只记录能力快照。
    gpuPassTimer.Initialize(*rendererBackend, MAX_FRAMES_IN_FLIGHT);
}

void RenderSystem::ShutdownFrameResources()
{
    if (rendererBackend == nullptr)
    {
        return;
    }

    // 先销毁统计 query pool，再释放 swapchain 相关帧资源。
    gpuPassTimer.Shutdown(*rendererBackend);
    environmentRuntime.Shutdown(*rendererBackend);
    if (shaderReloadCoordinator != nullptr)
    {
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &proceduralSkyCubeGenerator);
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &skyShReloadParticipant);
        shaderReloadCoordinator->UnregisterComputeParticipant(
            &prefilterReloadParticipant);
        shaderReloadCoordinator->UnregisterComputeParticipant(&exposureHistogramReloadParticipant);
        shaderReloadCoordinator->UnregisterComputeParticipant(&exposureAdaptReloadParticipant);
    }
    environmentIblBaker.Shutdown(*rendererBackend);
    proceduralSkyCubeGenerator.Shutdown(*rendererBackend);
    environmentRuntime.ResetPendingWork();
    
    frameResources.Shutdown(*rendererBackend);
}

void RenderSystem::ValidateFrameResourceDescriptors()
{
    if (!frameResources.IsInitialized())
    {
        throw std::runtime_error("RenderSystem cannot expose frame resource descriptors before frame resources are initialized");
    }
}

std::shared_ptr<Texture> RenderSystem::GetActiveEnvironmentCube()
{
    return GetEnvironmentCube(
        currentRenderScene,
        VL::RendererResourceCache::GetInstance());
}

std::shared_ptr<Texture> RenderSystem::GetEnvironmentCube(
    const VL::RenderScene& renderScene,
    const VL::RendererResourceCache& resourceCache) const
{
    return environmentRuntime.GetEnvironmentCube(
        renderScene,
        resourceCache,
        proceduralSkyCubeGenerator);
}

void RenderSystem::PrepareEnvironmentResources()
{
    PrepareEnvironmentResources(
        currentRenderScene,
        VL::RendererResourceCache::GetInstance());
}

void RenderSystem::PrepareEnvironmentResources(
    const VL::RenderScene& renderScene,
    VL::RendererResourceCache& resourceCache) const
{
    environmentRuntime.PrepareResources(
        renderScene,
        resourceCache,
        proceduralSkyCubeGenerator,
        environmentIblBaker);
}


void RenderSystem::RecordSkinDeform(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex)
{
    if (!prerenderSkinEnabled)
        return;
    const auto& skinDeform = RenderGraph::GetInstance().GetSkinDeform();
    if (!skinDeform)
        return;

    // 预蒙皮 compute 读的 palette 必须在本帧任何 draw 之前写入当前 image 槽。
    // draw 路径在预蒙皮开启时不再重复上传，因此这里覆盖所有蒙皮 draw。
    std::unordered_set<VL::RendererObjectGpuResources*> uploaded;
    for (const auto& materialGroup : currentResolvedRenderScene.materialGroups)
    {
        for (const auto& instanceGroup : materialGroup.materialInstances)
        {
            for (const auto& draw : instanceGroup.draws)
            {
                if (!draw.objectResourceEntry)
                    continue;
                VL::RendererObjectGpuResources& resources =
                    draw.objectResourceEntry->GetResources();
                if (!resources.skinDeform.HasResources())
                    continue;
                if (!uploaded.insert(&resources).second)
                    continue;
                const auto& packet = currentRenderScene.drawPackets.at(draw.drawPacketIndex);
                if (!packet.skinPose)
                    continue;
                resources.submittedSkin.RetainIfSubmittedAt(submittedFrameSerial);
                frameResources.UpdateObjectSkinPalette(
                    swapchainImageIndex, resources, packet);
            }
        }
    }

    skinDeform->Record(commandBuffer, currentResolvedRenderScene, swapchainImageIndex);
}

void RenderSystem::RecordEnvironmentIbl(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex)
{
    environmentRuntime.RecordIbl(
        commandBuffer,
        swapchainImageIndex,
        currentRenderScene,
        VL::RendererResourceCache::GetInstance(),
        proceduralSkyCubeGenerator,
        environmentIblBaker);
}

void RenderSystem::RefreshEnvironmentUpdateDiagnostics()
{
    environmentRuntime.RefreshDiagnostics();
}

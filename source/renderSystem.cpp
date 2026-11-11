#include "renderSystem.h"
#include "render/preview/materialPreviewCommit.h"
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
        renderGraph.InitializeVolumetricFog(
            *pipelineFactory, *rendererBackend, descriptorContext);
        renderGraph.InitializeLightCulling(
            *pipelineFactory, *rendererBackend, descriptorContext);
        renderGraph.InitializeHairSimulation(*pipelineFactory, *rendererBackend);
        if (prerenderSkinEnabled)
            renderGraph.InitializeSkinDeform(*pipelineFactory, *rendererBackend);
        renderGraph.RenderInitialize(*rendererBackend, descriptorContext);
        InitializeCurrentRenderSceneResources();
        // ShutdownFrameResources 会释放粒子管线、默认纹理和发射器池；
        // resize 的同一事务内必须把它们恢复到新 render pass/swapchain 上，
        // 否则下一帧之前 backend 资源计数不稳定，旧 render pass 也可能被
        // 已销毁的粒子 pipeline 引用。
        particleRenderer.EnsureInitialized(
            *pipelineFactory,
            *rendererBackend,
            renderGraph);
        // resize 后 frameResources 已换新 buffer；light 发射器描述符必须在
        // 同一个事务里重新指向新输出，不能等下一帧。
        particleRenderer.SetParticleLightBuffers(
            frameResources.GetParticleLightBufferHandles());
        ConfigureParticleSceneLighting();
        particleRenderer.Synchronize(currentRenderScene);
        decalRenderer.EnsureInitialized(
            *pipelineFactory,
            *rendererBackend,
            renderGraph);
        decalRenderer.Synchronize(currentRenderScene);
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
    renderGraph.InitializeVolumetricFog(
        *pipelineFactory, *rendererBackend, descriptorContext);
    renderGraph.InitializeLightCulling(
        *pipelineFactory, *rendererBackend, descriptorContext);
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
    if (!offlineMode && rendererBackend != nullptr)
    {
        // 粒子管线没有注册成 shader reload participant；reload 提交后丢弃
        // 旧 artifact，下一次 Render 的 EnsureInitialized/Synchronize 重建。
        particleRenderer.InvalidatePipelines(*rendererBackend);
        decalRenderer.InvalidatePipelines(*rendererBackend);
    }
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
    candidateGraph.InitializeVolumetricFog(
        *pipelineFactory, *rendererBackend, descriptorContext);
    candidateGraph.InitializeLightCulling(
        *pipelineFactory, *rendererBackend, descriptorContext);
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

void RenderSystem::ApplyMaterialInstancePreviewMaterialOverride(
    VL::RenderScene& renderScene) const
{
    if (!hasMaterialInstancePreviewMaterialOverride ||
        materialInstancePreviewOverridePath.empty() ||
        materialInstancePreviewOverrideMaterialKey.empty())
    {
        return;
    }
    VL::ApplyMaterialOverrideToRenderScene(
        renderScene,
        materialInstancePreviewOverridePath,
        materialInstancePreviewOverrideMaterialKey);
}

void RenderSystem::RebuildRenderSceneMaterialGroups(
    VL::RenderScene& renderScene) const
{
    VL::RebuildRenderSceneMaterialGroupsImpl(renderScene);
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
    frameResources.UpdateLightBuffer(
        swapChainImageIndex, lights, animationTimeSeconds);
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
        buildDesc.simulationDeltaSeconds = pendingSimulationDeltaSeconds;
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

    if (!offlineMode)
    {
        // World 事务提交路径会直接设置 initializedRenderWorldGeneration，
        // 因此粒子同步不能只挂在 generation 变化的初始化分支上；这里按冻结
        // RenderScene 做幂等同步，新增/移除发射器才创建或销毁 GPU 池。
        particleRenderer.EnsureInitialized(
            *pipelineFactory,
            *rendererBackend,
            RenderGraph::GetInstance());
        // light 发射器复用 GPU 模拟池；emit compute 的输出使用固定 64 槽
        // per-image SSBO。buffer 必须在 Synchronize 创建发射器描述符前提供。
        particleRenderer.SetParticleLightBuffers(
            frameResources.GetParticleLightBufferHandles());
        ConfigureParticleSceneLighting();
        particleRenderer.Synchronize(currentRenderScene);
        // particle decal 同样消费 resolved assets（userParameters/Data
        // Channels）。必须先重建动态 decal 列表，再让 DecalRenderer
        // 按冻结列表同步纹理集；实例 SSBO 固定 64 槽，不做逐帧扩容。
        particleDecalRuntime.Synchronize(
            currentRenderScene,
            particleRenderer.GetResolvedSystemAssets(),
            particleRenderer.GetDecalGpuInstances());
        particleDecalRuntime.Update(
            currentRenderScene,
            VL::kMaxDecalInstances,
            particleRenderer.GetDecalGpuInstances());
        if (frameResources.EnsureLightCapacity(
                currentRenderScene.lights.size(),
                *rendererBackend))
        {
            // 仅场景静态灯使用这份 SSBO；GPU 粒子灯走独立固定容量缓冲，
            // 不会触发这里的扩容。异常容量增长时仍需先到 device idle。
            rendererBackend->WaitIdle();
            RenderGraph::GetInstance().RefreshRuntimeDescriptors(
                *rendererBackend,
                BuildRendererDescriptorContext());
        }
        decalRenderer.EnsureInitialized(
            *pipelineFactory,
            *rendererBackend,
            RenderGraph::GetInstance());
        // 贴图集按冻结 RenderScene 幂等同步：新增/移除 decal 或贴图路径
        // 变化时才创建/退休纹理，不能等到 pass 录制阶段再加载。
        decalRenderer.Synchronize(currentRenderScene);
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
        [this] { PrepareLocalShadowLights(); },
        [this] { ConfigureParticleSceneLighting(); },
        [this](vk::CommandBuffer& commandBuffer, uint32_t frameIndex)
        {
            RenderParticleSimulation(commandBuffer, frameIndex);
        },
        [this](vk::CommandBuffer& commandBuffer, const Renderpass& renderPass)
        {
            RecordParticleSoftDepthPass(commandBuffer, renderPass);
        },
        [this] { LogLocalShadowCasterStatistics(); },
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
    descriptorContext.particleLightBufferInfos =
        &frameResources.GetParticleLightBufferInfos();
    descriptorContext.lightGridBufferInfos =
        &frameResources.GetLightGridBufferInfos();
    descriptorContext.lightIndexBufferInfos =
        &frameResources.GetLightIndexBufferInfos();
    descriptorContext.lightGridParamBufferInfos =
        &frameResources.GetLightGridParamBufferInfos();
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
    // tile 聚类缓冲随主颜色目标尺寸重建；这里先按当前图取一次，
    // 运行时若图分辨率变化会在帧首再次校验。
    const vk::Extent2D sceneColorExtent =
        RenderGraph::GetInstance().GetSceneColorExtent();
    if (sceneColorExtent.width != 0 && sceneColorExtent.height != 0)
    {
        frameResources.EnsureLightCullingGrid(
            sceneColorExtent.width,
            sceneColorExtent.height,
            *rendererBackend);
    }
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
    particleGpuTimer.Initialize(*rendererBackend);
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
    particleGpuTimer.Shutdown(*rendererBackend);
    decalRenderer.Shutdown(*rendererBackend);
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
    particleRenderer.Shutdown(*rendererBackend);
    
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

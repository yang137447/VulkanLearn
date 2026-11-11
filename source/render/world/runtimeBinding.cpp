#include "renderSystem.h"

#include "core/runtimeResult.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/offline/offlineSceneAdapter.h"
#include "render/resource/rendererResourceCache.h"
#include "material.h"
#include "materialInstance.h"
#include "renderGraph.h"
#include "world/worldSnapshotBuilder.h"

#include <stdexcept>
#include <utility>
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

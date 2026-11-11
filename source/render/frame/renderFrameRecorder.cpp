#include "render/frame/renderFrameRecorder.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>
#include <vector>

#include "BaseStructs.h"
#include "commonFunction.h"
#include "passBarrier.h"
#include "profiler.h"
#include "renderGraph.h"
#include "material.h"
#include "materialInstance.h"
#include "renderSystem.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/uiOverlayRendererVulkan.h"
#include "vulkanDebug.h"
#include "hair/hairSimulationPass.h"
#include "render/skinning/skinDeformPass.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/pass/passRuntime.h"
#include "render/offline/offlineWorldAssets.h"
#include "render/offline/offlineDisplayExposure.h"
#include "render/backend/gpuPassTimer.h"
#include "render/backend/rendererFrameResources.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/camera/cameraExposure.h"
#include "render/frontend/renderScene.h"
#include "render/postprocess/postProcessControls.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/terrainPageBakeGpu.h"
#include "render/resource/terrainFeedbackGpu.h"
#include "render/resource/terrainLayerFeedbackGpu.h"
#include "render/resource/rendererTerrainLoader.h"
#include "render/shadow/csmRuntime.h"
#include "render/terrain/terrainFeedbackRuntime.h"
#include "render/hair/hairRuntime.h"
#include "renderableObject.h"

namespace VL
{

void RenderFrameRecorder::UpdateGlobalUBO(
    vk::CommandBuffer& commandBuffer,
    VL::RendererFrameResources& frameResources,
    uint32_t swapChainImageIndex,
    const VL::RenderScene& renderScene,
    bool hasRenderScene,
    const std::function<void()>& refreshRenderScene,
    VL::CsmRuntime& csmRuntime,
    uint32_t currentFrame)
{
    UpdateGlobalUBOWithProjectionTransform(
        commandBuffer,
        frameResources,
        swapChainImageIndex,
        renderScene,
        hasRenderScene,
        refreshRenderScene,
        csmRuntime,
        currentFrame,
        1.0f,
        1.0f,
        0.0f,
        0.0f);
}

void RenderFrameRecorder::UpdateGlobalUBOWithProjectionTransform(
    vk::CommandBuffer& commandBuffer,
    VL::RendererFrameResources& frameResources,
    uint32_t swapChainImageIndex,
    const VL::RenderScene& renderScene,
    bool hasRenderScene,
    const std::function<void()>& refreshRenderScene,
    VL::CsmRuntime& csmRuntime,
    uint32_t currentFrame,
    float projectionScaleX,
    float projectionScaleY,
    float projectionOffsetX,
    float projectionOffsetY)
{
    PROFILE_FUNCTION();
    if (!hasRenderScene)
    {
        refreshRenderScene();
    }

    UBOGlobal ubo;
    const VL::CameraSnapshot& camera = renderScene.camera;
    Eigen::Matrix4f projection = camera.projection;
    Eigen::Matrix4f previousViewProjection = camera.previousViewProjection;
    if (!std::isfinite(projectionScaleX) || projectionScaleX <= 0.0f ||
        !std::isfinite(projectionScaleY) || projectionScaleY <= 0.0f ||
        !std::isfinite(projectionOffsetX) ||
        !std::isfinite(projectionOffsetY))
    {
        projectionScaleX = 1.0f;
        projectionScaleY = 1.0f;
        projectionOffsetX = 0.0f;
        projectionOffsetY = 0.0f;
    }
    projection.row(0) =
        projectionScaleX * camera.projection.row(0) +
        projectionOffsetX * camera.projection.row(3);
    projection.row(1) =
        projectionScaleY * camera.projection.row(1) +
        projectionOffsetY * camera.projection.row(3);
    previousViewProjection.row(0) =
        projectionScaleX * camera.previousViewProjection.row(0) +
        projectionOffsetX * camera.previousViewProjection.row(3);
    previousViewProjection.row(1) =
        projectionScaleY * camera.previousViewProjection.row(1) +
        projectionOffsetY * camera.previousViewProjection.row(3);
    ubo.view = camera.view;
    ubo.projection = projection;
    ubo.invView = ubo.view.inverse();
    ubo.invProjection = ubo.projection.inverse();
    ubo.viewProjection = ubo.projection * ubo.view;
    ubo.invViewProjection = ubo.viewProjection.inverse();
    ubo.previousViewProjection = previousViewProjection;
    const VL::CsmSettings& frameSettings =
        csmRuntime.GetFrameSettings();
    if (frameSettings.castShadows)
    {
        if (!csmRuntime.GetFrameData().valid)
        {
            csmRuntime.BuildFrameData(renderScene, currentFrame, 1, 1);
        }
        const VL::ShadowCascadeFrameData& cascadeData =
            csmRuntime.GetFrameData();
        ubo.lightViewProj = cascadeData.lightViewProj;
        ubo.cascadeSplits = cascadeData.cascadeSplits;
        ubo.shadowBias = cascadeData.bias;
        ubo.csmParameters = cascadeData.csmParameters;
    }
    else
    {
        // Zero cascade splits is the GPU-side disabled flag consumed by
        // CalculateCsmShadow; keep every shadow field explicitly neutral.
        ubo.cascadeSplits = Eigen::Vector4f::Zero();
        ubo.shadowBias.fill(Eigen::Vector4f::Zero());
        ubo.csmParameters = Eigen::Vector4f::Zero();
        for (Eigen::Matrix4f& lightViewProj : ubo.lightViewProj)
        {
            lightViewProj = Eigen::Matrix4f::Identity();
        }
    }
    ubo.cameraPosition = camera.position;
    // environmentSH 由 GPU 在环境代际 commit 时广播，常规 CPU 上传会跳过该区间。
    ubo.debugViewMode = renderScene.debugViewMode;
    ubo.environmentIntensity = renderScene.environment.intensity;
    ubo.environmentType = renderScene.environment.type == VL::EnvironmentType::ProceduralSky ? 1 : 0;
    ubo.skyParameters = renderScene.environment.skyParameters;
    ubo.editorGridHeight = renderScene.editorGridHeight;
    const float cameraExposureScale = VL::ExposureScale(camera.exposure);
    const bool autoExposureEnabled =
        camera.exposure.mode == VL::ExposureMode::Auto;
    std::string exposureMessage;
    (void)VL::SetPassVectorComponent(
        "toneMapping",
        "u_toneMappingParams",
        0,
        cameraExposureScale,
        "Tone mapping pass material instance is expired.",
        exposureMessage);
    (void)VL::SetPassVector(
        "toneMapping",
        "u_exposureSettings",
        Eigen::Vector4f(autoExposureEnabled ? 1.0f : 0.0f, 0, 0, 0),
        exposureMessage);
    // Bloom 阈值属于曝光后空间，与最终合成共用同一冻结相机，输出仍保留场景线性单位。
    (void)VL::SetPassVectorComponent(
        "bloomPrefilter",
        "u_bloomPrefilterParams",
        3,
        cameraExposureScale,
        "Bloom prefilter material instance is expired.",
        exposureMessage);
    (void)VL::SetPassVector(
        "bloomPrefilter",
        "u_exposureSettings",
        Eigen::Vector4f(autoExposureEnabled ? 1.0f : 0.0f, 0, 0, 0),
        exposureMessage);
    frameResources.UpdateGlobalUniformBuffer(commandBuffer, swapChainImageIndex, ubo);
}

void RenderFrameRecorder::UpdateExposureFrameTiming(
    VL::GpuPassTimer& gpuPassTimer)
{
    const auto now = std::chrono::steady_clock::now();
    if (!exposureClockInitialized)
    {
        exposureLastFrameTime = now;
        exposureClockInitialized = true;
        exposureDeltaSeconds = 1.0f / 60.0f;
        // 首帧也发布 CPU 时长，避免 Performance 页在第一个读回窗口前空着 FPS。
        gpuPassTimer.SetCpuFrameTiming(exposureDeltaSeconds);
        return;
    }

    const double elapsed = std::chrono::duration<double>(
        now - exposureLastFrameTime).count();
    exposureLastFrameTime = now;
    // 眼适应不应因断点、窗口拖拽或首帧长暂停瞬间跳到目标值。
    exposureDeltaSeconds = static_cast<float>(
        std::clamp(elapsed, 1.0 / 240.0, 0.1));
    // Performance 通道复用这一个 frame delta，不另建帧率时钟；
    // 因此它和自动曝光看到的 CPU 帧间隔完全一致（含同样的 1/240..0.1 s 收敛区间）。
    gpuPassTimer.SetCpuFrameTiming(exposureDeltaSeconds);
}

void RenderFrameRecorder::UpdateShadowGlobalUBO(
    vk::CommandBuffer& commandBuffer,
    VL::RendererFrameResources& frameResources,
    uint32_t swapChainImageIndex,
    const VL::RenderScene& renderScene,
    bool hasRenderScene,
    const std::function<void()>& refreshRenderScene,
    VL::CsmRuntime& csmRuntime,
    uint32_t currentFrame,
    uint32_t passSizeWidth,
    uint32_t passSizeHeight,
    uint32_t cascadeIndex)
{
    PROFILE_FUNCTION();
    if (!hasRenderScene)
    {
        refreshRenderScene();
    }

    csmRuntime.BuildFrameData(renderScene, currentFrame, passSizeWidth, passSizeHeight);
    const uint32_t activeCascadeIndex =
        std::min(
            cascadeIndex,
            csmRuntime.GetFrameSettings().cascadeCount - 1);

    UBOGlobal ubo;
    const VL::ShadowCascadeFrameData& cascadeData =
        csmRuntime.GetFrameData();
    ubo.view = cascadeData.view[activeCascadeIndex];
    ubo.projection = cascadeData.projection[activeCascadeIndex];
    ubo.invView = ubo.view.inverse();
    ubo.invProjection = ubo.projection.inverse();
    ubo.viewProjection = ubo.projection * ubo.view;
    ubo.invViewProjection = ubo.viewProjection.inverse();
    ubo.previousViewProjection = ubo.viewProjection;
    ubo.lightViewProj = cascadeData.lightViewProj;
    ubo.cascadeSplits = cascadeData.cascadeSplits;
    ubo.shadowBias = cascadeData.bias;
    ubo.csmParameters = cascadeData.csmParameters;
    // environmentSH 由 GPU 在环境代际 commit 时广播，常规 CPU 上传会跳过该区间。
    ubo.debugViewMode = renderScene.debugViewMode;
    ubo.environmentIntensity = renderScene.environment.intensity;
    ubo.environmentType = renderScene.environment.type == VL::EnvironmentType::ProceduralSky ? 1 : 0;
    ubo.skyParameters = renderScene.environment.skyParameters;
    ubo.editorGridHeight = renderScene.editorGridHeight;
    {
        Eigen::Matrix3f rotT = ubo.view.block<3, 3>(0, 0);
        Eigen::Vector3f trans = ubo.view.block<3, 1>(0, 3);
        ubo.cameraPosition = -(rotT.transpose() * trans);
    }

    frameResources.UpdateGlobalUniformBuffer(commandBuffer, swapChainImageIndex, ubo);
}

void RenderFrameRecorder::ApplyQueuedDynamicMeshUpdates(
    VL::DynamicMeshUpdateQueue& queue,
    const VL::RenderScene& renderScene,
    const VL::ResolvedRenderScene& resolvedScene,
    uint32_t frameSlot)
{
    auto updates = queue.Drain();
    if (updates.empty())
        return;

    // Resolved draws own strong references to their RenderableObject, so a
    // lookup through this frame's resolved scene cannot dangle even if GT
    // removes the cache entry concurrently.
    std::unordered_map<std::string, std::shared_ptr<RenderableObject>>
        renderablesByMeshKey;
    for (const VL::ResolvedMaterialGroup& materialGroup :
         resolvedScene.materialGroups)
    {
        for (const VL::ResolvedMaterialInstanceGroup& instanceGroup :
             materialGroup.materialInstances)
        {
            for (const VL::ResolvedDrawPacket& draw : instanceGroup.draws)
            {
                if (draw.drawPacketIndex >=
                        renderScene.drawPackets.size() ||
                    !draw.renderableObject)
                {
                    continue;
                }
                const std::string& meshKey =
                    renderScene.drawPackets[draw.drawPacketIndex]
                        .mesh.key;
                if (meshKey.empty()) continue;
                renderablesByMeshKey.emplace(meshKey, draw.renderableObject);
            }
        }
    }

    for (auto& [meshKey, update] : updates)
    {
        const auto found = renderablesByMeshKey.find(meshKey);
        if (found == renderablesByMeshKey.end() || !found->second)
        {
            // A GT tick can queue content before its snapshot reaches RT.
            // Keep it for a couple of frames; a mesh key that never appears is
            // dropped with one diagnostic.
            const auto attempts = ++missingDynamicMeshAttempts[meshKey];
            if (attempts <= 2)
            {
                queue.RequeueIfAbsent(
                    meshKey, std::move(update));
                continue;
            }
            missingDynamicMeshAttempts.erase(meshKey);
            if (reportedMissingDynamicMeshKeys.insert(meshKey).second)
            {
                std::cerr
                    << "[DynamicMesh] Dropping update for missing mesh key: "
                    << meshKey << std::endl;
            }
            continue;
        }
        missingDynamicMeshAttempts.erase(meshKey);
        try
        {
            if (update.replacesIndices)
            {
                found->second->UpdateDynamicMesh(
                    update.vertices, update.indices, frameSlot);
            }
            else
            {
                found->second->UpdateVertices(update.vertices, frameSlot);
            }
        }
        catch (const std::exception& error)
        {
            if (reportedMissingDynamicMeshKeys.insert(
                    meshKey + "#error").second)
            {
                std::cerr
                    << "[DynamicMesh] Update failed for mesh key '"
                    << meshKey << "': " << error.what() << std::endl;
            }
        }
    }
}

void RenderFrameRecorder::RecordAndSubmitCurrentRenderScene(
    RenderFrameRecorderContext& ctx)
{
    const RenderGraph& renderGraph = RenderGraph::GetInstance();

    if (ctx.uiSnapshotQueue != nullptr)
    {
        std::shared_ptr<const VL::UiRenderSnapshot> latestUiSnapshot =
            ctx.uiSnapshotQueue->ConsumeLatest();
        if (latestUiSnapshot != nullptr)
        {
            ctx.currentUiSnapshot = std::move(latestUiSnapshot);
            ctx.uiOverlayRenderer.SynchronizeTextures(*ctx.currentUiSnapshot);
        }
    }
    // 统计 query pool 在 backend BeginFrame 之前创建：此时本帧命令缓冲尚未开始录制。
    // 未开启采集时这里只做两次原子读取，不创建任何 Vulkan 对象。
    if (!ctx.offlineMode)
    {
        ctx.gpuTimer.PrepareFrame(ctx.currentFrame % MAX_FRAMES_IN_FLIGHT);
    }
    // Backend owns swapchain frame synchronization. RenderSystem only records
    // pass work into the command buffer it receives for this image.
    VL::RendererFrameContext frameContext = ctx.backend.BeginFrame(ctx.currentFrame);
    if (!frameContext.ready)
    {
        // acquire 失败没有提交；保留帧号和截图请求，等待 GT 重建 swapchain。
        return;
    }
    // 相机历史同样绑定真实提交，不能使用可能未被渲染的 GT 快照历史。
    ctx.renderScene.camera.previousViewProjection =
        ctx.submittedViewProjection && ctx.submittedCameraGeneration == ctx.renderScene.worldGeneration
        ? *ctx.submittedViewProjection : ctx.renderScene.camera.viewProjection;
    ctx.uiOverlayRenderer.CollectRetiredTextures();
    // 每帧重新累计：诊断输出只描述刚刚录制完的这一帧。
    *ctx.frameShadowCasterCullStats = {};
    uint32_t frameIndex = frameContext.frameIndex;
    ctx.swapChainImageIndex = frameContext.swapchainImageIndex;
    // 当前 image 已经 acquire；此时才能安全重写它的对象描述符集。
    ctx.passServices.RefreshMaterialDescriptorsForImage(ctx.swapChainImageIndex);
    // GT publishes dynamic mesh content instead of writing host-visible buffers.
    // Apply it to this image's slot before any pass records a draw.
    ApplyQueuedDynamicMeshUpdates(
        ctx.dynamicMeshQueue,
        ctx.renderScene,
        ctx.resolvedScene,
        ctx.swapChainImageIndex % RenderableObject::DynamicMeshFrameSlotCount);
    // ghost 权重按 swapchain 槽写入；必须在拿到本帧 slot 之后更新，避免覆盖
    // 另一 frame-in-flight 仍在读取的 buffer。
    // 草稿 binding 先于 ghost/选中状态应用，保证本帧 shader 读到同一份组号表。
    ctx.hairRuntime.ApplyTressfxGroupBindingPreviewRequests(ctx.renderScene, ctx.resolvedScene);
    ctx.hairRuntime.ApplyTressfxBrushPreviewRequests(ctx.renderScene, ctx.resolvedScene, ctx.swapChainImageIndex);
    vk::CommandBuffer commandBuffer = frameContext.commandBuffer;
    // 每帧固定同一个 World-local streaming state；不要让世界交换发生在
    // BeginFrame 消费和帧末回读之间时出现两份 ring/page table。
    const std::shared_ptr<VL::TerrainStreamingState> terrainFeedbackStreaming =
        VL::RendererResourceCache::GetInstance().GetTerrainStreaming();
    // 本帧提交 epoch 必须与 SubmitFrame 记录的 epoch 相同（ctx.currentFrame + 1），
    // feedback frameTag 也由它推导。先把 GPU 上下文回灌给 page manager，
    // 新驻留页才能以“本帧提交”而不是过期 feedback epoch 起算安全窗。
    const uint64_t feedbackSubmittedEpoch =
        static_cast<uint64_t>(ctx.currentFrame) + 1;
    // Console 的预算命令只写 atomic；这里在渲染线程、任何驻留变更之前消费，
    // 保证本帧 feedback/淘汰/上传都看到同一个 residentCapacity。
    if (terrainFeedbackStreaming)
    {
        const uint32_t requestedCapacity =
            terrainFeedbackStreaming->ConsumeRequestedResidentCapacity();
        if (requestedCapacity != 0)
        {
            // 降预算也必须遵守 GPU 安全窗：窗口内的可见页暂时允许超预算，
            // 不能为了立刻回到预算而淘汰本帧/上一帧仍在采样的页。
            terrainFeedbackStreaming->pageManager.SetResidentCapacityAtEpoch(
                requestedCapacity,
                frameContext.completedEpoch);
        }
        else if (terrainFeedbackStreaming->pageManager.ResidentCount() >
                 terrainFeedbackStreaming->pageManager.ResidentCapacity())
        {
            // 上一次降预算若因安全窗暂时超预算，后续帧继续尝试收敛；
            // 没有安全候选时该方法仍是 no-op，不会形成死循环。
            terrainFeedbackStreaming->pageManager.SetResidentCapacityAtEpoch(
                terrainFeedbackStreaming->pageManager.ResidentCapacity(),
                frameContext.completedEpoch);
        }

        // L4a 层页预算/命令：与覆盖页相同，只在渲染线程消费 atomic。
        const uint32_t requestedLayerCapacity =
            terrainFeedbackStreaming->requestedLayerResidentCapacity.exchange(
                0, std::memory_order_relaxed);
        if (requestedLayerCapacity != 0)
        {
            terrainFeedbackStreaming->layerPageManager
                .SetResidentCapacityAtEpoch(
                    requestedLayerCapacity, frameContext.completedEpoch);
        }
        if (terrainFeedbackStreaming->requestedLayerPreload.exchange(
                false, std::memory_order_relaxed))
        {
            VL::RequestTerrainLayerPreload(*terrainFeedbackStreaming);
        }
    }
    // epoch 上下文必须先于 ApplyFeedback：ApplyFeedback 内新驻留的页要用
    // “本帧即将提交”的 epoch 作为安全窗起点，而不是上一帧已消费的旧值。
    if (terrainFeedbackStreaming)
    {
        // L4e：相机位移只在本帧提交时推进，确保预取方向与真正渲染的帧一致。
        ctx.terrainRuntime.UpdateTerrainCameraPrefetch(
            terrainFeedbackStreaming,
            ctx.renderScene.camera.position,
            ctx.renderScene.camera.forward);
        // console 的强制 mip 在 feedback 消费之前写进材质，feedback 与
        // M_terrain 才会在同帧使用同一个目标 mip。
        ctx.terrainRuntime.ApplyTerrainLayerForceMip(terrainFeedbackStreaming);
        terrainFeedbackStreaming->pageManager.SetGpuEpochContext(
            feedbackSubmittedEpoch,
            terrainFeedbackStreaming->lastConsumedFeedbackEpoch,
            ctx.backend.GetFrameFenceCount());
        // 层页页表/atlas 的物理槽位复用也走同一安全窗；feedback 命中与
        // RequestPageAtEpoch 都在本函数之后发生。
        terrainFeedbackStreaming->layerPageManager.SetGpuEpochContext(
            feedbackSubmittedEpoch,
            terrainFeedbackStreaming->lastConsumedFeedbackEpoch,
            ctx.backend.GetFrameFenceCount());
    }
    // P3：消费已完成回读 -> ApplyFeedback（命中推进 epoch、miss 入请求队列）。
    ctx.terrainRuntime.ConsumeTerrainFeedback(
        terrainFeedbackStreaming,
        frameContext.completedEpoch,
        frameIndex);
    if (terrainFeedbackStreaming)
    {
        // Consume 可能推进 lastConsumedFeedbackEpoch；再次写回上下文，使
        // page manager 的统计/加载期回退值与本帧实际消费进度一致。
        terrainFeedbackStreaming->pageManager.SetGpuEpochContext(
            feedbackSubmittedEpoch,
            terrainFeedbackStreaming->lastConsumedFeedbackEpoch,
            ctx.backend.GetFrameFenceCount());
        terrainFeedbackStreaming->layerPageManager.SetGpuEpochContext(
            feedbackSubmittedEpoch,
            terrainFeedbackStreaming->lastConsumedFeedbackEpoch,
            ctx.backend.GetFrameFenceCount());
    }
    const uint8_t feedbackFrameTag =
        VL::TerrainFeedbackFrameTagForEpoch(feedbackSubmittedEpoch);
    // 本帧上传事务：只有 SubmitFrame 成功后才 Commit；任何提前返回/异常
    // 都会在析构里 Abort，保留 dirty 与 pin，避免 GPU 与 CPU 内容错位。
    VL::TerrainUploadTransaction terrainUploadTransaction;
    VL::TerrainLayerUploadTransaction terrainLayerUploadTransaction;
    uint32_t terrainUploadPageCount = 0;
    uint32_t terrainGpuBakeJobCount = 0;
    const bool renderToViewport = ctx.currentUiSnapshot && ctx.currentUiSnapshot->sceneViewportEnabled;
    const RenderResource* viewportOutput = renderToViewport
        ? renderGraph.GetSceneViewportOutput(ctx.swapChainImageIndex) : nullptr;
    if (renderToViewport && viewportOutput == nullptr)
        throw std::runtime_error("Editor scene viewport has no graph-owned output target.");
    // 自动曝光按帧推进：它读上一帧的直方图并产出本帧的 EV，因此必须在录制之前更新。
    UpdateExposureFrameTiming(ctx.gpuTimer);

    {
        VulkanDebug::ScopedRegion frameRegion(
            commandBuffer,
            "Frame:" + std::to_string(frameIndex) + " Image:" + std::to_string(ctx.swapChainImageIndex),
            VulkanDebug::DebugCategory::eDefault);

        // Tick every tree species once so shadow and geometry consume the same
        // per-object wind state for the complete frame.
        if (ctx.offlineMode)
        {
            const auto result = ctx.captureOfflineResult();
            const auto preview = ctx.offlineCoordinator
                ? ctx.offlineCoordinator->CapturePreview() : nullptr;
            // RT 只消费冻结的完整 film 或视口专用部分预览；导出接口不见预览。
            ctx.offlineDisplay.Record(commandBuffer, ctx.swapChainImageIndex,
                result ? result->film : nullptr, preview, ctx.offlineAov,
                // 曝光取 master 的解析结果（手动/Auto 两模式），视口三个参数保留我方的：
                // 编辑器视口输出与 graph 代数是本分支引入的，master 侧没有。
                ResolveOfflineDisplayExposure(
                    result.get(),
                    ctx.offlineExposure,
                    ctx.renderScene.camera),
                viewportOutput, renderGraph.GetOwnerGeneration(), ctx.renderExtent);
        }
        else
        {
        // 读回发生在这里：backend BeginFrame 已等待本 frame slot 的 fence，上一批查询必然就绪。
        // 随后在命令缓冲里重置该槽位的查询范围，早于本帧任何被计时命令。
        ctx.gpuTimer.BeginFrame(commandBuffer, frameIndex);
        ctx.advanceSceneWind();
        // Procedural sky IBL needs the latest camera and sky parameters in global UBO,
        // but environmentSH is GPU-owned and must not be overwritten by this upload.
        ctx.updateGlobalUBO(commandBuffer);
        // Generate the procedural sky cubemap and write environmentSH before graphics
        // passes read global lighting data.
        ctx.recordEnvironmentIbl(commandBuffer, ctx.swapChainImageIndex);
        // 描边法线平滑是一次性 compute：必须在本帧任意图形 pass 读取顶点缓冲之前
        // 完成；这里已过 BeginFrame 的 image fence，且透明 pass 尚未开始。
        ctx.passServices.RecordOutlineNormalSmoothing(
            commandBuffer, ctx.swapChainImageIndex);
        // guide 模拟必须在任何 pass 读取 current positions 之前完成本帧 dispatch。
        // 只有本帧确实在采集时才做 HasWork 扫描；关闭 --gpu-stats 且未执行 gpustats
        // 时，统计路径对每帧的正常渲染只增加一次原子读取和一次分支。
        const auto& hairSimulation = renderGraph.GetHairSimulation();
        const bool hairWillRecord =
            ctx.gpuTimer.IsFrameActive() &&
            hairSimulation &&
            hairSimulation->HasWork(ctx.resolvedScene, ctx.renderScene);
        const bool hairTimed = hairWillRecord &&
            ctx.gpuTimer.BeginPass(commandBuffer, "hairSimulation");
        ctx.hairRuntime.RecordHairSimulation(
            commandBuffer, frameIndex, ctx.resolvedScene, ctx.renderScene);
        if (hairWillRecord)
        {
            ctx.gpuTimer.EndPass(commandBuffer, hairTimed);
        }

        // GPU 预蒙皮必须早于任何 Shadow/Geometry pass：本帧 palette 先在 CPU 侧
        // 写入 image 槽，compute 再一次性生成顶点结果，后续顶点着色器只读。
        const auto& skinDeform = renderGraph.GetSkinDeform();
        const bool skinDeformWillRecord =
            ctx.gpuTimer.IsFrameActive() &&
            skinDeform &&
            skinDeform->HasWork(ctx.resolvedScene);
        const bool skinDeformTimed = skinDeformWillRecord &&
            ctx.gpuTimer.BeginPass(commandBuffer, "skinnedDeform");
        ctx.recordSkinDeform(commandBuffer, ctx.swapChainImageIndex);
        if (skinDeformWillRecord)
        {
            ctx.gpuTimer.EndPass(commandBuffer, skinDeformTimed);
        }

        // scene pass 之前提交本帧地形页：Build -> 打包进 staging ring ->
        // 同一个主帧 command buffer 录制。剩余 dirty 页下一帧继续。
        if (terrainFeedbackStreaming &&
            terrainFeedbackStreaming->uploadRing)
        {
            VL::TerrainUploadBatch uploadBatch =
                terrainFeedbackStreaming->pageManager.BuildUploadBatch(
                    terrainFeedbackStreaming->maxUploadPagesPerFrame,
                    VL::TerrainPageBakeGpu::MaxJobsPerFrame);
            if (!uploadBatch.Empty())
            {
                try
                {
                    const VL::TerrainUploadStagingOffsets offsets =
                        terrainFeedbackStreaming->uploadRing->Write(
                            frameIndex,
                            uploadBatch);
                    // compute 先烘焙 job 对应的物理槽位；buffer→image copy
                    // 与页表上传仍在同一个主帧 command buffer、scene pass 之前。
                    if (terrainFeedbackStreaming->pageBake &&
                        !uploadBatch.gpuBakeJobs.empty())
                    {
                        terrainFeedbackStreaming->pageBake->Record(
                            commandBuffer,
                            frameIndex,
                            uploadBatch.gpuBakeJobs,
                            terrainFeedbackStreaming->pageResources);
                    }
                    VL::RecordTerrainPageUploads(
                        commandBuffer,
                        terrainFeedbackStreaming->pageResources,
                        uploadBatch,
                        terrainFeedbackStreaming->uploadRing->Buffer(frameIndex),
                        offsets);
                    terrainUploadPageCount =
                        static_cast<uint32_t>(
                            uploadBatch.atlasUploads.size() +
                            uploadBatch.gpuBakeJobs.size());
                    terrainGpuBakeJobCount =
                        static_cast<uint32_t>(uploadBatch.gpuBakeJobs.size());
                    terrainUploadTransaction.Begin(
                        terrainFeedbackStreaming,
                        std::move(uploadBatch));
                }
                catch (...)
                {
                    // 录制失败时 batch 仍持有 pin；必须回滚，不能留给下一帧。
                    terrainFeedbackStreaming->pageManager.AbortUploadBatch(
                        uploadBatch);
                    throw;
                }
            }
        }

        // L4a：层页上传与覆盖页上传同一时机（scene pass 之前）。页表 texel
        // 走 staging ring；albedo/normal/pbr atlas 是 GPU→GPU copy，不需要
        // staging。剩余 dirty 页下一帧继续，Commit 在 SubmitFrame 之后。
        if (terrainFeedbackStreaming &&
            terrainFeedbackStreaming->layerVTRequested &&
            terrainFeedbackStreaming->layerUploadRing)
        {
            VL::TerrainLayerUploadBatch layerBatch =
                terrainFeedbackStreaming->layerPageManager.BuildUploadBatch(
                    terrainFeedbackStreaming->maxLayerPageUploadsPerFrame);
            if (!layerBatch.Empty())
            {
                try
                {
                    const vk::DeviceSize stagingOffset =
                        terrainFeedbackStreaming->layerUploadRing->Write(
                            frameIndex, layerBatch);
                    VL::RecordTerrainLayerPageUploads(
                        commandBuffer,
                        terrainFeedbackStreaming->layerPageManager,
                        terrainFeedbackStreaming->layerResources,
                        terrainFeedbackStreaming->layerSources,
                        layerBatch,
                        terrainFeedbackStreaming->layerUploadRing->Buffer(
                            frameIndex),
                        stagingOffset);
                    terrainLayerUploadTransaction.Begin(
                        terrainFeedbackStreaming,
                        std::move(layerBatch));
                }
                catch (...)
                {
                    // 录制失败时 batch 仍持有 pin；必须回滚，不能留给下一帧。
                    terrainFeedbackStreaming->layerPageManager
                        .AbortUploadBatch(layerBatch);
                    throw;
                }
            }
        }

        // feedback pass 的 u_frameTag 必须与下面回读登记的 epoch tag 一致。
        ctx.terrainRuntime.PrepareTerrainFeedbackPass(
            feedbackFrameTag, ctx.renderExtent);

        const auto& renderPassOrdered = renderGraph.GetRenderpassesOrdered();
        for (size_t passIndex = 0; passIndex < renderPassOrdered.size(); ++passIndex)
        {
            const auto& renderPassName = renderPassOrdered[passIndex];
            std::string renderPassScopeName = "RenderPass:" + renderPassName;
            PROFILE_SCOPE(renderPassScopeName.c_str());
            const auto& renderPass = renderGraph.GetRenderpasses().at(renderPassName);
            if (ctx.renderScene.environment.type == VL::EnvironmentType::None)
            {
                const auto instance = renderPass.materialInstance.lock();
                const auto material = instance ? instance->GetBaseMaterial().lock() : nullptr;
                if (material && material->GetShaderVariantKey().shaderName == "pass/sky")
                    continue;
            }

            // 参考网格是编辑器视口的显示辅助：视口不可见（运行页面、无 UI、资源编辑器
            // 全屏）时整个 pass 不录制，运行时画面与截图保持与接入前一致。
            if (renderPass.type == VL::RenderGraphPassType::EditorGrid &&
                (!renderToViewport ||
                    !ctx.passServices.ShouldRecordEditorGridPass()))
            {
                continue;
            }

            // pass 本身可能开启 render pass 实例，因此 timestamp 必须在实例外 bracket：
            // BeginPass 在 ScopedRegion 之前写 begin timestamp，EndPass 在录制完成后写
            // end timestamp；两端都在 pass 实例外，因此不受 render pass 内部阶段限制。
            const bool passTimed =
                ctx.gpuTimer.BeginPass(commandBuffer, renderPassName);

            VulkanDebug::ScopedRegion passRegion(
                commandBuffer,
                renderPassName,
                VulkanDebug::DebugCategory::ePass);

            if (renderPass.type == VL::RenderGraphPassType::AutoExposure)
            {
                PassBarrier::PrepareForPass(commandBuffer, renderGraph, passIndex, ctx.swapChainImageIndex);
                renderGraph.GetAutoExposure()->Record(commandBuffer, ctx.swapChainImageIndex,
                    ctx.renderScene.camera.exposure,
                    ExposureDeltaSeconds());
                ctx.gpuTimer.EndPass(commandBuffer, passTimed);
                continue;
            }
            VL::PassRuntimeContext passContext{
                commandBuffer,
                renderPass,
                renderGraph,
                passIndex,
                ctx.swapChainImageIndex,
                ctx.renderScene,
                ctx.resolvedScene,
                ctx.passServices,
                renderToViewport
            };
            // 阴影 pass 把 before/after draw 数写回这里；非阴影 pass 不接收统计指针。
            VL::ShadowCasterCullStats shadowCasterCullStats;
            shadowCasterCullStats.cascadeIndex = renderPass.shadowCascadeIndex;
            if (renderPass.type == VL::RenderGraphPassType::Shadow)
            {
                passContext.shadowCasterCullStats = &shadowCasterCullStats;
            }
            ctx.passRuntime.RecordPass(renderPassName, passContext);
            if (renderPass.type == VL::RenderGraphPassType::Shadow &&
                shadowCasterCullStats.cascadeIndex <
                    VL::CsmSettings::MaxCascadeCount)
            {
                ctx.frameShadowCasterCullStats[shadowCasterCullStats.cascadeIndex] =
                    shadowCasterCullStats;
            }
            ctx.gpuTimer.EndPass(commandBuffer, passTimed);
        }

        // 与 GPU 表同周期（120 帧）输出；只有 --gpu-stats 打开时才打印。
        if (ctx.gpuPassStatsLoggingEnabled)
        {
            ++ctx.shadowCasterCullStatsFramesSinceLog;
            if (ctx.shadowCasterCullStatsFramesSinceLog >=
                VL::kGpuPassTimingWindowFrames)
            {
                ctx.shadowCasterCullStatsFramesSinceLog = 0;
                ctx.logShadowCasterCullStats();
            }
        }
        }

        if (ctx.uiOverlayRenderer.IsInitialized() && ctx.currentUiSnapshot != nullptr)
        {
            if (viewportOutput != nullptr)
            {
                // 场景 attachment 写入先完成，RmlUi 图像再采样；布局和句柄只在 RT 消费。
                vk::ImageMemoryBarrier barrier;
                barrier.setImage(viewportOutput->image)
                    .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1})
                    .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                    .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                    .setOldLayout(vk::ImageLayout::eColorAttachmentOptimal)
                    .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
                    .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
                    .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
                commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                    vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, barrier);
            }
            VulkanDebug::ScopedRegion uiRegion(
                commandBuffer,
                "UI Overlay",
                VulkanDebug::DebugCategory::eDefault);
            const RenderResource* debugDepth = nullptr;
            if (!ctx.offlineMode && ctx.currentUiSnapshot->debugDepthTest)
                debugDepth = PassBarrier::BeginSceneDepthRead(commandBuffer, renderGraph, ctx.swapChainImageIndex);
            ctx.uiOverlayRenderer.Record(
                commandBuffer,
                ctx.swapChainImageIndex,
                *ctx.currentUiSnapshot,
                viewportOutput ? viewportOutput->imageView : vk::ImageView{},
                viewportOutput ? viewportOutput->sampler : vk::Sampler{},
                debugDepth ? debugDepth->GetShaderDescriptorView() : vk::ImageView{},
                debugDepth ? debugDepth->sampler : vk::Sampler{});
            if (debugDepth) PassBarrier::EndSceneDepthRead(commandBuffer, renderGraph, ctx.swapChainImageIndex);
        }
    }

    if (!ctx.offlineMode)
    {
        if (terrainFeedbackStreaming &&
            terrainFeedbackStreaming->feedbackRing)
        {
            // terrainFeedback 是 graph-owned 的 per-swapchain-image RT；
            // 帧末把当前 image 拷进该 frameIndex 的 host-visible ring 槽。
            const auto& resolveResources =
                renderGraph.GetResourcesResolve();
            const auto feedbackIt =
                resolveResources.find("terrainFeedback");
            if (feedbackIt != resolveResources.end() &&
                ctx.swapChainImageIndex < feedbackIt->second.size())
            {
                terrainFeedbackStreaming->feedbackRing->RecordCopy(
                    commandBuffer,
                    feedbackIt->second[ctx.swapChainImageIndex].image,
                    frameIndex,
                    feedbackSubmittedEpoch,
                    feedbackFrameTag);
            }
            // L4b：4 张层页 attachment 与覆盖页 RT 在同一次录制里回读。
            // 只有真正使用 layer mode 1 时才记录，mode 0 不产生额外拷贝。
            if (terrainFeedbackStreaming->layerVTRequested &&
                terrainFeedbackStreaming->layerFeedbackRing)
            {
                std::array<vk::Image, VL::TerrainLayerFeedbackSlotCount>
                    layerImages{};
                bool allPresent = true;
                for (uint32_t slot = 0;
                     slot < VL::TerrainLayerFeedbackSlotCount;
                     ++slot)
                {
                    const auto layerIt = resolveResources.find(
                        "terrainLayerFeedback" + std::to_string(slot));
                    if (layerIt == resolveResources.end() ||
                        ctx.swapChainImageIndex >= layerIt->second.size())
                    {
                        allPresent = false;
                        break;
                    }
                    layerImages[slot] =
                        layerIt->second[ctx.swapChainImageIndex].image;
                }
                if (allPresent)
                {
                    terrainFeedbackStreaming->layerFeedbackRing->RecordCopy(
                        commandBuffer,
                        layerImages,
                        frameIndex,
                        feedbackSubmittedEpoch,
                        feedbackFrameTag);
                }
            }
        }
    }

    std::optional<std::string> screenshotPath;
    {
        std::lock_guard<std::mutex> lock(ctx.pendingScreenshotPathMutex);
        screenshotPath = std::move(ctx.pendingScreenshotPath);
        ctx.pendingScreenshotPath.reset();
    }

    bool screenshotRecorded = false;
    std::string screenshotRecordError;
    if (screenshotPath.has_value())
    {
        screenshotRecorded = ctx.backend.RecordSwapchainScreenshot(
            frameContext,
            screenshotPath.value(),
            screenshotRecordError);
        if (!screenshotRecorded)
        {
            std::cout << "[Diagnostics][Warning] Screenshot record failed: " <<
                screenshotRecordError << std::endl;
        }
    }

    // 本帧查询随 SubmitFrame 进入 GPU；下次复用该 frame slot 时（fence 已等待）再读回。
    ctx.gpuTimer.EndFrame();
    // RenderSystem only records pass commands. Queue, semaphore, fence,
    // swapchain, present, and retire epoch details stay backend-owned.
    ctx.backend.SubmitFrame(frameContext, ctx.currentFrame);
    // 提交成功后才确认 dirty：失败路径由 TerrainUploadTransaction 析构 Abort。
    terrainUploadTransaction.Commit();
    terrainLayerUploadTransaction.Commit();
    if (terrainFeedbackStreaming)
    {
        terrainFeedbackStreaming->uploadedPagesTotal += terrainUploadPageCount;
        terrainFeedbackStreaming->lastUploadedPages = terrainUploadPageCount;
        terrainFeedbackStreaming->gpuBakedPagesTotal += terrainGpuBakeJobCount;
        terrainFeedbackStreaming->lastGpuBakedPages = terrainGpuBakeJobCount;
        ctx.terrainRuntime.PublishTerrainStreamingStats(
            terrainFeedbackStreaming,
            frameContext.completedEpoch);
    }
    ++ctx.submittedFrameSerial;
    ctx.submittedCameraGeneration = ctx.renderScene.worldGeneration;
    ctx.submittedViewProjection = ctx.renderScene.camera.viewProjection;
    if (!ctx.offlineMode)
        for (const auto& material : ctx.resolvedScene.materialGroups)
            for (const auto& instance : material.materialInstances)
                for (const auto& draw : instance.draws)
                {
                    const auto& packet = ctx.renderScene.drawPackets.at(draw.drawPacketIndex);
                    if (packet.skinPose && draw.objectResourceEntry)
                        draw.objectResourceEntry->GetResources().submittedSkin.Commit(
                            packet.skinPose, packet.model, ctx.submittedFrameSerial);
                }
    if (!ctx.offlineMode && renderGraph.GetAutoExposure())
        renderGraph.GetAutoExposure()->OnFrameSubmitted();

    if (screenshotRecorded)
    {
        std::string screenshotError;
        if (ctx.backend.CompleteSwapchainScreenshot(screenshotError))
        {
            std::cout << "[Diagnostics][Info] Screenshot saved: " <<
                screenshotPath.value() << std::endl;
        }
        else
        {
            std::cout << "[Diagnostics][Warning] Screenshot failed: " <<
                screenshotError << std::endl;
        }
    }

    ctx.currentFrame = ctx.currentFrame + 1;
}

} // namespace VL



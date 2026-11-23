#include "render/frame/terrainCaptureFrameBridge.h"
#include "render/frame/renderFrameRecorder.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/pass/passRuntime.h"
#include "render/resource/terrainCompositedService.h"
#include "render/resource/terrainCompositedQualityGpu.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/rendergraph/renderGraphDepthOwner.h"
#include "renderGraph.h"
#include <chrono>
#include <limits>

namespace VL
{
namespace
{
struct FrozenCaptureFrame
{
    std::optional<TerrainRvtBudgetLease> metadata;
    RenderScene scene;
    ResolvedRenderScene resolved;
    std::shared_ptr<TerrainCompositedQualitySnapshot> tables;
};

struct FrozenCaptureGraph
{
    std::vector<std::shared_ptr<const void>> resources;
    std::shared_ptr<const void> present;
};

uint64_t FrameMetadataBytes(const RenderScene& scene, const ResolvedRenderScene& resolved)
{
    uint64_t bytes = sizeof(FrozenCaptureFrame) + sizeof(FrozenCaptureGraph) + 65536;
    const auto add = [&bytes](uint64_t count, uint64_t size) {
        if (size && count > (std::numeric_limits<uint64_t>::max() - bytes) / size)
            throw std::overflow_error("TES immutable frame cost overflow");
        bytes += count * size;
    };
    add(scene.drawPackets.size(), sizeof(RenderDrawPacket));
    for (const auto& packet : scene.drawPackets)
        if (packet.isTerrain)
        {
            if (!packet.placementBinding.IsEmpty() || packet.placementBinding.tressfx ||
                !packet.placementBinding.tressfxGroupConfigs.empty())
                throw std::runtime_error("TES diagnostic terrain cannot own hair placement metadata");
            for (const auto* value : {&packet.debugName, &packet.sceneObjectIdentity, &packet.materialSlotName,
                &packet.mesh.key, &packet.material.key, &packet.materialInstance.key,
                &packet.speedTreeWindProfileKey, &packet.windResponseProfileKey,
                &packet.placementTemplateMeshKey, &packet.lodGroupId,
                &packet.placementBinding.sourceSceneObject, &packet.placementBinding.rootBone,
                &packet.placementBinding.weights})
                add(value->size() + 64, 2);
        }
    for (const auto& group : resolved.materialGroups)
    {
        add(1, sizeof(ResolvedMaterialGroup));
        add(group.materialInstances.size(), sizeof(ResolvedMaterialInstanceGroup) + 128);
        for (const auto& instance : group.materialInstances)
            add(instance.draws.size(), sizeof(ResolvedDrawPacket) + 512);
    }
    return bytes;
}
}

void TerrainCaptureFrameBridge::ConsumeCompleted(uint64_t completedEpoch)
{
    lifecycle.Complete(completedEpoch);
}

void TerrainCaptureFrameBridge::CloseAfterDeviceIdle()
{
    if (const auto hooks = AcquireTerrainCaptureHooks())
        hooks->CloseAfterDeviceIdle();
    lifecycle.ReleaseAfterDeviceIdle();
    current.reset();
}

void TerrainCaptureFrameBridge::BeforeGeometry(PassRuntimeContext& pass,
    RenderFrameRecorderContext& context, const RendererFrameContext& backendFrame,
    const std::shared_ptr<TerrainCompositedService>& service,
    const std::shared_ptr<ITerrainCaptureHooks>& hooks)
{
    if (current)
        throw std::logic_error("TES frame has more than one geometry capture bracket");
    if (!service || !hooks || !backendFrame.ready ||
        backendFrame.disposition.Phase() != AcquiredFramePhase::Recording)
        throw std::logic_error("TES capture requires an actual recording terrain World");
    const auto budget = service->BudgetOwner();
    if (!budget)
        throw std::logic_error("TES capture has no actual World budget owner");
    const auto metadataBytes = FrameMetadataBytes(context.renderScene, context.resolvedScene);
    if (metadataBytes > 32ull * 1024 * 1024)
        throw std::runtime_error("TES immutable frame exceeds its explicit bounded workspace domain");
    auto metadata = budget->TryReserve({0, metadataBytes, 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    if (!metadata)
        throw std::runtime_error("TES immutable frame exceeds actual World workspace admission");
    auto frozen = std::make_shared<FrozenCaptureFrame>();
    frozen->metadata = std::move(metadata);
    // 诊断视图只复制实际 terrain packet，保持原 drawPacketIndex；不复制粒子作者闭包。
    frozen->scene.worldGeneration = context.renderScene.worldGeneration;
    frozen->scene.frameIndex = context.renderScene.frameIndex;
    frozen->scene.camera = context.renderScene.camera;
    frozen->scene.drawPackets.resize(context.renderScene.drawPackets.size());
    for (size_t index = 0; index < context.renderScene.drawPackets.size(); ++index)
        if (context.renderScene.drawPackets[index].isTerrain)
            frozen->scene.drawPackets[index] = context.renderScene.drawPackets[index];
    frozen->resolved = context.resolvedScene;
    const auto now = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto stamp = pose.Begin(context.renderScene.worldGeneration, pass.renderGraph.GetOwnerGeneration(),
        context.renderScene.camera.viewProjection, {}, now);
    const uint64_t epoch = uint64_t(context.currentFrame) + 1;
    frozen->tables = service->CaptureQualitySnapshot(context.currentFrame, stamp.poseId, epoch,
        context.renderExtent.width, context.renderExtent.height);
    auto graph = std::make_shared<FrozenCaptureGraph>();
    const auto retain = [&](const auto& resources) {
        for (const auto& entry : resources)
            if (backendFrame.swapchainImageIndex < entry.second.size())
            {
                const auto& image = entry.second[backendFrame.swapchainImageIndex];
                if (image.image)
                {
                    auto owner = CaptureRenderGraphDepthOwner(image.image);
                    if (owner)
                        graph->resources.push_back(std::move(owner));
                }
            }
    };
    retain(pass.renderGraph.GetResourcesResolve());
    retain(pass.renderGraph.GetResourcesMsaa());
    if (backendFrame.swapchainImageIndex < pass.renderPass.worldBindingImageOwners.size())
    {
        auto snapshot = pass.renderPass.worldBindingImageOwners[backendFrame.swapchainImageIndex].Snapshot();
        if (snapshot)
            graph->resources.push_back(std::move(snapshot));
    }
    graph->present = context.backend.CapturePresentTargetLifetime();
    if (graph->resources.empty() || !graph->present)
        throw std::runtime_error("TES capture graph/present lifetime is unavailable");
    uint32_t fine = 0;
    for (const auto& owner : frozen->tables->owners)
        if (owner.valid)
        {
            if (owner.surface >= frozen->tables->surfaces.size())
                throw std::logic_error("TES native page owner surface is out of range");
            if (owner.mip < (frozen->tables->surfaces[owner.surface].data.maxMipAndRepeat & 255u))
                ++fine;
        }
    current = lifecycle.Prepare(backendFrame.frameIndex, epoch, hooks, frozen, graph);
    // 提交前入退休队列，保证 submit/present 或回调抛错也不能提前销毁 storage。
    ResourceRetireQueue::GetInstance().RetireShared("TerrainTesActualFrame",
        context.renderScene.worldGeneration, epoch, current);
    TerrainCaptureFrameInfo info;
    info.backend = &context.backend;
    info.actualWorldBudget = budget;
    info.immutableFrameOwner = frozen;
    info.immutableGraphOwner = graph;
    info.frame = context.currentFrame;
    info.submissionEpoch = epoch;
    info.worldGeneration = context.renderScene.worldGeneration;
    info.poseId = stamp.poseId;
    info.actualBaseFineResidentPages = fine;
    PassRuntimeContext immutablePass{pass.commandBuffer, pass.renderPass, pass.renderGraph,
        pass.passIndex, pass.swapChainImageIndex, frozen->scene, frozen->resolved,
        pass.services, pass.renderToViewport, pass.shadowCasterCullStats};
    lifecycle.BeforeGeometry(current, immutablePass, info);
}

void TerrainCaptureFrameBridge::AfterGeometry(vk::CommandBuffer command)
{
    lifecycle.AfterGeometry(current, command);
}

void TerrainCaptureFrameBridge::SubmitAttempted()
{
    if (current)
        lifecycle.SubmitAttempted(current);
}

void TerrainCaptureFrameBridge::Submitted(AcquiredFramePhase phase, uint64_t epoch)
{
    if (current)
        lifecycle.Submitted(current, phase, epoch);
}

bool TerrainCaptureFrameBridge::AbortProvenUnsubmitted(AcquiredFramePhase phase)
{
    const bool aborted = lifecycle.AbortProvenUnsubmitted(current, phase);
    if (aborted)
        current.reset();
    return aborted;
}
}

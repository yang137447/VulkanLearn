#include "render/hair/hairRuntime.h"

#include <iostream>
#include <utility>

#include "render/backend/resolvedRenderScene.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/frontend/renderScene.h"
#include "hair/hairSimulationPass.h"
#include "renderGraph.h"
#include "renderableObject.h"

namespace VL
{
void HairRuntime::RequestHairTrace(std::string path, uint32_t frames,
    uint32_t guideStride)
{
    VL::Hair::HairTraceRequest request;
    request.path = std::move(path);
    request.frames = frames;
    request.guideStride = guideStride;
    std::lock_guard<std::mutex> lock(traceRequestMutex);
    pendingTraceRequest = std::move(request);
}

void HairRuntime::RequestHairSimulationReset(std::string objectName)
{
    std::lock_guard<std::mutex> lock(resetRequestMutex);
    pendingResetRequest = std::move(objectName);
}

void HairRuntime::RequestHairSimulationParameter(
    const VL::Hair::HairLiveParameterRequest& request)
{
    std::lock_guard<std::mutex> lock(liveParameterMutex);
    // 队列有界：滑杆拖动时一帧可能累积多条；超过上限先丢最旧的意图，
    // 保留最新值，避免没有渲染帧时无限增长。
    if (pendingLiveParameters.size() >= 64)
        pendingLiveParameters.erase(pendingLiveParameters.begin());
    pendingLiveParameters.push_back(request);
}

void HairRuntime::RequestTressfxBrushPreview(
    const VL::Hair::TressfxBrushPreviewRequest& request)
{
    std::lock_guard<std::mutex> lock(brushPreviewMutex);
    if (pendingBrushPreviews.size() >= 64)
        pendingBrushPreviews.erase(pendingBrushPreviews.begin());
    pendingBrushPreviews.push_back(request);
}

void HairRuntime::RequestTressfxGroupBindingPreview(
    const VL::Hair::TressfxGroupBindingPreviewRequest& request)
{
    std::lock_guard<std::mutex> lock(groupBindingPreviewMutex);
    // 同一对象的草稿结构更新只需要最新一份；拖拽编辑时避免队列被旧 binding 占满。
    for (auto entry = pendingGroupBindingPreviews.begin();
         entry != pendingGroupBindingPreviews.end(); ++entry)
        if (entry->objectName == request.objectName &&
            entry->worldGeneration == request.worldGeneration)
        {
            *entry = request;
            return;
        }
    if (pendingGroupBindingPreviews.size() >= 16)
        pendingGroupBindingPreviews.erase(
            pendingGroupBindingPreviews.begin());
    pendingGroupBindingPreviews.push_back(request);
}

void HairRuntime::ApplyTressfxGroupBindingPreviewRequests(
    const VL::RenderScene& renderScene,
    const VL::ResolvedRenderScene& resolvedRenderScene)
{
    std::vector<VL::Hair::TressfxGroupBindingPreviewRequest> requests;
    {
        std::lock_guard<std::mutex> lock(groupBindingPreviewMutex);
        requests.swap(pendingGroupBindingPreviews);
    }
    if (requests.empty()) return;

    for (const VL::Hair::TressfxGroupBindingPreviewRequest& request : requests)
    {
        if (request.worldGeneration != renderScene.worldGeneration) continue;
        // Resolved draws hold strong renderable references; never re-resolve a
        // cache pointer here, because GT may retire the cache entry mid-frame.
        std::shared_ptr<RenderableObject> object;
        for (const auto& material : resolvedRenderScene.materialGroups)
            for (const auto& instance : material.materialInstances)
                for (const auto& draw : instance.draws)
                {
                    if (!draw.renderableObject) continue;
                    const VL::RenderDrawPacket& packet =
                        renderScene.drawPackets.at(draw.drawPacketIndex);
                    if (packet.debugName != request.objectName) continue;
                    object = draw.renderableObject;
                    break;
                }
        if (!object) continue;
        const VL::Hair::TressfxGroupBinding binding =
            VL::Hair::MakeTressfxGroupBinding(request);
        std::string error;
        if (!object->UpdateTressfxGroupBinding(binding, error))
        {
            std::cout << "[TressFX][Warning] group binding preview rejected: "
                      << error << " object=" << request.objectName << std::endl;
        }
    }
}

void HairRuntime::ApplyTressfxBrushPreviewRequests(
    const VL::RenderScene& renderScene,
    VL::ResolvedRenderScene& resolvedRenderScene,
    uint32_t swapchainImageIndex)
{
    std::vector<VL::Hair::TressfxBrushPreviewRequest> requests;
    {
        std::lock_guard<std::mutex> lock(brushPreviewMutex);
        requests.swap(pendingBrushPreviews);
    }
    if (requests.empty()) return;

    for (const VL::Hair::TressfxBrushPreviewRequest& request : requests)
    {
        if (request.worldGeneration != renderScene.worldGeneration) continue;
        std::shared_ptr<RenderableObject> object;
        for (const auto& material : resolvedRenderScene.materialGroups)
            for (const auto& instance : material.materialInstances)
                for (const auto& draw : instance.draws)
                {
                    if (!draw.renderableObject) continue;
                    const VL::RenderDrawPacket& packet =
                        renderScene.drawPackets.at(draw.drawPacketIndex);
                    if (packet.debugName != request.objectName) continue;
                    object = draw.renderableObject;
                    break;
                }
        if (!object) continue;
        std::string error;
        if (!object->SetTressfxBrushPreview(request.targetGroupId,
                request.mode, request.weights, swapchainImageIndex, error))
        {
            std::cout << "[TressFX][Warning] brush preview rejected: " << error
                      << " object=" << request.objectName << std::endl;
            continue;
        }
        // UBO 的 brush 状态属于对象资源包；在 draw 录制前同步，保证本帧读到新值。
        for (auto& material : resolvedRenderScene.materialGroups)
            for (auto& instance : material.materialInstances)
                for (auto& draw : instance.draws)
                {
                    if (!draw.objectResourceEntry) continue;
                    const VL::RenderDrawPacket& packet =
                        renderScene.drawPackets.at(draw.drawPacketIndex);
                    if (packet.debugName != request.objectName) continue;
                    VL::RendererObjectGpuResources& resources =
                        draw.objectResourceEntry->GetResources();
                    resources.tressfxBrushActive = object->HasTressfxBrushPreview();
                    resources.tressfxBrushRemoveMode = object->TressfxBrushRemoveMode();
                }
    }
}
void HairRuntime::RecordHairSimulation(
    vk::CommandBuffer commandBuffer,
    uint32_t frameSlot,
    const VL::ResolvedRenderScene& resolvedRenderScene,
    const VL::RenderScene& renderScene)
{
    const auto& hairSimulation = RenderGraph::GetInstance().GetHairSimulation();
    if (!hairSimulation)
        return;
    // GT 侧只登记请求；这里在渲染线程把它交给 pass。trace 的资源创建/拷贝
    // 全部发生在 Record 内，Console 线程不触碰 GPU。
    std::optional<VL::Hair::HairTraceRequest> traceRequest;
    {
        std::lock_guard<std::mutex> lock(traceRequestMutex);
        traceRequest = std::move(pendingTraceRequest);
        pendingTraceRequest.reset();
    }
    if (traceRequest)
        hairSimulation->BeginTrace(*traceRequest);
    // 复位请求只在存在 pass 时消费；没有 pass 就保留到下一个可录制帧，
    // 避免把请求丢在无 World/无 graph 的帧里。
    std::optional<std::string> resetRequest;
    {
        std::lock_guard<std::mutex> lock(resetRequestMutex);
        resetRequest = std::move(pendingResetRequest);
        pendingResetRequest.reset();
    }
    if (resetRequest)
        hairSimulation->RequestReset(std::move(*resetRequest));
    // 实时参数覆盖与复位同构：GT 只登记，渲染线程每帧取走一次。没有 pass 时
    // 上面的提前 return 会保留队列，避免把请求丢在无 World/无 graph 的帧里。
    std::vector<VL::Hair::HairLiveParameterRequest> liveParameters;
    {
        std::lock_guard<std::mutex> lock(liveParameterMutex);
        liveParameters.swap(pendingLiveParameters);
    }
    for (const auto& parameter : liveParameters)
        hairSimulation->RequestLiveParameter(parameter);
    // 每帧只调用一次，且在所有 graphics pass 之前：本帧所有读取 current positions 的
    // vertex shader 都看到同一份已经完成 dispatch 的位置。
    hairSimulation->Record(
        commandBuffer, resolvedRenderScene, renderScene, frameSlot);
}
} // namespace VL

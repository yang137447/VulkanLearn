#pragma once

#include "render/frame/terrainCaptureFrameLifecycle.h"
#include "render/frame/terrainCompositedQualityValidation.h"

namespace VL
{
struct RenderFrameRecorderContext;
struct RendererFrameContext;
class TerrainCompositedService;

// 显式诊断时冻结实际 frame/draw/graph owner；普通 Game 不创建本 bridge。
class TerrainCaptureFrameBridge
{
public:
    void ConsumeCompleted(uint64_t completedEpoch);
    void CloseAfterDeviceIdle();
    void BeforeGeometry(PassRuntimeContext& pass, RenderFrameRecorderContext& context,
        const RendererFrameContext& backendFrame,
        const std::shared_ptr<TerrainCompositedService>& service,
        const std::shared_ptr<ITerrainCaptureHooks>& hooks);
    void AfterGeometry(vk::CommandBuffer command);
    void SubmitAttempted();
    void Submitted(AcquiredFramePhase phase, uint64_t epoch);
    bool AbortProvenUnsubmitted(AcquiredFramePhase phase);
    bool HasCurrent() const noexcept { return bool(current); }
    bool SubmitWasAttempted() const noexcept { return current && current->submitAttempted; }
    void DetachCurrent() noexcept { current.reset(); }

private:
    TerrainCaptureFrameLifecycle lifecycle;
    TerrainQualityValidation pose;
    std::shared_ptr<TerrainCaptureFrameLifecycle::Flight> current;
};
}

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include "terrain/terrainFaultInjection.h"
#include "render/backend/frameAbandon.h"
#include "render/backend/gpuFrameIdentity.h"

namespace VL
{
class TerrainCompositedService;

// owner 提供已录制任务及 Abort 后的真实计数；frame 不重建任务 token 或伪造发布计数。
struct FrameSubmitFault
{
    uint64_t registration = 0;
    TerrainFaultSubmitFailure failure;
    TerrainFaultSubmitObservation task;
    std::shared_ptr<const void> ownerLease;
    std::function<TerrainFaultSubmitAbortObservation()> observeAfterAbort;
};

using FrameSubmitFaultProvider = std::function<std::optional<FrameSubmitFault>(
    const std::shared_ptr<TerrainCompositedService>&, uint64_t, uint64_t)>;

void SetFrameSubmitFaultProvider(FrameSubmitFaultProvider provider);
std::optional<FrameSubmitFault> TakeFrameSubmitFault(
    const std::shared_ptr<TerrainCompositedService>& service, uint64_t worldGeneration, uint64_t epoch,
    std::optional<uint64_t> renderFrame = {});
void ObserveRecoveredFrameAbort(const FrameSubmitFault& fault, const FrameAbortResult& result,
    std::optional<uint64_t> renderFrame = {});
void ObservePostAbortSubmittedFrame(uint64_t worldGeneration, uint64_t epoch,
    std::optional<uint64_t> renderFrame = {});
void ObservePostAbortCompletedFrame(uint64_t completedEpoch);

struct FrameBindingIdentity
{
    uint64_t worldGeneration = 0;
    uint64_t revision = 0;
};
void ObserveRecoveredDescriptorFrame(const RenderFrameIdentity& frame, const FrameAbortResult& result,
    const FrameBindingIdentity& requested, const char* operation, int32_t rawResult, uint32_t retryCause = 0);
bool NeedsDescriptorRecoveryWitness() noexcept;
void ObserveDescriptorRecoverySubmittedFrame(
    const RenderFrameIdentity& frame, const FrameBindingIdentity* actualBindings);
}

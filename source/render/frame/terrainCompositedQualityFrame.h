#pragma once

#include "render/resource/terrainCompositedQualityGpu.h"
#include <functional>

class RenderGraph;
struct RenderResource;

namespace VL
{
struct RenderFrameRecorderContext;
struct RendererFrameContext;
struct PassRuntimeContext;
class TerrainCompositedService;
struct TerrainCompositedLoadingTarget;

struct TerrainQualityObservation
{
    uint64_t worldGeneration = 0;
    uint64_t poseId = 0;
    uint64_t frame = 0;
    uint64_t submissionEpoch = 0;
    uint64_t completedEpoch = 0;
    double completedMonotonicMs = 0;
    TerrainQualityAbi::Counts counts;
};

class TerrainCompositedQualityFrame
{
public:
    using DepthOwnerResolver = std::function<std::shared_ptr<const void>(const RenderResource&)>;
    static void SetDepthOwnerResolver(DepthOwnerResolver resolver);
    static std::optional<TerrainQualityObservation> TakeObservation(uint64_t worldGeneration);
    static void CloseAfterDeviceIdle(const char* reason = "shutdown");
    static TerrainCompositedQualityFrame BeginFrame(
        RenderFrameRecorderContext& context, const RendererFrameContext& frame,
        const std::shared_ptr<TerrainCompositedService>& service);
    TerrainCompositedQualityFrame();
    ~TerrainCompositedQualityFrame();
    TerrainCompositedQualityFrame(TerrainCompositedQualityFrame&&) noexcept;
    TerrainCompositedQualityFrame& operator=(TerrainCompositedQualityFrame&&) noexcept;
    TerrainCompositedQualityFrame(const TerrainCompositedQualityFrame&) = delete;
    void RecordAfterFeedback(PassRuntimeContext& context);
    void RecordLoading(RenderFrameRecorderContext& context, vk::CommandBuffer command,
        const TerrainCompositedLoadingTarget& target);
    void Submitted(uint64_t epoch) noexcept;
    void Abort() noexcept;

private:
    struct State;
    std::shared_ptr<State> state;
    std::weak_ptr<TerrainCompositedService> service;
    uint64_t frameNumber = 0;
    uint64_t drawEpoch = 0;
    uint64_t poseId = 0;
    uint64_t handle = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t frameSlot = 0;
    std::shared_ptr<void> loadingLifetime;
};
}

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "hair/hairTraceFormat.h"
#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxGroupPaint.h"
#include "hair/tressfxLiveParams.h"

namespace VL
{

struct RenderScene;
struct ResolvedRenderScene;

// GT/RT bridge for hair commands. GT only registers value-semantic requests;
// the render thread drains them once per recorded frame and forwards them to
// HairSimulationPass / RenderableObject. No GPU resource is touched on GT.
class HairRuntime
{
public:
    void RequestHairTrace(std::string path, uint32_t frames,
        uint32_t guideStride);
    void RequestHairSimulationReset(std::string objectName);
    void RequestHairSimulationParameter(
        const VL::Hair::HairLiveParameterRequest& request);
    void RequestTressfxBrushPreview(
        const VL::Hair::TressfxBrushPreviewRequest& request);
    void RequestTressfxGroupBindingPreview(
        const VL::Hair::TressfxGroupBindingPreviewRequest& request);

    void ApplyTressfxGroupBindingPreviewRequests(
        const VL::RenderScene& renderScene,
        const VL::ResolvedRenderScene& resolvedRenderScene);
    void ApplyTressfxBrushPreviewRequests(
        const VL::RenderScene& renderScene,
        VL::ResolvedRenderScene& resolvedRenderScene,
        uint32_t swapchainImageIndex);
    void RecordHairSimulation(
        vk::CommandBuffer commandBuffer,
        uint32_t frameSlot,
        const VL::ResolvedRenderScene& resolvedRenderScene,
        const VL::RenderScene& renderScene);

private:
    std::mutex traceRequestMutex;
    std::optional<VL::Hair::HairTraceRequest> pendingTraceRequest;
    std::mutex resetRequestMutex;
    std::optional<std::string> pendingResetRequest;
    std::mutex liveParameterMutex;
    std::vector<VL::Hair::HairLiveParameterRequest> pendingLiveParameters;
    std::mutex brushPreviewMutex;
    std::vector<VL::Hair::TressfxBrushPreviewRequest> pendingBrushPreviews;
    std::mutex groupBindingPreviewMutex;
    std::vector<VL::Hair::TressfxGroupBindingPreviewRequest>
        pendingGroupBindingPreviews;
};

} // namespace VL

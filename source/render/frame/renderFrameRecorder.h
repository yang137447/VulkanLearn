#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

#include "render/shadow/shadowCasterCull.h"
#include "render/offline/offlineDisplayVulkan.h"
#include "render/offline/offlineRenderCoordinator.h"

namespace VL
{
struct RenderScene;
struct ResolvedRenderScene;
struct UiRenderSnapshot;
class RendererFrameResources;
class CsmRuntime;
class GpuPassTimer;
class DynamicMeshUpdateQueue;
class RendererBackendVulkan;
class PassRuntime;
class PassRuntimeServices;
class TerrainFeedbackRuntime;
class HairRuntime;
class UiOverlayRendererVulkan;
struct UiRenderSnapshotQueue;
struct OfflinePublishedResult;

// Explicit RT-side view of RenderSystem state. Every field is a reference (or a
// value-type callback) into the owner; the recorder never caches state.
struct RenderFrameRecorderContext
{
    RendererBackendVulkan& backend;
    PassRuntime& passRuntime;
    PassRuntimeServices& passServices;
    RenderScene& renderScene;
    ResolvedRenderScene& resolvedScene;
    GpuPassTimer& gpuTimer;
    UiOverlayRendererVulkan& uiOverlayRenderer;
    UiRenderSnapshotQueue* uiSnapshotQueue;
    std::shared_ptr<const UiRenderSnapshot>& currentUiSnapshot;
    TerrainFeedbackRuntime& terrainRuntime;
    HairRuntime& hairRuntime;
    DynamicMeshUpdateQueue& dynamicMeshQueue;
    OfflineRenderCoordinator* offlineCoordinator;
    OfflineDisplayVulkan& offlineDisplay;
    std::atomic<bool>& offlineMode;
    std::atomic<OfflineAov>& offlineAov;
    std::atomic<float>& offlineExposure;
    ShadowCasterCullStats* frameShadowCasterCullStats;
    uint32_t& currentFrame;
    uint32_t& swapChainImageIndex;
    uint64_t& submittedCameraGeneration;
    std::optional<Eigen::Matrix4f>& submittedViewProjection;
    uint64_t& submittedFrameSerial;
    std::atomic<bool>& gpuPassStatsLoggingEnabled;
    uint32_t& shadowCasterCullStatsFramesSinceLog;
    std::optional<std::string>& pendingScreenshotPath;
    std::mutex& pendingScreenshotPathMutex;
    vk::Extent2D renderExtent;
    std::function<void()> advanceSceneWind;
    std::function<void(vk::CommandBuffer&)> updateGlobalUBO;
    std::function<void(vk::CommandBuffer&, uint32_t)> recordEnvironmentIbl;
    std::function<void(vk::CommandBuffer&, uint32_t)> recordSkinDeform;
    std::function<std::shared_ptr<const OfflinePublishedResult>()>
        captureOfflineResult;
    std::function<void()> logShadowCasterCullStats;
    std::function<void()> refreshRenderScene;
};
// Owns the per-frame CPU plumbing that does not depend on RenderSystem's
// lifetime: dynamic-mesh queue drain, global/shadow UBO construction and the
// exposure timing clock. RenderSystem keeps the RT ownership and safe points.
class RenderFrameRecorder
{
public:
    void RecordAndSubmitCurrentRenderScene(
        RenderFrameRecorderContext& ctx);

    void ApplyQueuedDynamicMeshUpdates(
        VL::DynamicMeshUpdateQueue& queue,
        const VL::RenderScene& renderScene,
        const VL::ResolvedRenderScene& resolvedScene,
        uint32_t frameSlot);

    void UpdateExposureFrameTiming(VL::GpuPassTimer& gpuPassTimer);
    float ExposureDeltaSeconds() const noexcept { return exposureDeltaSeconds; }

    void UpdateGlobalUBO(
        vk::CommandBuffer& commandBuffer,
        VL::RendererFrameResources& frameResources,
        uint32_t swapChainImageIndex,
        const VL::RenderScene& renderScene,
        bool hasRenderScene,
        const std::function<void()>& refreshRenderScene,
        VL::CsmRuntime& csmRuntime,
        uint32_t currentFrame);
    void UpdateGlobalUBOWithProjectionTransform(
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
        float projectionOffsetY);
    void UpdateShadowGlobalUBO(
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
        uint32_t cascadeIndex);

private:
    // RT-only: one warning per mesh key, otherwise a missing object spams every
    // frame while its World entry is being torn down.
    std::unordered_set<std::string> reportedMissingDynamicMeshKeys;
    std::unordered_map<std::string, int> missingDynamicMeshAttempts;

    float exposureDeltaSeconds = 1.0f / 60.0f;
    bool exposureClockInitialized = false;
    std::chrono::steady_clock::time_point exposureLastFrameTime;
};

} // namespace VL

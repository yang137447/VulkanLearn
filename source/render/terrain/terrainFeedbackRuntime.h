#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

#include "terrain/terrainFeedback.h"
#include "terrain/terrainLayerFeedback.h"

namespace VL
{

struct TerrainStreamingState;
struct TerrainLodStatsSnapshot;

// 控制台只读诊断：最近一次已完成 feedback 回读的页统计。
struct TerrainFeedbackReport
{
    VL::TerrainFeedbackFrameSummary summary;
    bool hasPendingFrameTagRange = false;
    uint8_t oldestPendingFrameTag = 0;
    uint8_t newestPendingFrameTag = 0;
};

// Owns the render-thread side of terrain page streaming: feedback consume/apply,
// layer exact feedback, console budget/preload/force-mip requests, parity bake
// and the published statistics. All page residency remains in
// TerrainStreamingState; this class only sequences access to it.
class TerrainFeedbackRuntime
{
public:
    // Console commands only write these atomics; the render thread consumes
    // them at frame start before any residency change.
    void RequestTerrainResidentCapacity(uint32_t capacity);
    void RequestTerrainLayerResidentCapacity(uint32_t capacity);
    void RequestTerrainLayerPreload();
    void RequestTerrainLayerForceMip(int forceMip);
    void RequestTerrainPageBakeParity(int tileX, int tileZ);
    void RunTerrainPageBakeParityIfRequested();

    void ConsumeTerrainFeedback(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming,
        uint64_t completedEpoch,
        uint32_t frameIndex);
    void PrepareTerrainFeedbackPass(
        uint8_t frameTag,
        vk::Extent2D renderExtent);
    void ApplyTerrainLayerForceMip(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming);
    void UpdateTerrainCameraPrefetch(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming,
        const Eigen::Vector3f& cameraPosition,
        const Eigen::Vector3f& cameraForward);
    void PublishTerrainStreamingStats(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming,
        uint64_t completedEpoch);

    void ReportTerrainFeedbackStatistics() const;
    static void ReportTerrainStatistics();
    static void ReportTerrainLayerStatistics(vk::Extent2D renderExtent);
    static void ReportTerrainLodStatistics(
        const VL::TerrainLodStatsSnapshot& stats);
    static void ReportTerrainChunkStatistics(
        const VL::TerrainLodStatsSnapshot& lodStats);

private:
    void ApplyTerrainLayerFeedback(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming,
        const VL::TerrainFeedbackFrameSummary& summary,
        uint64_t completedEpoch,
        uint32_t frameIndex);
    bool ApplyTerrainLayerExactFeedback(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming,
        const VL::TerrainLayerFeedbackFrameSummary& summary,
        uint64_t completedEpoch,
        uint32_t frameIndex);
    void StoreTerrainFeedbackSummary(
        const std::shared_ptr<VL::TerrainStreamingState>& streaming,
        const VL::TerrainFeedbackFrameSummary& summary);

    mutable std::mutex statisticsMutex;
    std::optional<TerrainFeedbackReport> latestReport;

    std::atomic<bool> pageBakeParityRequested{false};
    std::atomic<int32_t> pageBakeParityTileX{0};
    std::atomic<int32_t> pageBakeParityTileZ{0};

    // L4e：相机预取用的上一帧相机位置；只反映真正提交的帧。
    Eigen::Vector3f lastPrefetchCameraPosition = Eigen::Vector3f::Zero();
    bool hasPrefetchCameraPosition = false;
};

} // namespace VL

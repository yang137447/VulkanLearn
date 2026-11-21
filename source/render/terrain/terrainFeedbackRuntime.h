#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include <vulkan/vulkan.hpp>
#include "terrain/terrainFeedback.h"

namespace VL
{
struct TerrainStreamingState;
struct TerrainLodStatsSnapshot;

struct TerrainFeedbackReport
{
    TerrainFeedbackFrameSummary summary;
    bool hasPendingFrameTagRange = false;
    uint8_t oldestPendingFrameTag = 0;
    uint8_t newestPendingFrameTag = 0;
};

class TerrainFeedbackRuntime
{
public:
    void RequestTerrainResidentCapacity(uint32_t capacity);
    void RequestTerrainPageBakeParity(int tileX, int tileZ);
    void RunTerrainPageBakeParityIfRequested();
    void ConsumeTerrainFeedback(const std::shared_ptr<TerrainStreamingState>& streaming,
        uint64_t completedEpoch, uint32_t frameIndex);
    void PrepareTerrainFeedbackPass(uint8_t frameTag, vk::Extent2D renderExtent);
    void PublishTerrainStreamingStats(const std::shared_ptr<TerrainStreamingState>& streaming,
        uint64_t completedEpoch);
    void ReportTerrainFeedbackStatistics() const;
    static void ReportTerrainStatistics();
    static void ReportTerrainLodStatistics(const TerrainLodStatsSnapshot& stats);
    static void ReportTerrainChunkStatistics(const TerrainLodStatsSnapshot& lodStats);

private:
    void StoreTerrainFeedbackSummary(const std::shared_ptr<TerrainStreamingState>& streaming,
        const TerrainFeedbackFrameSummary& summary);
    mutable std::mutex statisticsMutex;
    std::optional<TerrainFeedbackReport> latestReport;
    std::atomic<bool> pageBakeParityRequested{false};
    std::atomic<int32_t> pageBakeParityTileX{0};
    std::atomic<int32_t> pageBakeParityTileZ{0};
};
}

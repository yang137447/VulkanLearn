#pragma once

#include "render/terrain/terrainCompositedRequestDiagnostics.h"
#include <memory>
#include <string>
#include <string_view>

namespace VL
{
class TerrainCompositedServiceRequestDiagnostics
{
public:
    static std::unique_ptr<TerrainCompositedServiceRequestDiagnostics> Create(
        TerrainRvtBudgetOwner& owner);
    void Capture(const TerrainCompositedRuntime& runtime, TerrainRvtBudgetOwner& owner,
        uint64_t world, uint64_t pose, double poseStartedMs, uint64_t renderFrame,
        uint64_t completedEpoch, double nowMs);
    void Cancel() noexcept;
    void ObserveFeedback(const TerrainCompositedRuntime& runtime,
        const std::vector<TerrainCompositedVTTaskToken>& requests);
    void ObserveReadback(const TerrainCompositedRuntime& runtime,
        const std::array<std::string_view, 256>& surfaces, size_t surfaceCount,
        const uint8_t* bytes, size_t byteCount, uint8_t frameTag,
        uint64_t copiedEpoch, uint64_t completedEpoch);
    bool Finished() const noexcept { return terminal; }

private:
    std::optional<TerrainRvtBudgetLease> configurationLease;
    nlohmann::json plan;
    TerrainCompositedExplicitRequestCollector collector;
    std::string requestId;
    uint64_t activationPose = 0;
    uint64_t expectedWorld = 0;
    uint64_t activeWorld = 0;
    uint32_t captures = 0;
    bool armed = false;
    bool terminal = false;
    std::array<uint64_t, 8> matchingFeedbackRequests{};
    uint64_t feedbackBatch = 0;
    uint64_t feedbackRequestCount = 0;
    struct ReadbackWitness
    {
        std::array<uint8_t, 4> address{};
        std::array<uint8_t, 4> metadata{};
        uint64_t sameSurfaceMipTexels = 0;
        uint64_t exactAddressTexels = 0;
        uint64_t decodedMatches = 0;
        bool present = false;
        bool selectedBefore = false;
        bool selectedAfter = false;
    };
    std::array<ReadbackWitness, 8> witnesses{};
    uint64_t feedbackCopiedEpoch = 0;
    uint64_t feedbackCompletedEpoch = 0;
    uint8_t feedbackFrameTag = 0;
};
}

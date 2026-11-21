#pragma once

#include "render/resource/terrainCompositedQualityData.h"

#include <Eigen/Core>
#include <array>
#include <mutex>

namespace VL
{
struct TerrainQualityInvalidation
{
    uint64_t worldGeneration = 0;
    uint64_t sequence = 0;
    double arrivedMonotonicMs = 0;
};

struct TerrainQualityValidationStamp
{
    uint64_t worldGeneration = 0;
    uint64_t graphGeneration = 0;
    uint64_t poseId = 0;
    uint64_t resourceVersion = 0;
    uint64_t invalidationSequence = 0;
};

class TerrainQualityValidation
{
public:
    TerrainQualityValidationStamp Begin(
        uint64_t worldGeneration, uint64_t graphGeneration,
        const Eigen::Matrix4f& viewProjection, TerrainQualityInvalidation invalidation,
        double monotonicMs);
    TerrainQualityValidationStamp Resources(
        const TerrainQualityAbi::Surface* surfaces, size_t count, double monotonicMs);
    TerrainQualityValidationStamp BindingsChanged(
        const Eigen::Matrix4f& viewProjection, double monotonicMs);
    bool Observe(const TerrainQualityValidationStamp& stamp,
        const Eigen::Matrix4f& capturedCamera, uint64_t submissionEpoch,
        uint64_t completedEpoch, double completedMs,
        const TerrainQualityAbi::Counts& counts, bool serviceAccepted);
    bool Ready(uint64_t worldGeneration, const Eigen::Matrix4f& viewProjection,
        TerrainQualityInvalidation invalidation) const;
    double PoseArrivedMonotonicMs() const;
    void Revoke();

private:
    void Invalidate(const Eigen::Matrix4f& viewProjection, double monotonicMs);
    mutable std::mutex mutex;
    TerrainQualityValidationStamp current;
    Eigen::Matrix4f arrivalCamera = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f validatedCamera = Eigen::Matrix4f::Identity();
    std::array<TerrainQualityAbi::Surface, TerrainQualityAbi::Surfaces> resources{};
    size_t surfaceCount = 0;
    double arrivedMs = 0;
    uint64_t lastEpoch = 0;
    bool ready = false;
};

bool TerrainQualityViewWithinValidatedDomain(
    const Eigen::Matrix4f& reference, const Eigen::Matrix4f& candidate);
}

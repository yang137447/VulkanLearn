#include "render/frame/terrainCompositedQualityValidation.h"

#include <Eigen/LU>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace VL
{
namespace
{
bool SameStamp(const TerrainQualityValidationStamp& left,
    const TerrainQualityValidationStamp& right)
{
    return left.worldGeneration == right.worldGeneration &&
        left.graphGeneration == right.graphGeneration && left.poseId == right.poseId &&
        left.resourceVersion == right.resourceVersion &&
        left.invalidationSequence == right.invalidationSequence;
}
}

bool TerrainQualityViewWithinValidatedDomain(
    const Eigen::Matrix4f& reference, const Eigen::Matrix4f& candidate)
{
    if (!reference.allFinite() || !candidate.allFinite())
        return false;
    const Eigen::FullPivLU<Eigen::Matrix4d> inverse(reference.cast<double>());
    const Eigen::FullPivLU<Eigen::Matrix4d> candidateInverse(candidate.cast<double>());
    if (!inverse.isInvertible() || !candidateInverse.isInvertible())
        return false;
    const Eigen::Matrix4d previous = inverse.inverse();
    const Eigen::Matrix4d next = candidateInverse.inverse();
    const Eigen::Vector4d previousEye = previous.col(2);
    const Eigen::Vector4d nextEye = next.col(2);
    if (std::abs(previousEye.w()) > 1.0e-12 && std::abs(nextEye.w()) > 1.0e-12)
    {
        const Eigen::Vector3d previousPosition = previousEye.head<3>() / previousEye.w();
        const Eigen::Vector3d nextPosition = nextEye.head<3>() / nextEye.w();
        if ((previousPosition - nextPosition).squaredNorm() > 512.0 * 512.0)
            return false;
        for (double vertical : {-0.75, 0.0, 0.75})
            for (double horizontal : {-0.75, 0.0, 0.75})
            {
                const Eigen::Vector4d clip(horizontal, vertical, 0.5, 1.0);
                const Eigen::Vector4d previousPoint = previous * clip;
                const Eigen::Vector4d nextPoint = next * clip;
                if (std::abs(previousPoint.w()) <= 1.0e-12 ||
                    std::abs(nextPoint.w()) <= 1.0e-12)
                    return false;
                const Eigen::Vector3d previousRay =
                    previousPoint.head<3>() / previousPoint.w() - previousPosition;
                const Eigen::Vector3d nextRay = nextPoint.head<3>() / nextPoint.w() - nextPosition;
                if (previousRay.squaredNorm() <= 1.0e-12 || nextRay.squaredNorm() <= 1.0e-12 ||
                    previousRay.normalized().dot(nextRay.normalized()) < 0.9659258262890683)
                    return false;
            }
        return true;
    }
    const Eigen::Matrix4f motion = (candidate.cast<double>() * previous).cast<float>();
    for (float depth : {0.0f, 0.5f, 0.99f})
        for (float vertical : {-0.75f, 0.0f, 0.75f})
            for (float horizontal : {-0.75f, 0.0f, 0.75f})
            {
                const Eigen::Vector4f projected =
                    motion * Eigen::Vector4f(horizontal, vertical, depth, 1.0f);
                if (!projected.allFinite() || projected.w() <= 1.0e-6f)
                    return false;
                if (std::abs(projected.x() / projected.w() - horizontal) > 0.25f ||
                    std::abs(projected.y() / projected.w() - vertical) > 0.25f ||
                    std::abs(projected.z() / projected.w() - depth) > 0.1f)
                    return false;
            }
    return true;
}

void TerrainQualityValidation::Invalidate(
    const Eigen::Matrix4f& viewProjection, double monotonicMs)
{
    ++current.poseId;
    arrivalCamera = viewProjection;
    arrivedMs = monotonicMs;
    ready = false;
    lastEpoch = 0;
}

TerrainQualityValidationStamp TerrainQualityValidation::Begin(
    uint64_t worldGeneration, uint64_t graphGeneration,
    const Eigen::Matrix4f& viewProjection, TerrainQualityInvalidation invalidation,
    double monotonicMs)
{
    if (!worldGeneration || !viewProjection.allFinite() || !std::isfinite(monotonicMs))
        throw std::invalid_argument("Quality validation needs a finite World/view arrival");
    std::lock_guard<std::mutex> lock(mutex);
    const bool replaced = current.worldGeneration != worldGeneration ||
        current.graphGeneration != graphGeneration;
    const bool explicitArrival = invalidation.worldGeneration == worldGeneration &&
        invalidation.sequence != current.invalidationSequence;
    if (replaced)
    {
        current.worldGeneration = worldGeneration;
        current.graphGeneration = graphGeneration;
        surfaceCount = 0;
        ++current.resourceVersion;
    }
    if (replaced || explicitArrival ||
        (ready && !TerrainQualityViewWithinValidatedDomain(validatedCamera, viewProjection)))
    {
        if (explicitArrival)
            current.invalidationSequence = invalidation.sequence;
        Invalidate(viewProjection, explicitArrival ? invalidation.arrivedMonotonicMs : monotonicMs);
    }
    return current;
}

TerrainQualityValidationStamp TerrainQualityValidation::Resources(
    const TerrainQualityAbi::Surface* surfaces, size_t count, double monotonicMs)
{
    if (!surfaces || !count || count > resources.size() || !std::isfinite(monotonicMs))
        throw std::invalid_argument("Quality resource identity exceeds the frozen surface domain");
    std::lock_guard<std::mutex> lock(mutex);
    if (count != surfaceCount ||
        std::memcmp(resources.data(), surfaces, count * sizeof(*surfaces)) != 0)
    {
        const bool replaced = surfaceCount != 0;
        std::memcpy(resources.data(), surfaces, count * sizeof(*surfaces));
        surfaceCount = count;
        ++current.resourceVersion;
        if (replaced)
            Invalidate(arrivalCamera, monotonicMs);
    }
    return current;
}

bool TerrainQualityValidation::Observe(const TerrainQualityValidationStamp& stamp,
    const Eigen::Matrix4f& capturedCamera, uint64_t submissionEpoch,
    uint64_t completedEpoch, double completedMs,
    const TerrainQualityAbi::Counts& counts, bool serviceAccepted)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!SameStamp(stamp, current) || !surfaceCount || !submissionEpoch ||
        completedEpoch < submissionEpoch || submissionEpoch <= lastEpoch ||
        !std::isfinite(completedMs) || completedMs < arrivedMs || !capturedCamera.allFinite())
        return false;
    lastEpoch = submissionEpoch;
    ready = serviceAccepted && counts.MeetsQualityThreshold();
    if (ready)
        validatedCamera = capturedCamera;
    return ready;
}

TerrainQualityValidationStamp TerrainQualityValidation::BindingsChanged(
    const Eigen::Matrix4f& viewProjection, double monotonicMs)
{
    if (!viewProjection.allFinite() || !std::isfinite(monotonicMs))
        throw std::invalid_argument("Quality binding change needs its actual view/event time");
    std::lock_guard<std::mutex> lock(mutex);
    ++current.resourceVersion;
    Invalidate(viewProjection, monotonicMs);
    return current;
}

bool TerrainQualityValidation::Ready(uint64_t worldGeneration,
    const Eigen::Matrix4f& viewProjection, TerrainQualityInvalidation invalidation) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return ready && current.worldGeneration == worldGeneration &&
        !(invalidation.worldGeneration == worldGeneration &&
            invalidation.sequence != current.invalidationSequence) &&
        TerrainQualityViewWithinValidatedDomain(validatedCamera, viewProjection);
}

double TerrainQualityValidation::PoseArrivedMonotonicMs() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return arrivedMs;
}

void TerrainQualityValidation::Revoke()
{
    std::lock_guard<std::mutex> lock(mutex);
    ready = false;
}
}

#include "hair/rod/rodAirDrag.h"

#include <cmath>
#include <stdexcept>

namespace VL::Hair::Rod
{
Vec3 FrozenPointTangent(const State& old, size_t point)
{
    if (old.positions.size() < 2 || point >= old.positions.size())
        throw std::invalid_argument("frozen tangent point is outside the rod");
    Vec3 tangent;
    if (point == 0)
        tangent = old.positions[1] - old.positions[0];
    else if (point + 1 == old.positions.size())
        tangent = old.positions[point] - old.positions[point - 1];
    else
    {
        const Vec3 first = old.positions[point] - old.positions[point - 1];
        const Vec3 second = old.positions[point + 1] - old.positions[point];
        if (!first.allFinite() || !second.allFinite() || first.norm() <= 1.0e-14 ||
            second.norm() <= 1.0e-14)
            throw std::domain_error("frozen tangent has a degenerate edge");
        tangent = first.normalized() + second.normalized();
    }
    if (!tangent.allFinite() || tangent.norm() <= 1.0e-14)
        throw std::domain_error("frozen tangent is not defined");
    return tangent.normalized();
}

AirDragEvaluation EvaluateFrozenTangentAirDrag(const State& old, const State& candidate,
    size_t point, double mass, double stepSeconds, const ProgressiveSupport& profile,
    const Vec3& airVelocityWorld, double forceFraction)
{
    if (candidate.positions.size() != old.positions.size() || point >= candidate.positions.size() ||
        !std::isfinite(mass) || mass <= 0.0 || !std::isfinite(stepSeconds) || stepSeconds <= 0.0 ||
        !std::isfinite(forceFraction) || forceFraction < 0.0 || forceFraction > 1.0 ||
        !std::isfinite(profile.dragAccelerationPerSpeed) || profile.dragAccelerationPerSpeed < 0.0 ||
        !airVelocityWorld.allFinite() || !candidate.positions[point].allFinite())
        throw std::invalid_argument("implicit air drag input is invalid");
    AirDragEvaluation result;
    result.tangent = FrozenPointTangent(old, point);
    const Mat3 projection = Mat3::Identity() - result.tangent * result.tangent.transpose();
    const Vec3 velocity = (candidate.positions[point] - old.positions[point]) / stepSeconds;
    result.relativeNormalVelocity = projection * (airVelocityWorld - velocity);
    const double magnitude = result.relativeNormalVelocity.norm();
    const double coefficient = mass * profile.dragAccelerationPerSpeed * forceFraction;
    result.force = coefficient * magnitude * result.relativeNormalVelocity;
    result.negativeVelocityDerivative = coefficient * magnitude * projection;
    if (magnitude > 0.0)
        result.negativeVelocityDerivative += coefficient *
            result.relativeNormalVelocity * result.relativeNormalVelocity.transpose() / magnitude;
    if (!result.force.allFinite() || !result.negativeVelocityDerivative.allFinite())
        throw std::invalid_argument("implicit air drag result is not finite");
    return result;
}
}

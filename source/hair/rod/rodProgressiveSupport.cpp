#include "hair/rod/rodProgressiveSupport.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "hair/rod/rodRootSupport.h"
#include "hair/tressfxSolverMath.h"

namespace VL::Hair::Rod
{
double EffectiveBundleDragAccelerationPerSpeed(const Material& material)
{
    if (!std::isfinite(material.radius) || material.radius <= 0.0 ||
        !std::isfinite(material.density) || material.density <= 0.0)
        throw std::invalid_argument("effective bundle drag requires positive radius and density");
    return 1.225 * static_cast<double>(TressfxHairDragCoefficient) /
        (Pi * material.density * material.radius);
}

ProgressiveSupport BuildProgressiveSupport(const RestState& rest, const Material& material)
{
    if (!std::isfinite(rest.totalLength) || rest.totalLength <= 0.0 ||
        rest.masses.size() != rest.supportStiffness.size() ||
        rest.masses.size() != rest.inverseMasses.size() ||
        !std::isfinite(material.radius) || material.radius <= 0.0 ||
        !std::isfinite(material.density) || material.density <= 0.0)
        throw std::invalid_argument("progressive support requires valid rest and effective material");
    ProgressiveSupport profile;
    profile.dragAccelerationPerSpeed = EffectiveBundleDragAccelerationPerSpeed(material);
    const double maximumDisplacement = profile.responseLimitRatio * rest.totalLength;
    const double designAcceleration = profile.dragAccelerationPerSpeed *
        profile.designAirSpeed * profile.designAirSpeed;
    for (size_t point = 0; point < rest.masses.size(); ++point)
    {
        if (!std::isfinite(rest.masses[point]) || rest.masses[point] <= 0.0 ||
            !std::isfinite(rest.supportStiffness[point]) || rest.supportStiffness[point] < 0.0)
            throw std::invalid_argument("progressive support mass or stiffness is invalid");
        const double designForce = rest.masses[point] * designAcceleration;
        profile.designForces.push_back(designForce);
        profile.cubicStiffness.push_back(rest.inverseMasses[point] == 0.0 ? 0.0 :
            std::max(0.0, (designForce - rest.supportStiffness[point] * maximumDisplacement) /
                (maximumDisplacement * maximumDisplacement * maximumDisplacement)));
    }
    return profile;
}

ProgressiveNode EvaluateProgressiveNode(
    const Vec3& displacement, double linearStiffness, double cubicStiffness)
{
    if (!displacement.allFinite() || !std::isfinite(linearStiffness) || linearStiffness < 0.0 ||
        !std::isfinite(cubicStiffness) || cubicStiffness < 0.0)
        throw std::invalid_argument("progressive node requires finite displacement and nonnegative stiffness");
    ProgressiveNode result;
    const double squared = displacement.squaredNorm();
    const double stiffness = linearStiffness + cubicStiffness * squared;
    result.potential = 0.5 * linearStiffness * squared +
        0.25 * cubicStiffness * squared * squared;
    result.springForce = -stiffness * displacement;
    result.hessian = stiffness * Mat3::Identity() +
        2.0 * cubicStiffness * displacement * displacement.transpose();
    return result;
}

SupportEvaluation EvaluateProgressiveSupport(const RestState& rest, const State& state,
    const RootFrame& root, const ProgressiveSupport& profile)
{
    if (state.positions.size() != rest.positions.size() ||
        state.velocities.size() != rest.positions.size() ||
        profile.cubicStiffness.size() != rest.positions.size() ||
        rest.supportStiffness.size() != rest.positions.size() ||
        rest.supportDamping.size() != rest.positions.size() ||
        rest.rootLocalPositions.size() != rest.positions.size() ||
        !root.position.allFinite() || !root.linearVelocity.allFinite() ||
        !root.angularVelocityWorld.allFinite() || !root.orientation.coeffs().allFinite() ||
        std::abs(root.orientation.norm() - 1.0) > 1.0e-8)
        throw std::invalid_argument("progressive support dimensions or root frame are invalid");
    SupportEvaluation result;
    result.forces.assign(state.positions.size(), Vec3::Zero());
    for (size_t point = 0; point < state.positions.size(); ++point)
    {
        if (!state.velocities[point].allFinite() ||
            !rest.rootLocalPositions[point].allFinite() ||
            !std::isfinite(rest.supportDamping[point]) || rest.supportDamping[point] < 0.0)
            throw std::invalid_argument("progressive support velocity or damping is invalid");
        const SupportSample sample = SampleRootSupport(root, rest.rootLocalPositions[point]);
        const ProgressiveNode node = EvaluateProgressiveNode(state.positions[point] - sample.position,
            rest.supportStiffness[point], profile.cubicStiffness[point]);
        const Vec3 relativeVelocity = state.velocities[point] - sample.velocity;
        result.forces[point] = node.springForce - rest.supportDamping[point] * relativeVelocity;
        result.potential += node.potential;
        result.drivePower += result.forces[point].dot(sample.velocity);
        result.dissipation += rest.supportDamping[point] * relativeVelocity.squaredNorm();
    }
    return result;
}

Vec3 ProgressiveAirDragForce(const ProgressiveSupport& profile,
    double mass, const Vec3& relativeAirVelocity, const Vec3& unitTangent)
{
    if (!std::isfinite(mass) || mass <= 0.0 || !relativeAirVelocity.allFinite() ||
        !unitTangent.allFinite() || std::abs(unitTangent.norm() - 1.0) > 1.0e-8 ||
        !std::isfinite(profile.dragAccelerationPerSpeed) || profile.dragAccelerationPerSpeed < 0.0)
        throw std::invalid_argument("progressive drag requires finite positive mass and unit tangent");
    const Vec3 transverse = relativeAirVelocity -
        unitTangent * unitTangent.dot(relativeAirVelocity);
    return mass * profile.dragAccelerationPerSpeed * transverse.norm() * transverse;
}
}

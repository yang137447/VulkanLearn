#include "hair/rod/rodWorkCertificate.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodImplicit.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodRootSupport.h"

namespace VL::Hair::Rod
{
ResidualWorkCertificate EvaluateProgressiveResidualWork(const RestState& rest,
    const State& old, const State& candidate, const StepInput& input,
    const ProgressiveSupport& profile)
{
    if (!ValidateImplicitStepInput(rest, old, input) ||
        !ValidateImplicitStepInput(rest, candidate, input) ||
        candidate.positions.size() != old.positions.size() ||
        candidate.orientations.size() != old.orientations.size() ||
        profile.cubicStiffness.size() != rest.positions.size())
        throw std::invalid_argument("residual work certificate dimensions are invalid");
    ResidualWorkCertificate certificate;
    State next = candidate;
    for (size_t point = 0; point < rest.positions.size(); ++point)
        next.velocities[point] = (next.positions[point] - old.positions[point]) / input.stepSeconds;
    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
        next.angularVelocities[segment] =
            Log(old.orientations[segment].conjugate() * next.orientations[segment]) / input.stepSeconds;
    const auto physical = EvaluateProgressiveImplicitResidual(rest, old, next, input, profile);
    const auto oldElastic = EvaluateEnergy(rest, old);
    const auto newElastic = EvaluateEnergy(rest, next);
    const double oldSupport = input.supportEnabled ?
        EvaluateProgressiveSupport(rest, old, input.root, profile).potential : 0.0;
    const double newSupport = input.supportEnabled ?
        EvaluateProgressiveSupport(rest, next, input.root, profile).potential : 0.0;
    double oldPotential = oldElastic.Total() + oldSupport;
    double newPotential = newElastic.Total() + newSupport;
    double oldLinear = 0.0;
    double newLinear = 0.0;
    double oldAngular = 0.0;
    double newAngular = 0.0;
    double linearDiscrete = 0.0;
    double angularWork = 0.0;
    double externalWork = 0.0;
    double dampingWork = 0.0;
    double conservativeWork = 0.0;
    double massSum = 0.0;
    double maximumStiffness = 0.0;
    const double infinity = std::numeric_limits<double>::infinity();
    auto accumulateUpper = [&](const Vec3& residual, const Vec3& increment)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const double term = std::nextafter(
                std::abs(residual[axis]) * std::abs(increment[axis]), infinity);
            certificate.freeResidualWorkUpper =
                std::nextafter(certificate.freeResidualWorkUpper + term, infinity);
        }
    };
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        const double mass = rest.masses[point];
        massSum += mass;
        maximumStiffness = std::max(maximumStiffness, rest.supportStiffness[point]);
        const Vec3 delta = next.positions[point] - old.positions[point];
        oldLinear += 0.5 * mass * old.velocities[point].squaredNorm();
        newLinear += 0.5 * mass * next.velocities[point].squaredNorm();
        linearDiscrete += 0.5 * mass * (next.velocities[point] - old.velocities[point]).squaredNorm();
        oldPotential -= mass * input.gravity.dot(old.positions[point]);
        newPotential -= mass * input.gravity.dot(next.positions[point]);
        const Vec3 external = input.externalForces.empty() ?
            Vec3::Zero().eval() : input.externalForces[point];
        externalWork += external.dot(delta);
        Vec3 springForce = Vec3::Zero();
        if (input.supportEnabled)
        {
            const auto target = SampleRootSupport(input.root, rest.rootLocalPositions[point]);
            springForce = EvaluateProgressiveNode(next.positions[point] - target.position,
                rest.supportStiffness[point], profile.cubicStiffness[point]).springForce;
            dampingWork += input.stepSeconds * rest.supportDamping[point] *
                (next.velocities[point] - target.velocity).dot(next.velocities[point]);
        }
        conservativeWork += (newElastic.forces[point] + springForce + mass * input.gravity).dot(delta);
        const double residualWork = physical[point].head<3>().dot(delta);
        if (rest.inverseMasses[point] == 0.0)
            certificate.rootConstraintWork += residualWork;
        else
        {
            certificate.freeResidualWork += residualWork;
            accumulateUpper(physical[point].head<3>(), delta);
        }
    }
    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
    {
        const Vec3 theta = Log(old.orientations[segment].conjugate() * next.orientations[segment]);
        const Vec3 transported = next.orientations[segment].conjugate() *
            (old.orientations[segment] *
                rest.inertias[segment].cwiseProduct(old.angularVelocities[segment]));
        oldAngular += 0.5 * old.angularVelocities[segment].dot(
            rest.inertias[segment].cwiseProduct(old.angularVelocities[segment]));
        newAngular += 0.5 * next.angularVelocities[segment].dot(
            rest.inertias[segment].cwiseProduct(next.angularVelocities[segment]));
        angularWork += next.angularVelocities[segment].dot(
            rest.inertias[segment].cwiseProduct(next.angularVelocities[segment]) - transported);
        conservativeWork += newElastic.bodyTorques[segment].dot(theta);
        const Vec3 external = input.externalTorquesWorld.empty() ?
            Vec3::Zero().eval() : input.externalTorquesWorld[segment];
        externalWork += (next.orientations[segment].conjugate() * external).dot(theta);
        const double residualWork = physical[segment].tail<3>().dot(theta);
        if (segment == 0)
            certificate.rootConstraintWork += residualWork;
        else
        {
            certificate.freeResidualWork += residualWork;
            accumulateUpper(physical[segment].tail<3>(), theta);
        }
    }
    certificate.energyReference =
        (massSum * 9.8 + maximumStiffness * rest.totalLength) * rest.totalLength;
    const double remainder = newPotential - oldPotential + conservativeWork;
    const double angularDiscrete = angularWork - (newAngular - oldAngular);
    certificate.frozenTargetBalance = newLinear + newAngular + newPotential -
        oldLinear - oldAngular - oldPotential - externalWork + dampingWork +
        linearDiscrete + angularDiscrete - remainder - certificate.rootConstraintWork;
    certificate.ledgerScale = certificate.energyReference +
        std::abs(newLinear + newAngular + newPotential) +
        std::abs(oldLinear + oldAngular + oldPotential) + std::abs(externalWork) +
        std::abs(dampingWork) + std::abs(linearDiscrete) + std::abs(angularDiscrete) +
        std::abs(remainder) + std::abs(certificate.rootConstraintWork);
    certificate.workBoundScaled = certificate.freeResidualWorkUpper / certificate.energyReference;
    certificate.balanceScaled = std::abs(certificate.frozenTargetBalance) / certificate.ledgerScale;
    certificate.identityScaled =
        std::abs(certificate.frozenTargetBalance - certificate.freeResidualWork) / certificate.ledgerScale;
    certificate.finite = std::isfinite(certificate.workBoundScaled) &&
        std::isfinite(certificate.balanceScaled) && std::isfinite(certificate.identityScaled);
    certificate.accepted = certificate.finite && certificate.workBoundScaled <= 5.0e-11 &&
        certificate.balanceScaled <= 1.0e-10 && certificate.identityScaled <= 1.0e-10;
    return certificate;
}
}

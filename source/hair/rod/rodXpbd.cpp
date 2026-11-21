#include "hair/rod/rodXpbd.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <Eigen/Cholesky>

#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodRootSupport.h"

namespace VL::Hair::Rod
{
namespace
{
Vec3 SolveProjection(const Mat3& metric, const Mat3& compliance,
    const Vec3& constraint, const Vec3& multiplier, double stepSeconds)
{
    const Mat3 scaled = compliance / (stepSeconds * stepSeconds);
    const Mat3 system = metric + scaled;
    const Eigen::LDLT<Mat3> factorization(system);
    if (factorization.info() != Eigen::Success || !factorization.isPositive())
        throw std::runtime_error("rod projection matrix is not positive definite");
    const Vec3 result = factorization.solve(-constraint - scaled * multiplier);
    if (!result.allFinite())
        throw std::runtime_error("rod projection is non-finite");
    return result;
}

bool ValidInput(const RestState& rest, const State& state, const StepInput& input)
{
    const size_t points = rest.positions.size();
    const size_t segments = rest.lengths.size();
    if (points < 2 || segments + 1 != points ||
        state.positions.size() != points || state.velocities.size() != points ||
        state.orientations.size() != segments || state.angularVelocities.size() != segments ||
        rest.masses.size() != points || rest.inverseMasses.size() != points ||
        rest.orientations.size() != segments || rest.restStrains.size() != segments ||
        rest.restJointAngles.size() + 1 != segments ||
        rest.stretchShear.size() != segments || rest.bendTwist.size() != segments ||
        rest.inertias.size() != segments || rest.inverseInertias.size() != segments ||
        rest.rootLocalPositions.size() != points || rest.supportStiffness.size() != points ||
        rest.supportDamping.size() != points || input.sweeps == 0 ||
        !std::isfinite(input.stepSeconds) || input.stepSeconds <= 0.0 ||
        !std::isfinite(input.contactMargin) || input.contactMargin < 0.0 ||
        !std::isfinite(input.contactCompliance) || input.contactCompliance < 0.0 ||
        !input.gravity.allFinite() || !input.root.position.allFinite() ||
        !input.root.linearVelocity.allFinite() || !input.root.angularVelocityWorld.allFinite() ||
        !input.root.orientation.coeffs().allFinite() ||
        std::abs(input.root.orientation.norm() - 1.0) > 1.0e-8 ||
        (!input.externalForces.empty() && input.externalForces.size() != points) ||
        (!input.externalTorquesWorld.empty() && input.externalTorquesWorld.size() != segments))
        return false;
    for (size_t point = 0; point < points; ++point)
        if (!state.positions[point].allFinite() || !state.velocities[point].allFinite() ||
            !std::isfinite(rest.masses[point]) || rest.masses[point] <= 0.0 ||
            !std::isfinite(rest.inverseMasses[point]) || rest.inverseMasses[point] < 0.0 ||
            !std::isfinite(rest.supportStiffness[point]) || rest.supportStiffness[point] < 0.0 ||
            !std::isfinite(rest.supportDamping[point]) || rest.supportDamping[point] < 0.0 ||
            !rest.rootLocalPositions[point].allFinite() ||
            (!input.externalForces.empty() && !input.externalForces[point].allFinite()))
            return false;
    for (size_t segment = 0; segment < segments; ++segment)
        if (!state.orientations[segment].coeffs().allFinite() ||
            std::abs(state.orientations[segment].norm() - 1.0) > 1.0e-8 ||
            !state.angularVelocities[segment].allFinite() ||
            !std::isfinite(rest.lengths[segment]) || rest.lengths[segment] <= 0.0 ||
            !rest.inertias[segment].allFinite() || (rest.inertias[segment].array() <= 0.0).any() ||
            !rest.inverseInertias[segment].allFinite() ||
            (rest.inverseInertias[segment].array() < 0.0).any() ||
            !rest.stretchShear[segment].allFinite() ||
            (rest.stretchShear[segment].array() <= 0.0).any() ||
            !rest.bendTwist[segment].allFinite() || (rest.bendTwist[segment].array() <= 0.0).any() ||
            !rest.restStrains[segment].allFinite() ||
            (!input.externalTorquesWorld.empty() && !input.externalTorquesWorld[segment].allFinite()))
            return false;
    for (const Vec3& angle : rest.restJointAngles)
        if (!angle.allFinite() || angle.norm() >= RotationLimit)
            return false;
    if (rest.inverseMasses.front() != 0.0 || rest.inverseInertias.front().squaredNorm() != 0.0)
        return false;
    return true;
}
}

StepDiagnostics Step(const RestState& rest, State& state, const StepInput& input)
{
    StepDiagnostics diagnostics;
    if (!ValidInput(rest, state, input))
    {
        diagnostics.status = Status::InvalidInput;
        return diagnostics;
    }
    const State old = state;
    State candidate = state;
    const double stepSeconds = input.stepSeconds;
    const size_t segments = rest.lengths.size();
    std::vector<Vec3> shearMultipliers(segments, Vec3::Zero());
    std::vector<Vec3> bendMultipliers(segments - 1, Vec3::Zero());
    std::vector<Vec3> supportMultipliers(rest.positions.size(), Vec3::Zero());
    std::vector<CollisionSdfQuery> contacts;
    for (const CollisionSphere& sphere : input.collisionSpheres)
        contacts.emplace_back([sphere](const Vec3& position)
            { return SampleCollisionSphere(position, sphere); });
    for (const CollisionCapsule& capsule : input.collisionCapsules)
        contacts.emplace_back([capsule](const Vec3& position)
            { return SampleCollisionCapsule(position, capsule); });
    contacts.insert(contacts.end(), input.collisionFields.begin(), input.collisionFields.end());
    std::vector<std::vector<double>> contactMultipliers(contacts.size(),
        std::vector<double>(rest.positions.size(), 0.0));
    std::vector<Vec3> contactCorrections(rest.positions.size(), Vec3::Zero());
    try
    {
        for (size_t point = 1; point < candidate.positions.size(); ++point)
        {
            const SupportSample target = SampleRootSupport(input.root, rest.rootLocalPositions[point]);
            if (rest.inverseMasses[point] == 0.0)
            {
                candidate.positions[point] = target.position;
                continue;
            }
            const double dampingRate = input.supportEnabled
                ? rest.supportDamping[point] / rest.masses[point] : 0.0;
            const Vec3 external = input.externalForces.empty()
                ? Vec3::Zero().eval() : input.externalForces[point];
            candidate.velocities[point] = (old.velocities[point] + stepSeconds *
                (input.gravity + external / rest.masses[point] + dampingRate * target.velocity)) /
                (1.0 + stepSeconds * dampingRate);
            candidate.positions[point] = old.positions[point] + stepSeconds * candidate.velocities[point];
        }
        for (size_t segment = 1; segment < segments; ++segment)
        {
            const Vec3 torqueWorld = input.externalTorquesWorld.empty()
                ? Vec3::Zero().eval() : input.externalTorquesWorld[segment];
            double residual = 0.0;
            Vec3 predicted;
            try
            {
                predicted = PredictBodyAngularVelocity(rest.inertias[segment],
                    old.angularVelocities[segment], old.orientations[segment].conjugate() *
                        torqueWorld, stepSeconds, residual);
            }
            catch (const std::runtime_error&)
            {
                diagnostics.status = Status::PredictorFailure;
                return diagnostics;
            }
            diagnostics.predictorResidual = std::max(diagnostics.predictorResidual, residual);
            candidate.orientations[segment] = ApplyBodyIncrement(
                old.orientations[segment], stepSeconds * predicted);
        }
        candidate.positions.front() = input.root.position;
        candidate.orientations.front() = input.root.orientation;
        for (unsigned sweep = 0; sweep < input.sweeps; ++sweep)
        {
            for (size_t color = 0; color < 2; ++color)
                for (size_t segment = color; segment < segments; segment += 2)
                {
                    const ShearBlock block = EvaluateShearBlock(rest, candidate, segment);
                    const Mat3 angularWeight = rest.inverseInertias[segment].asDiagonal();
                    const Mat3 metric =
                        rest.inverseMasses[segment] * block.firstPositionJacobian *
                            block.firstPositionJacobian.transpose() +
                        rest.inverseMasses[segment + 1] * block.secondPositionJacobian *
                            block.secondPositionJacobian.transpose() +
                        block.orientationJacobian * angularWeight *
                            block.orientationJacobian.transpose();
                    const Vec3 delta = SolveProjection(metric, block.compliance, block.constraint,
                        shearMultipliers[segment], stepSeconds);
                    candidate.positions[segment] += rest.inverseMasses[segment] *
                        block.firstPositionJacobian.transpose() * delta;
                    candidate.positions[segment + 1] += rest.inverseMasses[segment + 1] *
                        block.secondPositionJacobian.transpose() * delta;
                    const Vec3 angularDelta = angularWeight *
                        block.orientationJacobian.transpose() * delta;
                    candidate.orientations[segment] = ApplyBodyIncrement(
                        candidate.orientations[segment], angularDelta);
                    shearMultipliers[segment] += delta;
                }
            for (size_t color = 0; color < 2; ++color)
                for (size_t joint = color; joint + 1 < segments; joint += 2)
                {
                    const BendBlock block = EvaluateBendBlock(rest, candidate, joint);
                    const Mat3 firstWeight = rest.inverseInertias[joint].asDiagonal();
                    const Mat3 secondWeight = rest.inverseInertias[joint + 1].asDiagonal();
                    const Mat3 metric = block.firstOrientationJacobian * firstWeight *
                        block.firstOrientationJacobian.transpose() +
                        block.secondOrientationJacobian * secondWeight *
                            block.secondOrientationJacobian.transpose();
                    const Vec3 delta = SolveProjection(metric, block.compliance, block.constraint,
                        bendMultipliers[joint], stepSeconds);
                    candidate.orientations[joint] = ApplyBodyIncrement(candidate.orientations[joint],
                        firstWeight * block.firstOrientationJacobian.transpose() * delta);
                    candidate.orientations[joint + 1] = ApplyBodyIncrement(
                        candidate.orientations[joint + 1],
                        secondWeight * block.secondOrientationJacobian.transpose() * delta);
                    bendMultipliers[joint] += delta;
                }
            if (input.supportEnabled)
                for (size_t point = 1; point < candidate.positions.size(); ++point)
                {
                    const double stiffness = rest.supportStiffness[point];
                    if (stiffness <= 0.0)
                        continue;
                    const SupportSample target = SampleRootSupport(input.root, rest.rootLocalPositions[point]);
                    const Vec3 constraint = candidate.positions[point] - target.position;
                    const Vec3 delta = SolveProjection(
                        rest.inverseMasses[point] * Mat3::Identity(),
                        Mat3::Identity() / stiffness, constraint,
                        supportMultipliers[point], stepSeconds);
                    candidate.positions[point] += rest.inverseMasses[point] * delta;
                    supportMultipliers[point] += delta;
                }
            for (size_t contact = 0; contact < contacts.size(); ++contact)
                for (size_t point = 0; point < candidate.positions.size(); ++point)
                {
                    if (rest.inverseMasses[point] == 0.0)
                        continue;
                    const CollisionSample sample = SampleCollisionSdf(
                        candidate.positions[point], contacts[contact]);
                    const ContactProjection projection = ProjectContact(candidate.positions[point],
                        candidate.velocities[point], rest.inverseMasses[point], stepSeconds,
                        input.contactMargin, input.contactCompliance,
                        contactMultipliers[contact][point], sample);
                    candidate.positions[point] = projection.position;
                    contactMultipliers[contact][point] = projection.lambda;
                    contactCorrections[point] += projection.correction;
                }
            ++diagnostics.completedSweeps;
        }
        for (size_t point = 0; point < candidate.positions.size(); ++point)
        {
            candidate.velocities[point] = ReconstructContactVelocity(old.positions[point],
                candidate.positions[point], contactCorrections[point], stepSeconds);
            if (rest.inverseMasses[point] > 0.0)
                for (size_t contact = 0; contact < contacts.size(); ++contact)
                {
                    const CollisionSample sample = SampleCollisionSdf(candidate.positions[point], contacts[contact]);
                    const bool active = sample.valid &&
                        (contactMultipliers[contact][point] > 0.0 ||
                            sample.distance <= input.contactMargin + 1.0e-10);
                    candidate.velocities[point] = StabilizeContactVelocity(candidate.velocities[point],
                        rest.inverseMasses[point], sample, active);
                    if (sample.valid)
                        diagnostics.maximumPenetration = std::max(diagnostics.maximumPenetration,
                            input.contactMargin - sample.distance);
                }
            diagnostics.maximumContactCorrection = std::max(diagnostics.maximumContactCorrection,
                contactCorrections[point].norm());
        }
        for (size_t segment = 0; segment < segments; ++segment)
            candidate.angularVelocities[segment] = Log(old.orientations[segment].conjugate() *
                candidate.orientations[segment]) / stepSeconds;
        const EnergyEvaluation energy = EvaluateEnergy(rest, candidate);
        SupportEvaluation support;
        support.forces.assign(candidate.positions.size(), Vec3::Zero());
        if (input.supportEnabled)
            support = EvaluateRootSupport(rest, candidate, input.root);
        diagnostics.energy = energy.Total() + support.potential;
        diagnostics.supportDrivePower = support.drivePower;
        diagnostics.supportDissipation = support.dissipation;
        for (size_t point = 0; point < candidate.positions.size(); ++point)
        {
            const Vec3 external = input.externalForces.empty()
                ? Vec3::Zero().eval() : input.externalForces[point];
            const Vec3 residual = rest.masses[point] *
                (candidate.velocities[point] - old.velocities[point]) / stepSeconds -
                energy.forces[point] - support.forces[point] -
                rest.masses[point] * input.gravity - external;
            diagnostics.energy -= rest.masses[point] * input.gravity.dot(candidate.positions[point]);
            diagnostics.linearKineticEnergy +=
                0.5 * rest.masses[point] * candidate.velocities[point].squaredNorm();
            diagnostics.maximumForce = std::max(diagnostics.maximumForce,
                (energy.forces[point] + support.forces[point]).norm());
            if (rest.inverseMasses[point] == 0.0)
            {
                diagnostics.rootForce += residual;
                diagnostics.rootBodyTorque += input.root.orientation.conjugate() *
                    ((candidate.positions[point] - input.root.position).cross(residual));
            }
            else
                diagnostics.maximumLinearResidual =
                    std::max(diagnostics.maximumLinearResidual, residual.norm());
            if (input.supportEnabled && rest.supportStiffness[point] > 0.0)
            {
                const Vec3 constraint = candidate.positions[point] -
                    SampleRootSupport(input.root, rest.rootLocalPositions[point]).position;
                diagnostics.maximumSupportDefect = std::max(diagnostics.maximumSupportDefect,
                    (constraint + supportMultipliers[point] /
                        (rest.supportStiffness[point] * stepSeconds * stepSeconds)).norm());
            }
        }
        for (size_t segment = 0; segment < segments; ++segment)
        {
            const Vec3 torqueWorld = input.externalTorquesWorld.empty()
                ? Vec3::Zero().eval() : input.externalTorquesWorld[segment];
            const Vec3 transportedMomentum = candidate.orientations[segment].conjugate() *
                (old.orientations[segment] *
                    rest.inertias[segment].cwiseProduct(old.angularVelocities[segment]));
            const Vec3 residual = (rest.inertias[segment].cwiseProduct(
                candidate.angularVelocities[segment]) - transportedMomentum) / stepSeconds -
                energy.bodyTorques[segment] - candidate.orientations[segment].conjugate() * torqueWorld;
            diagnostics.maximumBodyTorque = std::max(diagnostics.maximumBodyTorque,
                energy.bodyTorques[segment].norm());
            diagnostics.angularKineticEnergy += 0.5 * candidate.angularVelocities[segment].dot(
                rest.inertias[segment].cwiseProduct(candidate.angularVelocities[segment]));
            if (segment == 0)
                diagnostics.rootBodyTorque += residual;
            else
                diagnostics.maximumAngularResidual =
                    std::max(diagnostics.maximumAngularResidual, residual.norm());
            const ShearBlock block = EvaluateShearBlock(rest, candidate, segment);
            diagnostics.maximumShearDefect = std::max(diagnostics.maximumShearDefect,
                (block.constraint + block.compliance * shearMultipliers[segment] /
                    (stepSeconds * stepSeconds)).norm());
        }
        for (size_t joint = 0; joint + 1 < segments; ++joint)
        {
            const BendBlock block = EvaluateBendBlock(rest, candidate, joint);
            diagnostics.maximumBendDefect = std::max(diagnostics.maximumBendDefect,
                (block.constraint + block.compliance * bendMultipliers[joint] /
                    (stepSeconds * stepSeconds)).norm());
        }
        diagnostics.converged = diagnostics.maximumShearDefect < 1.0e-6 &&
            diagnostics.maximumBendDefect < 1.0e-6 && diagnostics.maximumSupportDefect < 1.0e-8;
        diagnostics.shearMultipliers = shearMultipliers;
        diagnostics.bendMultipliers = bendMultipliers;
        diagnostics.supportMultipliers = supportMultipliers;
        if (!std::isfinite(diagnostics.energy) ||
            !std::isfinite(diagnostics.maximumAngularResidual))
        {
            diagnostics.status = Status::NonFinite;
            return diagnostics;
        }
        for (size_t point = 0; point < candidate.positions.size(); ++point)
            if (!candidate.positions[point].allFinite() || !candidate.velocities[point].allFinite())
            {
                diagnostics.status = Status::NonFinite;
                return diagnostics;
            }
        for (size_t segment = 0; segment < segments; ++segment)
            if (!candidate.orientations[segment].coeffs().allFinite() ||
                !candidate.angularVelocities[segment].allFinite())
            {
                diagnostics.status = Status::NonFinite;
                return diagnostics;
            }
        state = std::move(candidate);
    }
    catch (const std::domain_error&)
    {
        diagnostics.status = Status::DomainFailure;
    }
    catch (const std::runtime_error&)
    {
        diagnostics.status = Status::LinearSolveFailure;
    }
    return diagnostics;
}
}

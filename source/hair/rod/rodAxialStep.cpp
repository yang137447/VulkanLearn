#include "hair/rod/rodAxialStep.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "hair/rod/rodAirDrag.h"
#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodRootSupport.h"

namespace VL::Hair::Rod
{
namespace
{
std::pair<double, double> Merit(const AxialContactSystem& system)
{
    double sum = 0.0;
    double maximum = 0.0;
    for (const auto& row : system.residual)
    {
        if (!row.allFinite())
            return {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
        sum += row.squaredNorm();
        maximum = std::max(maximum, row.cwiseAbs().maxCoeff());
    }
    return {sum, maximum};
}

AxialStepDiagnostics SolveStage(const RestState& rest, const State& old, State& candidate,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, const FrozenTangentAir* air,
    unsigned budget, bool requireWork, bool fallback, double fraction, AxialStepTrace* trace)
{
    AxialStepDiagnostics result;
    AxialContactActiveSet active;
    const size_t count = rest.positions.size();
    active.contacts.resize(count);
    active.tensileActive.assign(count - 1, false);
    active.tensileMultipliers.assign(count - 1, 0.0);
    std::vector<bool> edgeExcluded(count - 1, false);
    std::vector<std::vector<bool>> contactExcluded(count, std::vector<bool>(constraints.size(), false));
    auto finish = [&]()
    {
        if (trace)
        {
            trace->finalCandidate = candidate;
            trace->finalActive = active;
            trace->failureReason = result.failureReason;
        }
        return result;
    };
    try
    {
        for (size_t point = 0; point < count; ++point)
            if (rest.inverseMasses[point] == 0.0)
                candidate.positions[point] =
                    SampleRootSupport(input.root, rest.rootLocalPositions[point]).position;
        candidate.orientations.front() = input.root.orientation;
        for (unsigned iteration = 0; iteration <= budget; ++iteration)
        {
            unsigned dependent = 0;
            active.contacts = SelectContactActiveSet(rest, candidate, input, constraints,
                active.contacts, contactExcluded, dependent);
            for (size_t edge = 0; edge + 1 < count; ++edge)
            {
                const auto value = EvaluateAxialLimit(old.positions[edge], old.positions[edge + 1],
                    candidate.positions[edge], candidate.positions[edge + 1],
                    rest.lengths[edge], active.tensileMultipliers[edge]);
                if (rest.inverseMasses[edge] == 0.0 && rest.inverseMasses[edge + 1] == 0.0)
                {
                    if (value.gap < -1.0e-9 * rest.totalLength)
                        throw std::domain_error("prescribed endpoints violate tensile limit");
                    active.tensileActive[edge] = false;
                    active.tensileMultipliers[edge] = 0.0;
                    continue;
                }
                const bool penetrates = value.gap < -1.0e-9 * rest.totalLength;
                const bool retained = active.tensileActive[edge] &&
                    active.tensileMultipliers[edge] > 0.0 && !edgeExcluded[edge];
                active.tensileActive[edge] = penetrates ||
                    (!edgeExcluded[edge] && value.gap <= 1.0e-9 * rest.totalLength) || retained;
                if (active.tensileActive[edge] && penetrates)
                    edgeExcluded[edge] = false;
                if (!active.tensileActive[edge])
                    active.tensileMultipliers[edge] = 0.0;
            }
            const auto system = AssembleAxialContactSystem(rest, old, candidate, input, profile,
                constraints, active, air);
            const auto merit = Merit(system);
            result.normalizedResidual = merit.second;
            if (trace)
                trace->iterations.push_back({candidate, active, system, {}, {}, fraction, merit.second, fallback});
            bool feasible = true;
            bool negativeReaction = false;
            result.maximumTensileViolation = 0.0;
            result.maximumContactPenetration = 0.0;
            result.maximumComplementarity = 0.0;
            StepInput workInput = input;
            if (workInput.externalForces.empty())
                workInput.externalForces.assign(count, Vec3::Zero());
            result.tensileWork = 0.0;
            result.contactWork = 0.0;
            for (size_t edge = 0; edge + 1 < count; ++edge)
            {
                const auto value = EvaluateAxialLimit(old.positions[edge], old.positions[edge + 1],
                    candidate.positions[edge], candidate.positions[edge + 1],
                    rest.lengths[edge], active.tensileMultipliers[edge]);
                result.maximumTensileViolation = std::max(result.maximumTensileViolation, -value.gap);
                const double complementarity = std::abs(active.tensileMultipliers[edge] * value.gap) /
                    (system.forceScale * system.lengthScale);
                result.maximumComplementarity = std::max(result.maximumComplementarity, complementarity);
                if (active.tensileMultipliers[edge] < -1.0e-9 * system.forceScale)
                {
                    edgeExcluded[edge] = true;
                    feasible = false;
                    negativeReaction = true;
                }
                workInput.externalForces[edge] += value.force.head<3>();
                workInput.externalForces[edge + 1] += value.force.tail<3>();
                result.tensileWork += value.work;
            }
            for (size_t point = 0; point < count; ++point)
            {
                if (air)
                    workInput.externalForces[point] += EvaluateFrozenTangentAirDrag(old, candidate,
                        point, rest.masses[point], input.stepSeconds, profile,
                        air->velocityWorld, air->forceFraction).force;
                if (rest.inverseMasses[point] == 0.0)
                    continue;
                for (size_t identity = 0; identity < constraints.size(); ++identity)
                {
                    const auto geometry = constraints[identity].sample(candidate.positions[point]);
                    if (!geometry.valid)
                        continue;
                    if (!std::isfinite(geometry.distance) || !geometry.gradient.allFinite() ||
                        geometry.gradient.norm() <= 1.0e-14)
                        throw std::domain_error("axial contact geometry is invalid");
                    const double gap = geometry.distance - constraints[identity].distanceScale * input.contactMargin;
                    result.maximumContactPenetration = std::max(result.maximumContactPenetration,
                        -gap / geometry.gradient.norm());
                    for (int slot = 0; slot < 3; ++slot)
                        if (active.contacts[point].identities[slot] == static_cast<int>(identity))
                        {
                            const double multiplier = active.contacts[point].multipliers[slot];
                            const Vec3 force = multiplier * geometry.gradient;
                            result.maximumComplementarity = std::max(result.maximumComplementarity,
                                std::abs(multiplier * gap) / (system.forceScale * system.lengthScale));
                            if (multiplier < -1.0e-9 * system.forceScale)
                            {
                                contactExcluded[point][identity] = true;
                                feasible = false;
                                negativeReaction = true;
                            }
                            workInput.externalForces[point] += force;
                            result.contactWork += force.dot(candidate.positions[point] - old.positions[point]);
                        }
                }
            }
            feasible = feasible && result.maximumTensileViolation <= 1.0e-9 * system.lengthScale &&
                result.maximumContactPenetration <= 1.0e-9 * system.lengthScale &&
                result.maximumComplementarity <= 1.0e-9;
            if (merit.second <= 1.0e-8 && feasible)
            {
                result.workCertificate = EvaluateProgressiveResidualWork(rest, old, candidate, workInput, profile);
                if (!requireWork || result.workCertificate.accepted)
                {
                    for (size_t point = 0; point < count; ++point)
                        candidate.velocities[point] =
                            (candidate.positions[point] - old.positions[point]) / input.stepSeconds;
                    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
                        candidate.angularVelocities[segment] =
                            Log(old.orientations[segment].conjugate() * candidate.orientations[segment]) /
                            input.stepSeconds;
                    const auto physical = EvaluateProgressiveImplicitResidual(rest, old, candidate, workInput, profile);
                    const auto elastic = EvaluateEnergy(rest, candidate);
                    const auto support = input.supportEnabled ?
                        EvaluateProgressiveSupport(rest, candidate, input.root, profile) : SupportEvaluation{};
                    result.energy = elastic.Total() + support.potential;
                    result.supportDrivePower = support.drivePower;
                    result.supportDissipation = support.dissipation;
                    for (size_t point = 0; point < count; ++point)
                    {
                        result.energy -= rest.masses[point] * input.gravity.dot(candidate.positions[point]);
                        result.linearKineticEnergy += 0.5 * rest.masses[point] *
                            candidate.velocities[point].squaredNorm();
                        result.maximumForce = std::max(result.maximumForce, elastic.forces[point].norm());
                        if (rest.inverseMasses[point] == 0.0)
                        {
                            result.rootForce += physical[point].head<3>();
                            result.rootBodyTorque += input.root.orientation.conjugate() *
                                ((candidate.positions[point] - input.root.position).cross(physical[point].head<3>()));
                        }
                        else
                            result.maximumLinearResidual = std::max(result.maximumLinearResidual,
                                physical[point].head<3>().norm());
                    }
                    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
                    {
                        result.angularKineticEnergy += 0.5 * candidate.angularVelocities[segment].dot(
                            rest.inertias[segment].cwiseProduct(candidate.angularVelocities[segment]));
                        result.maximumBodyTorque = std::max(result.maximumBodyTorque,
                            elastic.bodyTorques[segment].norm());
                        if (segment == 0)
                            result.rootBodyTorque += physical[segment].tail<3>();
                        else
                            result.maximumAngularResidual = std::max(result.maximumAngularResidual,
                                physical[segment].tail<3>().norm());
                    }
                    result.maximumShearDefect = std::numeric_limits<double>::quiet_NaN();
                    result.maximumBendDefect = std::numeric_limits<double>::quiet_NaN();
                    result.maximumSupportDefect = std::numeric_limits<double>::quiet_NaN();
                    if (!std::isfinite(result.energy) || !std::isfinite(result.linearKineticEnergy) ||
                        !std::isfinite(result.angularKineticEnergy) || !result.rootForce.allFinite() ||
                        !result.rootBodyTorque.allFinite())
                    {
                        result.status = Status::NonFinite;
                        result.failureReason = "axial-contact-final-nonfinite";
                        return finish();
                    }
                    result.converged = true;
                    return finish();
                }
            }
            if (iteration == budget)
                break;
            if (merit.second <= 1.0e-8 && negativeReaction)
                continue;
            const auto direction = SolveAxialContactSystem(system);
            if (trace)
                trace->iterations.back().direction = direction;
            for (size_t point = 0; point < count; ++point)
            {
                AxialContactVector remaining = system.diagonal[point] * direction[point] + system.residual[point];
                if (point > 0)
                    remaining += system.lower[point] * direction[point - 1];
                if (point + 1 < count)
                    remaining += system.upper[point] * direction[point + 1];
                result.linearSolveResidual = std::max(result.linearSolveResidual,
                    remaining.cwiseAbs().maxCoeff());
            }
            bool accepted = false;
            double alpha = 1.0;
            for (unsigned trialIndex = 0; trialIndex <= 20; ++trialIndex)
            {
                try
                {
                    auto trial = candidate;
                    auto trialActive = active;
                    for (size_t point = 0; point < count; ++point)
                    {
                        trial.positions[point] += alpha * system.lengthScale * direction[point].head<3>();
                        if (point < rest.lengths.size())
                            trial.orientations[point] = ApplyBodyIncrement(candidate.orientations[point],
                                alpha * direction[point].segment<3>(3));
                        trialActive.contacts[point].multipliers +=
                            alpha * system.forceScale * direction[point].segment<3>(6);
                        if (point > 0)
                            trialActive.tensileMultipliers[point - 1] +=
                                alpha * system.forceScale * direction[point][9];
                    }
                    const auto trialMerit = Merit(AssembleAxialContactSystem(rest, old, trial, input,
                        profile, constraints, trialActive, air));
                    const bool accept = std::isfinite(trialMerit.first) &&
                        trialMerit.first <= (1.0 - 1.0e-4 * alpha) * merit.first;
                    if (trace)
                        trace->iterations.back().trials.push_back(
                            {alpha, trialMerit.first, trialMerit.second, false, accept});
                    if (accept)
                    {
                        candidate = std::move(trial);
                        active = std::move(trialActive);
                        accepted = true;
                        break;
                    }
                }
                catch (const std::domain_error&)
                {
                    if (trace)
                        trace->iterations.back().trials.push_back({alpha,
                            std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::infinity(), true, false});
                }
                alpha *= 0.5;
                ++result.backtracks;
            }
            ++result.newtonIterations;
            if (!accepted)
            {
                result.failureReason = "axial-contact-armijo";
                result.status = Status::PredictorFailure;
                return finish();
            }
        }
        result.failureReason = "axial-contact-update-budget";
        result.status = Status::PredictorFailure;
    }
    catch (const std::domain_error& error)
    {
        result.status = Status::DomainFailure;
        result.failureReason = error.what();
    }
    catch (const std::invalid_argument& error)
    {
        result.status = Status::InvalidInput;
        result.failureReason = error.what();
    }
    catch (const std::runtime_error& error)
    {
        result.status = Status::LinearSolveFailure;
        result.failureReason = error.what();
    }
    return finish();
}
}

AxialStepDiagnostics StepAxialContactImplicit(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, const FrozenTangentAir* air, AxialStepTrace* trace)
{
    AxialStepDiagnostics result;
    if (!ValidateImplicitStepInput(rest, state, input) ||
        profile.cubicStiffness.size() != rest.positions.size() ||
        !input.collisionSpheres.empty() || !input.collisionCapsules.empty() || !input.collisionFields.empty() ||
        !std::isfinite(input.contactMargin) || input.contactMargin < 0.0 ||
        !std::isfinite(profile.dragAccelerationPerSpeed) || profile.dragAccelerationPerSpeed < 0.0 ||
        (air && (!air->velocityWorld.allFinite() || !std::isfinite(air->forceFraction) ||
            air->forceFraction < 0.0 || air->forceFraction > 1.0)))
    {
        result.status = Status::InvalidInput;
        return result;
    }
    for (double stiffness : profile.cubicStiffness)
        if (!std::isfinite(stiffness) || stiffness < 0.0)
        {
            result.status = Status::InvalidInput;
            return result;
        }
    for (const auto& constraint : constraints)
        if (constraint.identity.empty() || !constraint.sample ||
            !std::isfinite(constraint.distanceScale) || constraint.distanceScale <= 0.0)
        {
            result.status = Status::InvalidInput;
            return result;
        }
    const State old = state;
    if (trace)
    {
        *trace = AxialStepTrace{};
        trace->old = old;
        trace->finalCandidate = old;
    }
    State candidate = old;
    result = SolveStage(rest, old, candidate, input, profile, constraints, air, 24, true, false, 1.0, trace);
    const unsigned direct = result.newtonIterations;
    const unsigned directBacktracks = result.backtracks;
    double linearResidual = result.linearSolveResidual;
    if (result.status == Status::Success)
    {
        result.directIterations = direct;
        state = std::move(candidate);
        return result;
    }
    candidate = old;
    unsigned fallbackIterations = 0;
    unsigned backtracks = directBacktracks;
    for (unsigned stage = 0; stage <= 8; ++stage)
    {
        const double fraction = static_cast<double>(stage) / 8.0;
        StepInput staged = input;
        for (auto& force : staged.externalForces)
            force *= fraction;
        for (auto& torque : staged.externalTorquesWorld)
            torque *= fraction;
        FrozenTangentAir stagedAir;
        if (air)
        {
            stagedAir = *air;
            stagedAir.forceFraction *= fraction;
        }
        result = SolveStage(rest, old, candidate, staged, profile, constraints, air ? &stagedAir : nullptr,
            std::min(24u, 108u - fallbackIterations), stage == 8, true, fraction, trace);
        fallbackIterations += result.newtonIterations;
        backtracks += result.backtracks;
        linearResidual = std::max(linearResidual, result.linearSolveResidual);
        if (result.status != Status::Success)
            break;
    }
    result.usedFallback = true;
    result.directIterations = direct;
    result.fallbackIterations = fallbackIterations;
    result.newtonIterations = direct + fallbackIterations;
    result.backtracks = backtracks;
    result.linearSolveResidual = linearResidual;
    if (result.status == Status::Success)
        state = std::move(candidate);
    return result;
}
}

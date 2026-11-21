#include "hair/rod/rodImplicit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <Eigen/LU>

#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodRootSupport.h"
#include "hair/rod/rodAnalytic.h"
#include "hair/rod/rodAirDrag.h"

namespace VL::Hair::Rod
{
namespace
{
bool FixedComponent(const RestState& rest, size_t block, int component)
{
    if (component < 3)
        return rest.inverseMasses[block] == 0.0;
    return block == 0 || block == rest.lengths.size();
}

std::array<Vector6, 3> LocalElastic(
    const RestState& rest, const State& candidate, size_t column)
{
    std::array<Vector6, 3> values{Vector6::Zero(), Vector6::Zero(), Vector6::Zero()};
    auto accumulate = [&](size_t row, int offset, const Vec3& value)
    {
        const auto slot = static_cast<std::ptrdiff_t>(row) -
            static_cast<std::ptrdiff_t>(column) + 1;
        if (slot >= 0 && slot < 3)
            values[static_cast<size_t>(slot)].segment<3>(offset) += value;
    };
    const size_t first = column == 0 ? 0 : column - 1;
    const size_t end = std::min(rest.lengths.size(), column + 2);
    for (size_t segment = first; segment < end; ++segment)
    {
        const ShearBlock block = EvaluateShearBlock(rest, candidate, segment);
        const Vec3 gradient = rest.lengths[segment] *
            rest.stretchShear[segment].cwiseProduct(block.constraint);
        accumulate(segment, 0, -block.firstPositionJacobian.transpose() * gradient);
        accumulate(segment + 1, 0, -block.secondPositionJacobian.transpose() * gradient);
        accumulate(segment, 3, -block.orientationJacobian.transpose() * gradient);
    }
    const size_t jointEnd = std::min(rest.restJointAngles.size(), column + 1);
    for (size_t joint = first; joint < jointEnd; ++joint)
    {
        const BendBlock block = EvaluateBendBlock(rest, candidate, joint);
        const double length = 0.5 * (rest.lengths[joint] + rest.lengths[joint + 1]);
        const Vec3 gradient = rest.bendTwist[joint].cwiseProduct(block.constraint) / length;
        accumulate(joint, 3, -block.firstOrientationJacobian.transpose() * gradient);
        accumulate(joint + 1, 3, -block.secondOrientationJacobian.transpose() * gradient);
    }
    return values;
}

std::array<double, 3> Scales(const RestState& rest)
{
    double mass = 0.0;
    double stiffness = 0.0;
    for (size_t point = 0; point < rest.masses.size(); ++point)
    {
        mass += rest.masses[point];
        stiffness = std::max(stiffness, rest.supportStiffness[point]);
    }
    const double force = mass * 9.8 + stiffness * rest.totalLength;
    return {rest.totalLength, force, force * rest.totalLength};
}

std::vector<Vector6> NormalizedResidual(const RestState& rest,
    const State& old, const State& candidate, const StepInput& input,
    const ProgressiveSupport* profile = nullptr, const FrozenTangentAir* air = nullptr)
{
    std::vector<Vector6> result = air && profile
        ? EvaluateProgressiveAirResidual(rest, old, candidate, input, *profile, *air)
        : (profile ? EvaluateProgressiveImplicitResidual(rest, old, candidate, input, *profile)
                   : EvaluateImplicitResidual(rest, old, candidate, input));
    const auto scales = Scales(rest);
    for (size_t block = 0; block < result.size(); ++block)
    {
        result[block].head<3>() /= scales[1];
        result[block].tail<3>() /= scales[2];
        for (int component = 0; component < 6; ++component)
            if (FixedComponent(rest, block, component))
                result[block][component] = 0.0;
    }
    return result;
}

double SquaredMerit(const std::vector<Vector6>& residual)
{
    double result = 0.0;
    for (const Vector6& block : residual)
        result += block.squaredNorm();
    return result;
}

double MaximumResidual(const std::vector<Vector6>& residual)
{
    double result = 0.0;
    for (const Vector6& block : residual)
    {
        if (!block.allFinite())
            return std::numeric_limits<double>::infinity();
        result = std::max(result, block.cwiseAbs().maxCoeff());
    }
    return result;
}

bool ValidImplicitInput(const RestState& rest, const State& state, const StepInput& input)
{
    const size_t points = rest.positions.size();
    const size_t segments = rest.lengths.size();
    if (points < 2 || points != segments + 1 || state.positions.size() != points ||
        state.velocities.size() != points || state.orientations.size() != segments ||
        state.angularVelocities.size() != segments || rest.inverseMasses.size() != points ||
        rest.masses.size() != points || rest.supportStiffness.size() != points ||
        rest.supportDamping.size() != points || rest.rootLocalPositions.size() != points ||
        rest.inertias.size() != segments || rest.stretchShear.size() != segments ||
        rest.bendTwist.size() != segments || rest.restStrains.size() != segments ||
        rest.restJointAngles.size() + 1 != segments ||
        !std::isfinite(rest.totalLength) || rest.totalLength <= 0.0 ||
        !std::isfinite(input.stepSeconds) || input.stepSeconds <= 0.0 ||
        !input.collisionSpheres.empty() || !input.collisionCapsules.empty() ||
        !input.collisionFields.empty() || !input.gravity.allFinite() ||
        !input.root.position.allFinite() || !input.root.linearVelocity.allFinite() ||
        !input.root.angularVelocityWorld.allFinite() ||
        !input.root.orientation.coeffs().allFinite() ||
        std::abs(input.root.orientation.norm() - 1.0) > 1.0e-8 ||
        (!input.externalForces.empty() && input.externalForces.size() != points) ||
        (!input.externalTorquesWorld.empty() && input.externalTorquesWorld.size() != segments))
        return false;
    for (size_t point = 0; point < points; ++point)
        if (!state.positions[point].allFinite() || !state.velocities[point].allFinite() ||
            !rest.rootLocalPositions[point].allFinite() ||
            !std::isfinite(rest.masses[point]) || rest.masses[point] <= 0.0 ||
            !std::isfinite(rest.inverseMasses[point]) || rest.inverseMasses[point] < 0.0 ||
            !std::isfinite(rest.supportStiffness[point]) || rest.supportStiffness[point] < 0.0 ||
            !std::isfinite(rest.supportDamping[point]) || rest.supportDamping[point] < 0.0 ||
            (!input.externalForces.empty() && !input.externalForces[point].allFinite()))
            return false;
    for (size_t segment = 0; segment < segments; ++segment)
        if (!state.orientations[segment].coeffs().allFinite() ||
            std::abs(state.orientations[segment].norm() - 1.0) > 1.0e-8 ||
            !state.angularVelocities[segment].allFinite() ||
            !std::isfinite(rest.lengths[segment]) || rest.lengths[segment] <= 0.0 ||
            !rest.inertias[segment].allFinite() || (rest.inertias[segment].array() <= 0.0).any() ||
            !rest.stretchShear[segment].allFinite() ||
            (rest.stretchShear[segment].array() <= 0.0).any() ||
            !rest.bendTwist[segment].allFinite() || (rest.bendTwist[segment].array() <= 0.0).any() ||
            !rest.restStrains[segment].allFinite() ||
            (!input.externalTorquesWorld.empty() && !input.externalTorquesWorld[segment].allFinite()))
            return false;
    for (const Vec3& angle : rest.restJointAngles)
        if (!angle.allFinite() || angle.norm() >= RotationLimit)
            return false;
    return rest.inverseMasses.front() == 0.0;
}
}

std::vector<Vector6> EvaluateImplicitResidual(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input)
{
    const double step = input.stepSeconds;
    const EnergyEvaluation elastic = EvaluateEnergy(rest, candidate);
    std::vector<Vector6> result(rest.positions.size(), Vector6::Zero());
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        const SupportSample target = SampleRootSupport(input.root, rest.rootLocalPositions[point]);
        const double stiffness = input.supportEnabled ? rest.supportStiffness[point] : 0.0;
        const double damping = input.supportEnabled ? rest.supportDamping[point] : 0.0;
        const Vec3 external = input.externalForces.empty()
            ? Vec3::Zero().eval() : input.externalForces[point];
        result[point].head<3>() = rest.masses[point] *
            (candidate.positions[point] - old.positions[point] - step * old.velocities[point]) /
                (step * step) +
            damping * ((candidate.positions[point] - old.positions[point]) / step - target.velocity) -
            elastic.forces[point] + stiffness * (candidate.positions[point] - target.position) -
            rest.masses[point] * input.gravity - external;
    }
    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
    {
        const Vec3 angle = Log(old.orientations[segment].conjugate() * candidate.orientations[segment]);
        const Vec3 transported = candidate.orientations[segment].conjugate() *
            (old.orientations[segment] *
                rest.inertias[segment].cwiseProduct(old.angularVelocities[segment]));
        const Vec3 torqueWorld = input.externalTorquesWorld.empty()
            ? Vec3::Zero().eval() : input.externalTorquesWorld[segment];
        result[segment].tail<3>() = rest.inertias[segment].cwiseProduct(angle) / (step * step) -
            transported / step - elastic.bodyTorques[segment] -
            candidate.orientations[segment].conjugate() * torqueWorld;
    }
    return result;
}

bool ValidateImplicitStepInput(const RestState& rest, const State& state, const StepInput& input)
{
    return ValidImplicitInput(rest, state, input);
}

ImplicitSystem AssembleImplicitSystem(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input, double differenceStep)
{
    const size_t blocks = rest.positions.size();
    const auto scales = Scales(rest);
    ImplicitSystem system;
    system.lengthScale = scales[0];
    system.forceScale = scales[1];
    system.torqueScale = scales[2];
    system.lower.assign(blocks, Matrix6::Zero());
    system.diagonal.assign(blocks, Matrix6::Zero());
    system.upper.assign(blocks, Matrix6::Zero());
    system.residual = NormalizedResidual(rest, old, candidate, input);
    for (size_t column = 0; column < blocks; ++column)
        for (int component = 0; component < 6; ++component)
        {
            if (FixedComponent(rest, column, component))
                continue;
            State plus = candidate;
            State minus = candidate;
            const double epsilon = differenceStep *
                (component < 3 ? system.lengthScale : 1.0);
            if (component < 3)
            {
                plus.positions[column][component] += epsilon;
                minus.positions[column][component] -= epsilon;
            }
            else
            {
                const Vec3 angle = epsilon * Vec3::Unit(component - 3);
                plus.orientations[column] = ApplyBodyIncrement(candidate.orientations[column], angle);
                minus.orientations[column] = ApplyBodyIncrement(candidate.orientations[column], -angle);
            }
            const auto positive = LocalElastic(rest, plus, column);
            const auto negative = LocalElastic(rest, minus, column);
            for (int offset = -1; offset <= 1; ++offset)
            {
                const auto rowSigned = static_cast<std::ptrdiff_t>(column) + offset;
                if (rowSigned < 0 || rowSigned >= static_cast<std::ptrdiff_t>(blocks))
                    continue;
                const size_t row = static_cast<size_t>(rowSigned);
                Vector6 derivative = -(positive[static_cast<size_t>(offset + 1)] -
                    negative[static_cast<size_t>(offset + 1)]) / (2.0 * epsilon);
                derivative.head<3>() /= system.forceScale;
                derivative.tail<3>() /= system.torqueScale;
                if (component < 3)
                    derivative *= system.lengthScale;
                Matrix6& matrix = offset == 0 ? system.diagonal[row] :
                    (offset == -1 ? system.upper[row] : system.lower[row]);
                matrix.col(component) += derivative;
            }
        }
    for (size_t block = 0; block < blocks; ++block)
    {
        const double stiffness = input.supportEnabled ? rest.supportStiffness[block] : 0.0;
        const double damping = input.supportEnabled ? rest.supportDamping[block] : 0.0;
        const double step = input.stepSeconds;
        const double positional = rest.masses[block] / (step * step) + damping / step + stiffness;
        system.diagonal[block].topLeftCorner<3, 3>() +=
            (positional * system.lengthScale / system.forceScale) * Mat3::Identity();
        if (block < rest.lengths.size())
        {
            const Vec3 angle = Log(old.orientations[block].conjugate() * candidate.orientations[block]);
            const Vec3 transported = candidate.orientations[block].conjugate() *
                (old.orientations[block] *
                    rest.inertias[block].cwiseProduct(old.angularVelocities[block]));
            const Vec3 torqueWorld = input.externalTorquesWorld.empty()
                ? Vec3::Zero().eval() : input.externalTorquesWorld[block];
            const Vec3 bodyTorque = candidate.orientations[block].conjugate() * torqueWorld;
            const Mat3 derivative = rest.inertias[block].asDiagonal().toDenseMatrix() *
                RightJacobianInverse(angle) / (step * step) -
                CrossMatrix(transported) / step - CrossMatrix(bodyTorque);
            system.diagonal[block].bottomRightCorner<3, 3>() += derivative / system.torqueScale;
        }
    }
    for (size_t block = 0; block < blocks; ++block)
        for (int component = 0; component < 6; ++component)
            if (FixedComponent(rest, block, component))
            {
                system.diagonal[block].row(component).setZero();
                system.diagonal[block].col(component).setZero();
                system.lower[block].row(component).setZero();
                system.upper[block].row(component).setZero();
                if (block > 0)
                    system.upper[block - 1].col(component).setZero();
                if (block + 1 < blocks)
                    system.lower[block + 1].col(component).setZero();
                system.diagonal[block](component, component) = 1.0;
            }
    return system;
}

std::vector<Vector6> SolveImplicitBlocks(const ImplicitSystem& system)
{
    const size_t count = system.diagonal.size();
    std::vector<Matrix6> reducedUpper(count, Matrix6::Zero());
    std::vector<Vector6> reducedRight(count, Vector6::Zero());
    for (size_t block = 0; block < count; ++block)
    {
        Matrix6 diagonal = system.diagonal[block];
        Vector6 right = -system.residual[block];
        if (block > 0)
        {
            diagonal -= system.lower[block] * reducedUpper[block - 1];
            right -= system.lower[block] * reducedRight[block - 1];
        }
        Eigen::FullPivLU<Matrix6> factorization(diagonal);
        if (!factorization.isInvertible())
            throw std::runtime_error("implicit rod block pivot is singular");
        reducedUpper[block] = factorization.solve(system.upper[block]);
        reducedRight[block] = factorization.solve(right);
    }
    std::vector<Vector6> result = reducedRight;
    for (size_t block = count - 1; block > 0; --block)
        result[block - 1] -= reducedUpper[block - 1] * result[block];
    return result;
}

static ImplicitDiagnostics StepImplicitMomentumImpl(
    const RestState& rest, State& state, const StepInput& input, bool analytic,
    const ProgressiveSupport* profile = nullptr, ImplicitTrace* trace = nullptr,
    const State* initialGuess = nullptr, ResidualWorkCertificate* workCertificate = nullptr,
    const FrozenTangentAir* air = nullptr)
{
    ImplicitDiagnostics diagnostics;
    if (!ValidImplicitInput(rest, state, input))
    {
        diagnostics.status = Status::InvalidInput;
        return diagnostics;
    }
    if (profile)
    {
        if (profile->cubicStiffness.size() != rest.positions.size())
        {
            diagnostics.status = Status::InvalidInput;
            return diagnostics;
        }
        for (double stiffness : profile->cubicStiffness)
            if (!std::isfinite(stiffness) || stiffness < 0.0)
            {
                diagnostics.status = Status::InvalidInput;
                return diagnostics;
            }
    }
    if (air && (!profile || !air->velocityWorld.allFinite() ||
        !std::isfinite(air->forceFraction) || air->forceFraction < 0.0 || air->forceFraction > 1.0))
    {
        diagnostics.status = Status::InvalidInput;
        return diagnostics;
    }
    const State old = state;
    State candidate = initialGuess ? *initialGuess : state;
    candidate.velocities = old.velocities;
    candidate.angularVelocities = old.angularVelocities;
    struct TraceFinalizer
    {
        ImplicitTrace* trace;
        State* candidate;
        ~TraceFinalizer()
        {
            if (trace && !candidate->positions.empty())
                trace->finalCandidate = *candidate;
        }
    };
    const TraceFinalizer finalizer{trace, &candidate};
    for (size_t point = 0; point < rest.positions.size(); ++point)
        if (rest.inverseMasses[point] == 0.0)
            candidate.positions[point] = SampleRootSupport(input.root, rest.rootLocalPositions[point]).position;
    candidate.orientations.front() = input.root.orientation;
    try
    {
        for (unsigned iteration = 0; iteration <= 12; ++iteration)
        {
            const auto residual = NormalizedResidual(rest, old, candidate, input, profile, air);
            diagnostics.normalizedResidual = MaximumResidual(residual);
            if (trace)
            {
                trace->iterations.emplace_back();
                trace->iterations.back().candidate = candidate;
                trace->iterations.back().normalizedResidual = diagnostics.normalizedResidual;
            }
            if (!std::isfinite(diagnostics.normalizedResidual))
            {
                diagnostics.status = Status::NonFinite;
                return diagnostics;
            }
            if (diagnostics.normalizedResidual <= 1.0e-8)
            {
                bool workAccepted = true;
                if (workCertificate && profile)
                {
                    StepInput workInput = input;
                    if (air)
                    {
                        if (workInput.externalForces.empty())
                            workInput.externalForces.assign(rest.positions.size(), Vec3::Zero());
                        for (size_t point = 0; point < rest.positions.size(); ++point)
                            workInput.externalForces[point] += EvaluateFrozenTangentAirDrag(old, candidate,
                                point, rest.masses[point], input.stepSeconds, *profile,
                                air->velocityWorld, air->forceFraction).force;
                    }
                    *workCertificate = EvaluateProgressiveResidualWork(rest, old, candidate, workInput, *profile);
                    workAccepted = workCertificate->accepted;
                }
                if (workAccepted)
                {
                    diagnostics.converged = true;
                    break;
                }
            }
            if (iteration == 12)
            {
                if (trace)
                    trace->failureReason = workCertificate && diagnostics.normalizedResidual <= 1.0e-8 ?
                        "residual-work-certificate-budget" : "newton-iteration-budget";
                diagnostics.status = Status::PredictorFailure;
                return diagnostics;
            }
            const ImplicitSystem system = air && profile
                ? AssembleProgressiveAirSystem(rest, old, candidate, input, *profile, *air)
                : (profile ? AssembleProgressiveImplicitSystem(rest, old, candidate, input, *profile)
                    : (analytic ? AssembleImplicitSystemAnalytic(rest, old, candidate, input)
                                : AssembleImplicitSystem(rest, old, candidate, input)));
            const auto direction = SolveImplicitBlocks(system);
            if (trace)
            {
                trace->iterations.back().system = system;
                trace->iterations.back().direction = direction;
            }
            double linearResidual = 0.0;
            for (size_t block = 0; block < direction.size(); ++block)
            {
                Vector6 remaining = system.diagonal[block] * direction[block] + system.residual[block];
                if (block > 0)
                    remaining += system.lower[block] * direction[block - 1];
                if (block + 1 < direction.size())
                    remaining += system.upper[block] * direction[block + 1];
                linearResidual = std::max(linearResidual, remaining.cwiseAbs().maxCoeff());
            }
            diagnostics.linearSolveResidual = std::max(diagnostics.linearSolveResidual, linearResidual);
            const double merit = SquaredMerit(residual);
            bool accepted = false;
            double alpha = 1.0;
            for (unsigned trialIndex = 0; trialIndex <= 20; ++trialIndex)
            {
                try
                {
                    State trial = candidate;
                    for (size_t block = 0; block < direction.size(); ++block)
                    {
                        trial.positions[block] += alpha * system.lengthScale * direction[block].head<3>();
                        if (block < rest.lengths.size())
                            trial.orientations[block] = ApplyBodyIncrement(trial.orientations[block],
                                alpha * direction[block].tail<3>());
                    }
                    const auto trialResidual = NormalizedResidual(rest, old, trial, input, profile, air);
                    const double trialMerit = SquaredMerit(trialResidual);
                    if (trace)
                        trace->iterations.back().trials.push_back(
                            {alpha, trialMerit, MaximumResidual(trialResidual), false, false});
                    if (std::isfinite(trialMerit) &&
                        trialMerit <= (1.0 - 1.0e-4 * alpha) * merit)
                    {
                        candidate = std::move(trial);
                        if (trace)
                            trace->iterations.back().trials.back().accepted = true;
                        accepted = true;
                        break;
                    }
                }
                catch (const std::domain_error&)
                {
                    if (trace)
                        trace->iterations.back().trials.push_back(
                            {alpha, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::infinity(), true, false});
                }
                alpha *= 0.5;
                ++diagnostics.backtracks;
            }
            if (!accepted)
            {
                if (trace)
                    trace->failureReason = "full-residual-armijo-rejected";
                diagnostics.status = Status::PredictorFailure;
                return diagnostics;
            }
            ++diagnostics.newtonIterations;
        }
        for (size_t point = 0; point < rest.positions.size(); ++point)
            candidate.velocities[point] = (candidate.positions[point] - old.positions[point]) / input.stepSeconds;
        for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
            candidate.angularVelocities[segment] =
                Log(old.orientations[segment].conjugate() * candidate.orientations[segment]) / input.stepSeconds;
        const auto physical = air && profile
            ? EvaluateProgressiveAirResidual(rest, old, candidate, input, *profile, *air)
            : (profile ? EvaluateProgressiveImplicitResidual(rest, old, candidate, input, *profile)
                       : EvaluateImplicitResidual(rest, old, candidate, input));
        const auto elastic = EvaluateEnergy(rest, candidate);
        SupportEvaluation support;
        if (input.supportEnabled)
            support = profile ? EvaluateProgressiveSupport(rest, candidate, input.root, *profile)
                              : EvaluateRootSupport(rest, candidate, input.root);
        diagnostics.energy = elastic.Total() + support.potential;
        diagnostics.supportDrivePower = support.drivePower;
        diagnostics.supportDissipation = support.dissipation;
        for (size_t point = 0; point < rest.positions.size(); ++point)
        {
            diagnostics.energy -= rest.masses[point] * input.gravity.dot(candidate.positions[point]);
            diagnostics.linearKineticEnergy += 0.5 * rest.masses[point] *
                candidate.velocities[point].squaredNorm();
            diagnostics.maximumForce = std::max(diagnostics.maximumForce, elastic.forces[point].norm());
            if (rest.inverseMasses[point] == 0.0)
            {
                diagnostics.rootForce += physical[point].head<3>();
                diagnostics.rootBodyTorque += input.root.orientation.conjugate() *
                    ((candidate.positions[point] - input.root.position).cross(physical[point].head<3>()));
            }
            else
                diagnostics.maximumLinearResidual = std::max(diagnostics.maximumLinearResidual,
                    physical[point].head<3>().norm());
        }
        for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
        {
            diagnostics.angularKineticEnergy += 0.5 * candidate.angularVelocities[segment].dot(
                rest.inertias[segment].cwiseProduct(candidate.angularVelocities[segment]));
            diagnostics.maximumBodyTorque = std::max(diagnostics.maximumBodyTorque,
                elastic.bodyTorques[segment].norm());
            if (segment == 0)
                diagnostics.rootBodyTorque += physical[segment].tail<3>();
            else
                diagnostics.maximumAngularResidual = std::max(diagnostics.maximumAngularResidual,
                    physical[segment].tail<3>().norm());
        }
        diagnostics.maximumShearDefect = std::numeric_limits<double>::quiet_NaN();
        diagnostics.maximumBendDefect = std::numeric_limits<double>::quiet_NaN();
        diagnostics.maximumSupportDefect = std::numeric_limits<double>::quiet_NaN();
        if (!std::isfinite(diagnostics.energy) || !std::isfinite(diagnostics.linearKineticEnergy) ||
            !std::isfinite(diagnostics.angularKineticEnergy))
        {
            diagnostics.status = Status::NonFinite;
            return diagnostics;
        }
        if (trace)
            trace->finalCandidate = candidate;
        state = std::move(candidate);
    }
    catch (const std::domain_error&)
    {
        if (trace)
            trace->failureReason = "rotation-domain";
        diagnostics.status = Status::DomainFailure;
    }
    catch (const std::runtime_error&)
    {
        if (trace)
            trace->failureReason = "linear-factorization";
        diagnostics.status = Status::LinearSolveFailure;
    }
    return diagnostics;
}

ImplicitDiagnostics StepImplicitMomentum(
    const RestState& rest, State& state, const StepInput& input)
{
    return StepImplicitMomentumImpl(rest, state, input, false);
}

ImplicitDiagnostics StepImplicitMomentumAnalytic(
    const RestState& rest, State& state, const StepInput& input)
{
    return StepImplicitMomentumImpl(rest, state, input, true);
}

std::vector<Vector6> EvaluateProgressiveImplicitResidual(
    const RestState& rest, const State& old, const State& candidate,
    const StepInput& input, const ProgressiveSupport& profile)
{
    if (profile.cubicStiffness.size() != rest.positions.size())
        throw std::invalid_argument("progressive implicit profile dimensions are invalid");
    auto result = EvaluateImplicitResidual(rest, old, candidate, input);
    if (input.supportEnabled)
        for (size_t point = 0; point < rest.positions.size(); ++point)
        {
            const Vec3 displacement = candidate.positions[point] -
                SampleRootSupport(input.root, rest.rootLocalPositions[point]).position;
            const auto node = EvaluateProgressiveNode(displacement, 0.0, profile.cubicStiffness[point]);
            result[point].head<3>() -= node.springForce;
        }
    return result;
}

ImplicitSystem AssembleProgressiveImplicitSystem(
    const RestState& rest, const State& old, const State& candidate,
    const StepInput& input, const ProgressiveSupport& profile)
{
    if (profile.cubicStiffness.size() != rest.positions.size())
        throw std::invalid_argument("progressive implicit profile dimensions are invalid");
    auto system = AssembleImplicitSystemAnalytic(rest, old, candidate, input);
    if (input.supportEnabled)
        for (size_t point = 0; point < rest.positions.size(); ++point)
        {
            if (rest.inverseMasses[point] == 0.0)
                continue;
            const Vec3 displacement = candidate.positions[point] -
                SampleRootSupport(input.root, rest.rootLocalPositions[point]).position;
            const auto node = EvaluateProgressiveNode(displacement, 0.0, profile.cubicStiffness[point]);
            system.residual[point].head<3>() -= node.springForce / system.forceScale;
            system.diagonal[point].topLeftCorner<3, 3>() += node.hessian *
                (system.lengthScale / system.forceScale);
        }
    return system;
}

ImplicitDiagnostics StepImplicitMomentumProgressive(
    const RestState& rest, State& state, const StepInput& input, const ProgressiveSupport& profile)
{
    return StepImplicitMomentumImpl(rest, state, input, true, &profile);
}

ImplicitDiagnostics StepImplicitMomentumProgressiveWithTrace(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ImplicitTrace& trace)
{
    trace = ImplicitTrace{};
    trace.old = state;
    trace.finalCandidate = state;
    return StepImplicitMomentumImpl(rest, state, input, true, &profile, &trace);
}

static ImplicitDiagnostics ContinueProgressiveImpl(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ImplicitTrace* trace, ResidualWorkCertificate* certificate,
    const FrozenTangentAir* air = nullptr);

ImplicitDiagnostics StepImplicitMomentumProgressiveContinuation(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ImplicitTrace* trace)
{
    return ContinueProgressiveImpl(rest, state, input, profile, trace, nullptr);
}

static ImplicitDiagnostics ContinueProgressiveImpl(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ImplicitTrace* trace, ResidualWorkCertificate* certificate,
    const FrozenTangentAir* air)
{
    const State old = state;
    State seed = old;
    if (trace)
    {
        *trace = ImplicitTrace{};
        trace->old = old;
        trace->finalCandidate = old;
    }
    ImplicitDiagnostics result;
    unsigned iterations = 0;
    unsigned backtracks = 0;
    double maximumLinearResidual = 0.0;
    for (unsigned stage = 0; stage <= 8; ++stage)
    {
        const double fraction = static_cast<double>(stage) / 8.0;
        StepInput stagedInput = input;
        for (auto& force : stagedInput.externalForces)
            force *= fraction;
        for (auto& torque : stagedInput.externalTorquesWorld)
            torque *= fraction;
        FrozenTangentAir stagedAir;
        if (air)
        {
            stagedAir = *air;
            stagedAir.forceFraction *= fraction;
        }
        State working = old;
        ImplicitTrace stageTrace;
        if (trace)
        {
            stageTrace.old = old;
            stageTrace.finalCandidate = seed;
        }
        result = StepImplicitMomentumImpl(rest, working, stagedInput, true, &profile,
            trace ? &stageTrace : nullptr, &seed, stage == 8 ? certificate : nullptr,
            air ? &stagedAir : nullptr);
        iterations += result.newtonIterations;
        backtracks += result.backtracks;
        maximumLinearResidual = std::max(maximumLinearResidual, result.linearSolveResidual);
        if (trace)
        {
            for (auto& iteration : stageTrace.iterations)
            {
                iteration.loadFraction = fraction;
                trace->iterations.push_back(std::move(iteration));
            }
            trace->finalCandidate = stageTrace.finalCandidate;
        }
        if (result.status != Status::Success)
        {
            if (trace)
                trace->failureReason = "continuation-stage-" + std::to_string(stage) + "-" + stageTrace.failureReason;
            result.newtonIterations = iterations;
            result.backtracks = backtracks;
            result.linearSolveResidual = maximumLinearResidual;
            return result;
        }
        seed = std::move(working);
    }
    result.newtonIterations = iterations;
    result.backtracks = backtracks;
    result.linearSolveResidual = maximumLinearResidual;
    state = std::move(seed);
    return result;
}

ImplicitDiagnostics StepImplicitMomentumProgressiveContinuationWithTrace(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ImplicitTrace& trace)
{
    return StepImplicitMomentumProgressiveContinuation(rest, state, input, profile, &trace);
}

ImplicitDiagnostics StepImplicitMomentumProgressiveWorkGuard(const RestState& rest,
    State& state, const StepInput& input, const ProgressiveSupport& profile,
    ResidualWorkCertificate& certificate)
{
    certificate = ResidualWorkCertificate{};
    return StepImplicitMomentumImpl(rest, state, input, true, &profile, nullptr, nullptr, &certificate);
}

ImplicitDiagnostics StepImplicitMomentumProgressiveContinuationWorkGuard(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ResidualWorkCertificate& certificate, ImplicitTrace* trace)
{
    certificate = ResidualWorkCertificate{};
    return ContinueProgressiveImpl(rest, state, input, profile, trace, &certificate);
}

std::vector<Vector6> EvaluateProgressiveAirResidual(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input, const ProgressiveSupport& profile,
    const FrozenTangentAir& air)
{
    auto residual = EvaluateProgressiveImplicitResidual(rest, old, candidate, input, profile);
    for (size_t point = 0; point < rest.positions.size(); ++point)
        residual[point].head<3>() -= EvaluateFrozenTangentAirDrag(old, candidate, point,
            rest.masses[point], input.stepSeconds, profile, air.velocityWorld, air.forceFraction).force;
    return residual;
}

ImplicitSystem AssembleProgressiveAirSystem(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input, const ProgressiveSupport& profile,
    const FrozenTangentAir& air)
{
    auto system = AssembleProgressiveImplicitSystem(rest, old, candidate, input, profile);
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        if (rest.inverseMasses[point] == 0.0)
            continue;
        const auto drag = EvaluateFrozenTangentAirDrag(old, candidate, point, rest.masses[point],
            input.stepSeconds, profile, air.velocityWorld, air.forceFraction);
        system.residual[point].head<3>() -= drag.force / system.forceScale;
        system.diagonal[point].topLeftCorner<3, 3>() += drag.negativeVelocityDerivative *
            (system.lengthScale / (input.stepSeconds * system.forceScale));
    }
    return system;
}

ImplicitDiagnostics StepImplicitMomentumProgressiveAir(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile, const Vec3& airVelocityWorld,
    ResidualWorkCertificate& certificate, ImplicitTrace* trace)
{
    certificate = ResidualWorkCertificate{};
    const FrozenTangentAir air{airVelocityWorld, 1.0};
    if (trace)
    {
        *trace = ImplicitTrace{};
        trace->old = state;
        trace->finalCandidate = state;
    }
    return StepImplicitMomentumImpl(rest, state, input, true, &profile, trace, nullptr, &certificate, &air);
}

ImplicitDiagnostics StepImplicitMomentumProgressiveAirContinuation(const RestState& rest,
    State& state, const StepInput& input, const ProgressiveSupport& profile,
    const Vec3& airVelocityWorld, ResidualWorkCertificate& certificate, ImplicitTrace* trace)
{
    certificate = ResidualWorkCertificate{};
    const FrozenTangentAir air{airVelocityWorld, 1.0};
    return ContinueProgressiveImpl(rest, state, input, profile, trace, &certificate, &air);
}
}

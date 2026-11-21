#pragma once

#include <array>
#include <string>

#include "hair/rod/rodXpbd.h"
#include "hair/rod/rodProgressiveSupport.h"
#include "hair/rod/rodWorkCertificate.h"

namespace VL::Hair::Rod
{
using Vector6 = Eigen::Matrix<double, 6, 1>;
using Matrix6 = Eigen::Matrix<double, 6, 6>;

struct ImplicitSystem
{
    std::vector<Matrix6> lower;
    std::vector<Matrix6> diagonal;
    std::vector<Matrix6> upper;
    std::vector<Vector6> residual;
    double lengthScale = 1.0;
    double forceScale = 1.0;
    double torqueScale = 1.0;
};

struct ImplicitDiagnostics : StepDiagnostics
{
    unsigned newtonIterations = 0;
    unsigned backtracks = 0;
    double normalizedResidual = 0.0;
    double linearSolveResidual = 0.0;
};

struct ImplicitTrialTrace
{
    double alpha = 0.0;
    double merit = 0.0;
    double normalizedResidual = 0.0;
    bool domainFailure = false;
    bool accepted = false;
};

struct ImplicitIterationTrace
{
    State candidate;
    ImplicitSystem system;
    std::vector<Vector6> direction;
    std::vector<ImplicitTrialTrace> trials;
    double normalizedResidual = 0.0;
    double loadFraction = 1.0;
};

struct ImplicitTrace
{
    State old;
    State finalCandidate;
    std::vector<ImplicitIterationTrace> iterations;
    std::string failureReason;
};

struct FrozenTangentAir
{
    Vec3 velocityWorld = Vec3::Zero();
    double forceFraction = 1.0;
};

bool ValidateImplicitStepInput(const RestState& rest, const State& state, const StepInput& input);

std::vector<Vector6> EvaluateImplicitResidual(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input);
ImplicitSystem AssembleImplicitSystem(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input, double differenceStep = 1.0e-6);
std::vector<Vector6> SolveImplicitBlocks(const ImplicitSystem& system);
ImplicitDiagnostics StepImplicitMomentum(
    const RestState& rest, State& state, const StepInput& input);
ImplicitDiagnostics StepImplicitMomentumAnalytic(
    const RestState& rest, State& state, const StepInput& input);
ImplicitDiagnostics StepImplicitMomentumProgressive(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile);
ImplicitDiagnostics StepImplicitMomentumProgressiveWithTrace(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ImplicitTrace& trace);
ImplicitDiagnostics StepImplicitMomentumProgressiveContinuation(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ImplicitTrace* trace = nullptr);
ImplicitDiagnostics StepImplicitMomentumProgressiveContinuationWithTrace(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ImplicitTrace& trace);
ImplicitDiagnostics StepImplicitMomentumProgressiveWorkGuard(const RestState& rest,
    State& state, const StepInput& input, const ProgressiveSupport& profile,
    ResidualWorkCertificate& certificate);
ImplicitDiagnostics StepImplicitMomentumProgressiveContinuationWorkGuard(
    const RestState& rest, State& state, const StepInput& input,
    const ProgressiveSupport& profile, ResidualWorkCertificate& certificate,
    ImplicitTrace* trace = nullptr);
std::vector<Vector6> EvaluateProgressiveImplicitResidual(
    const RestState& rest, const State& old, const State& candidate,
    const StepInput& input, const ProgressiveSupport& profile);
ImplicitSystem AssembleProgressiveImplicitSystem(
    const RestState& rest, const State& old, const State& candidate,
    const StepInput& input, const ProgressiveSupport& profile);
std::vector<Vector6> EvaluateProgressiveAirResidual(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input, const ProgressiveSupport& profile,
    const FrozenTangentAir& air);
ImplicitSystem AssembleProgressiveAirSystem(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input, const ProgressiveSupport& profile,
    const FrozenTangentAir& air);
ImplicitDiagnostics StepImplicitMomentumProgressiveAir(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile, const Vec3& airVelocityWorld,
    ResidualWorkCertificate& certificate, ImplicitTrace* trace = nullptr);
ImplicitDiagnostics StepImplicitMomentumProgressiveAirContinuation(const RestState& rest,
    State& state, const StepInput& input, const ProgressiveSupport& profile,
    const Vec3& airVelocityWorld, ResidualWorkCertificate& certificate, ImplicitTrace* trace = nullptr);
}

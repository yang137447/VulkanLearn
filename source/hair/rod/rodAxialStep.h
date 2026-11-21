#pragma once

#include "hair/rod/rodAxialKkt.h"

namespace VL::Hair::Rod
{
struct AxialStepIteration
{
    State candidate;
    AxialContactActiveSet active;
    AxialContactSystem system;
    std::vector<AxialContactVector> direction;
    std::vector<ImplicitTrialTrace> trials;
    double loadFraction = 1.0;
    double normalizedResidual = 0.0;
    bool fallback = false;
};

struct AxialStepTrace
{
    State old;
    State finalCandidate;
    AxialContactActiveSet finalActive;
    std::vector<AxialStepIteration> iterations;
    std::string failureReason;
};

struct AxialStepDiagnostics : ImplicitDiagnostics
{
    ResidualWorkCertificate workCertificate;
    double maximumTensileViolation = 0.0;
    double maximumContactPenetration = 0.0;
    double maximumComplementarity = 0.0;
    double tensileWork = 0.0;
    double contactWork = 0.0;
    unsigned directIterations = 0;
    unsigned fallbackIterations = 0;
    bool usedFallback = false;
    std::string failureReason;
};

AxialStepDiagnostics StepAxialContactImplicit(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints = {},
    const FrozenTangentAir* air = nullptr, AxialStepTrace* trace = nullptr);
}

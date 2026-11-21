#pragma once

#include "hair/rod/rodXpbd.h"
#include "hair/rod/rodProgressiveSupport.h"

namespace VL::Hair::Rod
{
struct ResidualWorkCertificate
{
    double freeResidualWork = 0.0;
    double freeResidualWorkUpper = 0.0;
    double rootConstraintWork = 0.0;
    double energyReference = 0.0;
    double frozenTargetBalance = 0.0;
    double ledgerScale = 0.0;
    double workBoundScaled = 0.0;
    double balanceScaled = 0.0;
    double identityScaled = 0.0;
    bool finite = false;
    bool accepted = false;
};

ResidualWorkCertificate EvaluateProgressiveResidualWork(const RestState& rest,
    const State& old, const State& candidate, const StepInput& input,
    const ProgressiveSupport& profile);
}

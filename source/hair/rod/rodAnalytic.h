#pragma once

#include "hair/rod/rodImplicit.h"

namespace VL::Hair::Rod
{
ImplicitSystem AssembleImplicitSystemAnalytic(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input);
}

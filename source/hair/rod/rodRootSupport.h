#pragma once

#include "hair/rod/rodTypes.h"

namespace VL::Hair::Rod
{
SupportSample SampleRootSupport(const RootFrame& root, const Vec3& rootLocalPosition);
SupportEvaluation EvaluateRootSupport(
    const RestState& rest, const State& state, const RootFrame& root);
}

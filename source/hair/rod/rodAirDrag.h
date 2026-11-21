#pragma once

#include "hair/rod/rodProgressiveSupport.h"

namespace VL::Hair::Rod
{
struct AirDragEvaluation
{
    Vec3 tangent = Vec3::Zero();
    Vec3 relativeNormalVelocity = Vec3::Zero();
    Vec3 force = Vec3::Zero();
    Mat3 negativeVelocityDerivative = Mat3::Zero();
};

Vec3 FrozenPointTangent(const State& old, size_t point);
AirDragEvaluation EvaluateFrozenTangentAirDrag(const State& old, const State& candidate,
    size_t point, double mass, double stepSeconds, const ProgressiveSupport& profile,
    const Vec3& airVelocityWorld, double forceFraction = 1.0);
}

#pragma once

#include "hair/rod/rodTypes.h"

namespace VL::Hair::Rod
{
struct ProgressiveSupport
{
    std::vector<double> cubicStiffness;
    std::vector<double> designForces;
    double designAirSpeed = 20.0;
    double responseLimitRatio = 0.08;
    double dragAccelerationPerSpeed = 0.0;
};

struct ProgressiveNode
{
    double potential = 0.0;
    Vec3 springForce = Vec3::Zero();
    Mat3 hessian = Mat3::Zero();
};

ProgressiveSupport BuildProgressiveSupport(const RestState& rest,
    const Material& material = {});
double EffectiveBundleDragAccelerationPerSpeed(const Material& material = {});
ProgressiveNode EvaluateProgressiveNode(
    const Vec3& displacement, double linearStiffness, double cubicStiffness);
SupportEvaluation EvaluateProgressiveSupport(const RestState& rest, const State& state,
    const RootFrame& root, const ProgressiveSupport& profile);
Vec3 ProgressiveAirDragForce(const ProgressiveSupport& profile,
    double mass, const Vec3& relativeAirVelocity, const Vec3& unitTangent);
}

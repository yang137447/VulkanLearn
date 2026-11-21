#pragma once

#include "hair/rod/rodTypes.h"

namespace VL::Hair::Rod
{
inline constexpr unsigned AxialLimitMathVersion = 1;
inline constexpr double MaximumTensileStrain = 0.01;

using AxialVector6 = Eigen::Matrix<double, 6, 1>;
using AxialMatrix6 = Eigen::Matrix<double, 6, 6>;

struct AxialLimitEvaluation
{
    double gap = 0.0;
    double length = 0.0;
    Vec3 direction = Vec3::Zero();
    AxialVector6 gapGradient = AxialVector6::Zero();
    AxialMatrix6 gapHessian = AxialMatrix6::Zero();
    AxialVector6 force = AxialVector6::Zero();
    AxialMatrix6 negativeForceDerivative = AxialMatrix6::Zero();
    double work = 0.0;
};

AxialLimitEvaluation EvaluateAxialLimit(const Vec3& oldLeft, const Vec3& oldRight,
    const Vec3& newLeft, const Vec3& newRight, double restLength, double multiplier);
}

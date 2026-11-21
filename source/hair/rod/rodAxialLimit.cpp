#include "hair/rod/rodAxialLimit.h"

#include <cmath>
#include <stdexcept>

namespace VL::Hair::Rod
{
AxialLimitEvaluation EvaluateAxialLimit(const Vec3& oldLeft, const Vec3& oldRight,
    const Vec3& newLeft, const Vec3& newRight, double restLength, double multiplier)
{
    if (!oldLeft.allFinite() || !oldRight.allFinite() ||
        !newLeft.allFinite() || !newRight.allFinite() ||
        !std::isfinite(restLength) || restLength <= 0.0 || !std::isfinite(multiplier))
        throw std::invalid_argument("axial limit requires finite endpoints, multiplier and positive rest length");
    AxialLimitEvaluation result;
    const Vec3 edge = newRight - newLeft;
    result.length = edge.norm();
    if (!std::isfinite(result.length) || result.length <= 1.0e-14)
        throw std::domain_error("axial limit segment has no unique direction");
    result.direction = edge / result.length;
    result.gap = (1.0 + MaximumTensileStrain) * restLength - result.length;
    result.gapGradient.head<3>() = result.direction;
    result.gapGradient.tail<3>() = -result.direction;
    const Mat3 normalDerivative =
        (Mat3::Identity() - result.direction * result.direction.transpose()) / result.length;
    result.gapHessian.topLeftCorner<3, 3>() = -normalDerivative;
    result.gapHessian.topRightCorner<3, 3>() = normalDerivative;
    result.gapHessian.bottomLeftCorner<3, 3>() = normalDerivative;
    result.gapHessian.bottomRightCorner<3, 3>() = -normalDerivative;
    result.force = multiplier * result.gapGradient;
    result.negativeForceDerivative = -multiplier * result.gapHessian;
    result.work = result.force.head<3>().dot(newLeft - oldLeft) +
        result.force.tail<3>().dot(newRight - oldRight);
    return result;
}
}

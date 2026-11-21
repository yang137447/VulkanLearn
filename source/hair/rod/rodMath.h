#pragma once

#include "hair/rod/rodTypes.h"

namespace VL::Hair::Rod
{
Mat3 CrossMatrix(const Vec3& value);
Quat Exp(const Vec3& angle);
Vec3 Log(const Quat& orientation);
Mat3 RightJacobianInverse(const Vec3& angle);
Quat ApplyBodyIncrement(const Quat& orientation, const Vec3& angle);
Vec3 PredictBodyAngularVelocity(const Vec3& inertia, const Vec3& oldVelocity,
    const Vec3& bodyTorque, double stepSeconds, double& normalizedResidual);
RestState BuildRestState(const std::vector<Vec3>& positions,
    const Material& material = {},
    const std::vector<Quat>& orientations = {},
    const std::vector<double>& inverseMassMarkers = {});
State MakeAuthoredState(const RestState& rest);
}

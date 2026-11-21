#pragma once

#include "hair/rod/rodTypes.h"

namespace VL::Hair::Rod
{
struct ShearBlock
{
    Vec3 constraint;
    Mat3 firstPositionJacobian;
    Mat3 secondPositionJacobian;
    Mat3 orientationJacobian;
    Mat3 compliance;
};

struct BendBlock
{
    Vec3 constraint;
    Mat3 firstOrientationJacobian;
    Mat3 secondOrientationJacobian;
    Mat3 compliance;
};

ShearBlock EvaluateShearBlock(const RestState& rest, const State& state, size_t segment);
BendBlock EvaluateBendBlock(const RestState& rest, const State& state, size_t joint);
EnergyEvaluation EvaluateEnergy(const RestState& rest, const State& state);
}

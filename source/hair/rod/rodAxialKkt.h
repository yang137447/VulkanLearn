#pragma once

#include "hair/rod/rodAxialLimit.h"
#include "hair/rod/rodContactImplicit.h"

namespace VL::Hair::Rod
{
inline constexpr unsigned AxialLinearMathVersion = 2;
using AxialContactVector = Eigen::Matrix<double, 10, 1>;
using AxialContactMatrix = Eigen::Matrix<double, 10, 10>;

struct AxialContactActiveSet
{
    std::vector<ContactActiveNode> contacts;
    std::vector<bool> tensileActive;
    std::vector<double> tensileMultipliers;
};

struct AxialContactSystem
{
    std::vector<AxialContactMatrix> lower;
    std::vector<AxialContactMatrix> diagonal;
    std::vector<AxialContactMatrix> upper;
    std::vector<AxialContactVector> residual;
    double forceScale = 0.0;
    double lengthScale = 0.0;
};

AxialContactSystem AssembleAxialContactSystem(const RestState& rest, const State& old,
    const State& candidate, const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, const AxialContactActiveSet& active,
    const FrozenTangentAir* air = nullptr);
std::vector<AxialContactVector> SolveAxialContactSystem(const AxialContactSystem& system);
}

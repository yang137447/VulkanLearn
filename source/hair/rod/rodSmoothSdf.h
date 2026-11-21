#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "hair/rod/rodTypes.h"

namespace VL::Hair::Rod
{
inline constexpr char SmoothSdfIdentity[] = "cardinalCubicBSpline64V1";
inline constexpr unsigned SmoothSdfMathVersion = 1;

enum class SmoothSdfStatus
{
    Available,
    OutsideDomain,
    PositiveSaturated,
    NegativeSaturated,
    TruncatedSupport,
    InvalidInput,
    NonFinite
};

struct SmoothSdfGridSI
{
    std::array<std::uint32_t, 3> dimensions{};
    Vec3 origin = Vec3::Zero();
    double cellSize = 0.0;
    double band = 0.0;
    std::vector<double> coefficients;
};

struct SmoothSdfSample
{
    SmoothSdfStatus status = SmoothSdfStatus::InvalidInput;
    double value = 0.0;
    Vec3 gradient = Vec3::Zero();
    Mat3 hessian = Mat3::Zero();
    std::array<std::uint32_t, 3> supportBase{};
    double minimumCoefficient = 0.0;
    double maximumCoefficient = 0.0;
};

class SmoothSdfSampler
{
public:
    explicit SmoothSdfSampler(SmoothSdfGridSI grid);
    const SmoothSdfGridSI& Grid() const;
    SmoothSdfSample Sample(const Vec3& positionMetres) const;

private:
    SmoothSdfGridSI grid_;
    double inverseCell_ = 0.0;
    double inverseCellSquared_ = 0.0;
};
}

#include "hair/rod/rodSmoothSdf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace VL::Hair::Rod
{
namespace
{
struct CubicBasis
{
    std::array<double, 4> value;
    std::array<double, 4> first;
    std::array<double, 4> second;
};

CubicBasis Basis(double fraction)
{
    const double square = fraction * fraction;
    const double cube = square * fraction;
    const double remaining = 1.0 - fraction;
    return {{{remaining * remaining * remaining / 6.0,
                 (3.0 * cube - 6.0 * square + 4.0) / 6.0,
                 (-3.0 * cube + 3.0 * square + 3.0 * fraction + 1.0) / 6.0,
                 cube / 6.0}},
        {{-0.5 * remaining * remaining, 1.5 * square - 2.0 * fraction,
            -1.5 * square + fraction + 0.5, 0.5 * square}},
        {{remaining, 3.0 * fraction - 2.0, 1.0 - 3.0 * fraction, fraction}}};
}

std::size_t Index(const SmoothSdfGridSI& grid, std::uint32_t first,
    std::uint32_t second, std::uint32_t third)
{
    return static_cast<std::size_t>(first) + static_cast<std::size_t>(grid.dimensions[0]) *
        (static_cast<std::size_t>(second) + static_cast<std::size_t>(grid.dimensions[1]) * third);
}
}

SmoothSdfSampler::SmoothSdfSampler(SmoothSdfGridSI grid) : grid_(std::move(grid))
{
    std::size_t count = 1;
    for (const auto dimension : grid_.dimensions)
    {
        if (dimension < 4 || count > std::numeric_limits<std::size_t>::max() / dimension)
            throw std::invalid_argument("Smooth SDF dimensions require a representable full cubic support");
        count *= dimension;
    }
    if (count != grid_.coefficients.size() || !grid_.origin.allFinite() ||
        !std::isfinite(grid_.cellSize) || grid_.cellSize <= 0.0 ||
        !std::isfinite(grid_.band) || grid_.band <= 0.0)
        throw std::invalid_argument("Smooth SDF requires finite SI grid metadata and matching coefficients");
    inverseCell_ = 1.0 / grid_.cellSize;
    inverseCellSquared_ = inverseCell_ * inverseCell_;
    if (!std::isfinite(inverseCell_) || !std::isfinite(inverseCellSquared_))
        throw std::invalid_argument("Smooth SDF derivative scale is not representable");
    for (double value : grid_.coefficients)
        if (!std::isfinite(value) || value < -grid_.band || value > grid_.band)
            throw std::invalid_argument("Smooth SDF coefficients must be finite and within the authored band");
}

const SmoothSdfGridSI& SmoothSdfSampler::Grid() const
{
    return grid_;
}

SmoothSdfSample SmoothSdfSampler::Sample(const Vec3& positionMetres) const
{
    SmoothSdfSample result;
    if (!positionMetres.allFinite())
        return result;
    const Vec3 coordinate = (positionMetres - grid_.origin) * inverseCell_ - Vec3::Constant(0.5);
    if (!coordinate.allFinite())
    {
        result.status = SmoothSdfStatus::NonFinite;
        return result;
    }
    std::array<CubicBasis, 3> basis;
    for (int axis = 0; axis < 3; ++axis)
    {
        const double upper = static_cast<double>(grid_.dimensions[axis] - 2);
        if (coordinate[axis] < 1.0 || coordinate[axis] > upper)
        {
            result.status = SmoothSdfStatus::OutsideDomain;
            return result;
        }
        const auto cell = coordinate[axis] == upper ? grid_.dimensions[axis] - 3 :
            static_cast<std::uint32_t>(std::floor(coordinate[axis]));
        result.supportBase[axis] = cell - 1;
        basis[axis] = Basis(coordinate[axis] - static_cast<double>(cell));
    }
    const double reference = grid_.coefficients[Index(grid_, result.supportBase[0],
        result.supportBase[1], result.supportBase[2])];
    result.value = reference;
    result.minimumCoefficient = std::numeric_limits<double>::infinity();
    result.maximumCoefficient = -std::numeric_limits<double>::infinity();
    bool allPositive = true;
    bool allNegative = true;
    bool truncated = false;
    for (std::uint32_t third = 0; third < 4; ++third)
        for (std::uint32_t second = 0; second < 4; ++second)
            for (std::uint32_t first = 0; first < 4; ++first)
            {
                const double value = grid_.coefficients[Index(grid_, result.supportBase[0] + first,
                    result.supportBase[1] + second, result.supportBase[2] + third)];
                result.minimumCoefficient = std::min(result.minimumCoefficient, value);
                result.maximumCoefficient = std::max(result.maximumCoefficient, value);
                allPositive = allPositive && value == grid_.band;
                allNegative = allNegative && value == -grid_.band;
                truncated = truncated || value == grid_.band || value == -grid_.band;
                const double difference = value - reference;
                const std::array<std::uint32_t, 3> offsets{first, second, third};
                const double weight = basis[0].value[first] * basis[1].value[second] * basis[2].value[third];
                result.value += difference * weight;
                for (int axis = 0; axis < 3; ++axis)
                {
                    const int other = (axis + 1) % 3;
                    const int remaining = (axis + 2) % 3;
                    const double otherWeight = basis[other].value[offsets[other]] *
                        basis[remaining].value[offsets[remaining]];
                    result.gradient[axis] += difference * basis[axis].first[offsets[axis]] *
                        otherWeight * inverseCell_;
                    result.hessian(axis, axis) += difference * basis[axis].second[offsets[axis]] *
                        otherWeight * inverseCellSquared_;
                    for (int column = axis + 1; column < 3; ++column)
                    {
                        const int last = 3 - axis - column;
                        result.hessian(axis, column) += difference * basis[axis].first[offsets[axis]] *
                            basis[column].first[offsets[column]] * basis[last].value[offsets[last]] *
                            inverseCellSquared_;
                    }
                }
            }
    for (int axis = 0; axis < 3; ++axis)
        for (int column = axis + 1; column < 3; ++column)
            result.hessian(column, axis) = result.hessian(axis, column);
    if (!std::isfinite(result.value) || !result.gradient.allFinite() || !result.hessian.allFinite())
        result.status = SmoothSdfStatus::NonFinite;
    else if (allPositive)
        result.status = SmoothSdfStatus::PositiveSaturated;
    else if (allNegative)
        result.status = SmoothSdfStatus::NegativeSaturated;
    else if (truncated)
        result.status = SmoothSdfStatus::TruncatedSupport;
    else
        result.status = SmoothSdfStatus::Available;
    return result;
}
}

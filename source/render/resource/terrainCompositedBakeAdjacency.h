#pragma once

#include "terrain/terrainCompositedAdjacency.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace VL::TerrainBakeAdjacency
{
using Affine = std::array<double, 6>;

inline Affine Compose(const Affine& outer, const Affine& inner)
{
    return {outer[0] * inner[0] + outer[1] * inner[3],
        outer[0] * inner[1] + outer[1] * inner[4],
        outer[0] * inner[2] + outer[1] * inner[5] + outer[2],
        outer[3] * inner[0] + outer[4] * inner[3],
        outer[3] * inner[1] + outer[4] * inner[4],
        outer[3] * inner[2] + outer[4] * inner[5] + outer[5]};
}

inline Affine Inverse(const Affine& value)
{
    const double determinant = value[0] * value[4] - value[1] * value[3];
    Affine result{value[4] / determinant, -value[1] / determinant, 0,
        -value[3] / determinant, value[0] / determinant, 0};
    result[2] = -result[0] * value[2] - result[1] * value[5];
    result[5] = -result[3] * value[2] - result[4] * value[5];
    return result;
}

struct Chart
{
    std::string surfaceId;
    Affine sourceToTarget;
    Affine targetToComponent;
};

using PackedChart = std::array<float, 16>;

inline PackedChart Pack(const Chart& chart)
{
    const auto& mapping = chart.sourceToTarget;
    const auto& metric = chart.targetToComponent;
    PackedChart result{
        float(mapping[0]), float(mapping[1]), float(mapping[2]), 0,
        float(mapping[3]), float(mapping[4]), float(mapping[5]), 0,
        float(metric[0]), float(metric[1]), 0, 0,
        float(metric[3]), float(metric[4]), 0, 0};
    for (const float value : result)
        if (!std::isfinite(value))
            throw std::runtime_error("Bake chart exceeds float range");
    for (size_t index : {size_t(0), size_t(1), size_t(4), size_t(5)})
        if (mapping[index == 4 ? 3 : index == 5 ? 4 : index] != 0 && result[index] == 0)
            throw std::runtime_error("Bake chart Jacobian underflows float");
    const double mappingDeterminant = double(result[0]) * result[5] - double(result[1]) * result[4];
    const double metricDeterminant = double(result[8]) * result[13] - double(result[9]) * result[12];
    if (mappingDeterminant == 0 || metricDeterminant == 0)
        throw std::runtime_error("Bake chart becomes singular in float");
    const double mappedBound = 136 * std::max(
        std::abs(double(result[0])) + std::abs(double(result[1])),
        std::abs(double(result[4])) + std::abs(double(result[5]))) +
        std::max(std::abs(double(result[2])), std::abs(double(result[6]))) + 1;
    const double distanceBound = mappedBound * std::max(
        std::abs(double(result[8])) + std::abs(double(result[9])),
        std::abs(double(result[12])) + std::abs(double(result[13])));
    if (!std::isfinite(distanceBound) ||
        distanceBound > std::sqrt(double(std::numeric_limits<float>::max()) / 4))
        throw std::runtime_error("Bake chart distance may overflow for stored-page UV");
    return result;
}

inline uint32_t ResolveFloat(const std::vector<PackedChart>& charts, uint32_t source,
    const std::array<float, 2>& uv)
{
    if (charts.empty() || charts.size() > 256 || source >= charts.size() ||
        !std::isfinite(uv[0]) || !std::isfinite(uv[1]))
        throw std::runtime_error("Invalid bake route query");
    if (uv[0] >= 0 && uv[0] <= 1 && uv[1] >= 0 && uv[1] <= 1)
        return source;
    float best = std::numeric_limits<float>::infinity();
    uint32_t selected = UINT32_MAX;
    std::array<float, 2> bestDelta{};
    for (uint32_t index = 0; index < charts.size(); ++index)
    {
        const auto& chart = charts[index];
        const float mappedU = (chart[0] * uv[0] + chart[1] * uv[1]) + chart[2];
        const float mappedV = (chart[4] * uv[0] + chart[5] * uv[1]) + chart[6];
        const float deltaU = std::clamp(mappedU, 0.f, 1.f) - mappedU;
        const float deltaV = std::clamp(mappedV, 0.f, 1.f) - mappedV;
        const float distanceU = chart[8] * deltaU + chart[9] * deltaV;
        const float distanceV = chart[12] * deltaU + chart[13] * deltaV;
        const float distance = distanceU * distanceU + distanceV * distanceV;
        const float difference = (distanceU - bestDelta[0]) * (distanceU + bestDelta[0]) +
            (distanceV - bestDelta[1]) * (distanceV + bestDelta[1]);
        if (distance < best || (distance == best && difference < -best * 2.220446049250313e-16f))
        {
            best = distance;
            selected = index;
            bestDelta = {distanceU, distanceV};
        }
    }
    if (selected == UINT32_MAX)
        throw std::runtime_error("No finite bake route");
    return selected;
}

inline std::array<float, 16> NormalToSource(
    const std::array<float, 16>& source, const std::array<float, 16>& target)
{
    double sourceScaleSquared = 0;
    for (uint32_t row = 0; row < 3; ++row)
        sourceScaleSquared += double(source[row]) * source[row];
    std::array<float, 16> result{};
    result[15] = 1;
    for (uint32_t column = 0; column < 3; ++column)
        for (uint32_t row = 0; row < 3; ++row)
        {
            double rotation = 0;
            for (uint32_t axis = 0; axis < 3; ++axis)
                rotation += double(source[row * 4 + axis]) * target[column * 4 + axis];
            result[column * 4 + row] = float(rotation / sourceScaleSquared);
        }
    return result;
}

inline std::vector<Chart> Prepare(
    const TerrainCompositedAdjacencyGraph& graph, const std::string& source)
{
    const auto& ids = graph.SurfaceIds();
    graph.Validate(ids);
    const auto sourceFound = std::lower_bound(ids.begin(), ids.end(), source);
    if (sourceFound == ids.end() || *sourceFound != source)
        throw std::runtime_error("Bake adjacency source is absent from graph");
    const size_t sourceIndex = size_t(sourceFound - ids.begin());
    const auto& charts = graph.StaticCharts();
    std::vector<Chart> result;
    for (size_t target = 0; target < ids.size(); ++target)
        if (charts[target].component == charts[sourceIndex].component)
            result.push_back({ids[target],
                Compose(Inverse(charts[target].toComponent), charts[sourceIndex].toComponent),
                charts[target].toComponent});
    return result;
}

inline void ValidateAddressMode(const TerrainCompositedAdjacencyGraph& graph,
    const std::string& source, bool repeat)
{
    const auto charts = Prepare(graph, source);
    if (repeat && charts.size() != 1)
        throw std::runtime_error(
            "Repeat addressing is only supported for an explicitly isolated surface; connected adjacency worlds require Clamp");
}

inline bool RectangularBounds(const std::vector<Chart>& charts,
    std::vector<PackedChart>& packed)
{
    std::array<double, 2> minimum{std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    std::array<double, 2> maximum{-minimum[0], -minimum[1]};
    double area = 0;
    for (const auto& chart : charts)
    {
        const auto inverse = Inverse(chart.sourceToTarget);
        const std::array<double, 2> first{inverse[2], inverse[5]};
        const std::array<double, 2> last{inverse[0] + inverse[1] + inverse[2],
            inverse[3] + inverse[4] + inverse[5]};
        area += std::abs((last[0] - first[0]) * (last[1] - first[1]));
        for (uint32_t axis = 0; axis < 2; ++axis)
        {
            minimum[axis] = std::min({minimum[axis], first[axis], last[axis]});
            maximum[axis] = std::max({maximum[axis], first[axis], last[axis]});
        }
    }
    if (charts.empty() || area != (maximum[0] - minimum[0]) * (maximum[1] - minimum[1]))
        return false;
    for (uint32_t axis = 0; axis < 2; ++axis)
        if (double(float(minimum[axis])) != minimum[axis] ||
            double(float(maximum[axis])) != maximum[axis])
            return false;
    for (auto& chart : packed)
    {
        chart[10] = float(minimum[0]);
        chart[11] = float(minimum[1]);
        chart[14] = float(maximum[0]);
        chart[15] = float(maximum[1]);
    }
    return true;
}
}

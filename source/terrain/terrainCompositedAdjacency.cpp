#include "terrain/terrainCompositedAdjacency.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <utility>

namespace VL
{
namespace
{
using Transform = std::array<double, 6>;
using Point = std::array<double, 2>;
constexpr double Tolerance = 1.e-9;

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(std::string("Terrain composited adjacency: ") + message);
}

Point Apply(const Transform& transform, const Point& point)
{
    return {transform[0] * point[0] + transform[1] * point[1] + transform[2],
        transform[3] * point[0] + transform[4] * point[1] + transform[5]};
}

Transform Compose(const Transform& outer, const Transform& inner)
{
    return {outer[0] * inner[0] + outer[1] * inner[3],
        outer[0] * inner[1] + outer[1] * inner[4],
        outer[0] * inner[2] + outer[1] * inner[5] + outer[2],
        outer[3] * inner[0] + outer[4] * inner[3],
        outer[3] * inner[1] + outer[4] * inner[4],
        outer[3] * inner[2] + outer[4] * inner[5] + outer[5]};
}

Transform Inverse(const Transform& transform)
{
    for (const auto value : transform)
        Require(std::isfinite(value), "non-finite transform");
    const double determinant = transform[0] * transform[4] - transform[1] * transform[3];
    Require(std::isfinite(determinant) && std::abs(determinant) > Tolerance,
        "singular transform");
    Transform result{transform[4] / determinant, -transform[1] / determinant, 0,
        -transform[3] / determinant, transform[0] / determinant, 0};
    result[2] = -result[0] * transform[2] - result[1] * transform[5];
    result[5] = -result[3] * transform[2] - result[4] * transform[5];
    for (const auto value : result)
        Require(std::isfinite(value), "transform inverse overflow");
    return result;
}

template<size_t Count>
bool Near(const std::array<double, Count>& first, const std::array<double, Count>& second)
{
    for (size_t index = 0; index < Count; ++index)
        if (!std::isfinite(first[index]) || !std::isfinite(second[index]) ||
            std::abs(first[index] - second[index]) >
                Tolerance * std::max({1., std::abs(first[index]), std::abs(second[index])}))
            return false;
    return true;
}

Point EdgePoint(TerrainCompositedSurfaceEdge edge, double along)
{
    switch (edge)
    {
    case TerrainCompositedSurfaceEdge::MinU: return {0, along};
    case TerrainCompositedSurfaceEdge::MaxU: return {1, along};
    case TerrainCompositedSurfaceEdge::MinV: return {along, 0};
    case TerrainCompositedSurfaceEdge::MaxV: return {along, 1};
    }
    throw std::runtime_error("Terrain composited adjacency: invalid edge");
}

double InteriorDistance(TerrainCompositedSurfaceEdge edge, const Point& point)
{
    switch (edge)
    {
    case TerrainCompositedSurfaceEdge::MinU: return point[0];
    case TerrainCompositedSurfaceEdge::MaxU: return 1 - point[0];
    case TerrainCompositedSurfaceEdge::MinV: return point[1];
    case TerrainCompositedSurfaceEdge::MaxV: return 1 - point[1];
    }
    throw std::runtime_error("Terrain composited adjacency: invalid edge");
}

void Interval(const std::array<double, 2>& interval)
{
    Require(std::isfinite(interval[0]) && std::isfinite(interval[1]) &&
        interval[0] >= 0 && interval[1] <= 1 && interval[1] - interval[0] > Tolerance,
        "invalid edge interval");
}

bool Inside(const Point& point, double margin = 0)
{
    return point[0] >= margin && point[1] >= margin &&
        point[0] <= 1 - margin && point[1] <= 1 - margin;
}
}

TerrainCompositedAdjacencyGraph::TerrainCompositedAdjacencyGraph(
    std::vector<std::string> ids, std::vector<TerrainCompositedAdjacencyLink> connections)
    : surfaceIds(std::move(ids)), links(std::move(connections))
{
    Build();
}

void TerrainCompositedAdjacencyGraph::Validate(const std::vector<std::string>& expected) const
{
    auto ordered = expected;
    std::sort(ordered.begin(), ordered.end());
    Require(ordered == surfaceIds && charts.size() == surfaceIds.size(),
        "surface set differs from production closure");
}

void TerrainCompositedAdjacencyGraph::Build()
{
    Require(!surfaceIds.empty() && surfaceIds.size() <= 256 && links.size() <= 4096,
        "invalid topology size");
    std::sort(surfaceIds.begin(), surfaceIds.end());
    for (size_t index = 0; index < surfaceIds.size(); ++index)
        Require(!surfaceIds[index].empty() && surfaceIds[index].size() <= 1024 &&
            surfaceIds[index].find('\0') == std::string::npos &&
            (index == 0 || surfaceIds[index - 1] != surfaceIds[index]), "invalid/duplicate surface");
    const auto findIndex = [&](const std::string& id)
    {
        const auto found = std::lower_bound(surfaceIds.begin(), surfaceIds.end(), id);
        Require(found != surfaceIds.end() && *found == id, "unknown linked surface");
        return size_t(found - surfaceIds.begin());
    };
    for (const auto& link : links)
    {
        findIndex(link.sourceSurfaceId);
        findIndex(link.targetSurfaceId);
        Require(link.sourceSurfaceId != link.targetSurfaceId, "self link");
        Interval(link.sourceEdgeInterval);
        Interval(link.targetEdgeInterval);
        const auto inverse = Inverse(link.sourceUvToTargetUv);
        const auto& mapping = link.sourceUvToTargetUv;
        Require((mapping[1] == 0 && mapping[3] == 0) ||
            (mapping[0] == 0 && mapping[4] == 0), "unsupported non-rectangular UV transform");
        const auto first = Apply(link.sourceUvToTargetUv,
            EdgePoint(link.sourceEdge, link.sourceEdgeInterval[0]));
        const auto last = Apply(link.sourceUvToTargetUv,
            EdgePoint(link.sourceEdge, link.sourceEdgeInterval[1]));
        const auto targetFirst = EdgePoint(link.targetEdge, link.targetEdgeInterval[0]);
        const auto targetLast = EdgePoint(link.targetEdge, link.targetEdgeInterval[1]);
        Require((Near(first, targetFirst) && Near(last, targetLast)) ||
            (Near(first, targetLast) && Near(last, targetFirst)), "edge endpoints do not correspond");
        Require(InteriorDistance(link.targetEdge, Apply(link.sourceUvToTargetUv, {0.5, 0.5})) <
            -Tolerance, "linked interiors are on the same side");
        size_t reciprocals = 0;
        for (const auto& other : links)
        {
            if (&other == &link)
                continue;
            if (other.sourceSurfaceId == link.sourceSurfaceId && other.sourceEdge == link.sourceEdge)
                Require(std::min(other.sourceEdgeInterval[1], link.sourceEdgeInterval[1]) -
                    std::max(other.sourceEdgeInterval[0], link.sourceEdgeInterval[0]) <= Tolerance,
                    "overlapping edge links");
            if (other.sourceSurfaceId == link.targetSurfaceId &&
                other.targetSurfaceId == link.sourceSurfaceId && other.sourceEdge == link.targetEdge &&
                other.targetEdge == link.sourceEdge &&
                Near(other.sourceEdgeInterval, link.targetEdgeInterval) &&
                Near(other.targetEdgeInterval, link.sourceEdgeInterval) &&
                Near(other.sourceUvToTargetUv, inverse))
                ++reciprocals;
        }
        Require(reciprocals == 1, "missing/duplicate inverse link");
    }
    charts.resize(surfaceIds.size());
    std::vector<bool> assigned(surfaceIds.size(), false);
    for (size_t root = 0; root < surfaceIds.size(); ++root)
    {
        if (assigned[root])
            continue;
        assigned[root] = true;
        charts[root].component = root;
        std::queue<size_t> pending;
        pending.push(root);
        while (!pending.empty())
        {
            const auto current = pending.front();
            pending.pop();
            for (const auto& link : links)
            {
                if (link.sourceSurfaceId != surfaceIds[current])
                    continue;
                const auto target = findIndex(link.targetSurfaceId);
                const auto transform = Compose(charts[current].toComponent,
                    Inverse(link.sourceUvToTargetUv));
                Inverse(transform);
                if (assigned[target])
                    Require(charts[target].component == root &&
                        Near(charts[target].toComponent, transform),
                        "inconsistent cycle/corner transform");
                else
                {
                    assigned[target] = true;
                    charts[target] = {root, transform};
                    pending.push(target);
                }
            }
        }
    }
    for (size_t first = 0; first < charts.size(); ++first)
        for (size_t second = first + 1; second < charts.size(); ++second)
        {
            if (charts[first].component != charts[second].component)
                continue;
            const auto transform = Compose(Inverse(charts[first].toComponent), charts[second].toComponent);
            const auto cornerFirst = Apply(transform, {0, 0});
            const auto cornerLast = Apply(transform, {1, 1});
            const Point minimum{std::min(cornerFirst[0], cornerLast[0]),
                std::min(cornerFirst[1], cornerLast[1])};
            const Point maximum{std::max(cornerFirst[0], cornerLast[0]),
                std::max(cornerFirst[1], cornerLast[1])};
            const double overlapU = std::min(1., maximum[0]) - std::max(0., minimum[0]);
            const double overlapV = std::min(1., maximum[1]) - std::max(0., minimum[1]);
            Require(overlapU <= Tolerance || overlapV <= Tolerance, "overlapping surface interiors");
            const auto contact = [&](TerrainCompositedSurfaceEdge edge, const Point& interval)
            {
                size_t matching = 0;
                for (const auto& link : links)
                    if (link.sourceSurfaceId == surfaceIds[first] &&
                        link.targetSurfaceId == surfaceIds[second] && link.sourceEdge == edge &&
                        Near(link.sourceEdgeInterval, interval))
                        ++matching;
                Require(matching == 1, "geometric contact missing explicit full interval link");
            };
            if (overlapV > Tolerance && std::abs(minimum[0] - 1) <= Tolerance)
                contact(TerrainCompositedSurfaceEdge::MaxU, {std::max(0., minimum[1]), std::min(1., maximum[1])});
            if (overlapV > Tolerance && std::abs(maximum[0]) <= Tolerance)
                contact(TerrainCompositedSurfaceEdge::MinU, {std::max(0., minimum[1]), std::min(1., maximum[1])});
            if (overlapU > Tolerance && std::abs(minimum[1] - 1) <= Tolerance)
                contact(TerrainCompositedSurfaceEdge::MaxV, {std::max(0., minimum[0]), std::min(1., maximum[0])});
            if (overlapU > Tolerance && std::abs(maximum[1]) <= Tolerance)
                contact(TerrainCompositedSurfaceEdge::MinV, {std::max(0., minimum[0]), std::min(1., maximum[0])});
        }
}

TerrainCompositedAdjacencyAddress TerrainCompositedAdjacencyGraph::Resolve(
    const std::string& surfaceId, const std::array<double, 2>& uv) const
{
    Require(std::isfinite(uv[0]) && std::isfinite(uv[1]), "non-finite UV");
    const auto found = std::lower_bound(surfaceIds.begin(), surfaceIds.end(), surfaceId);
    Require(found != surfaceIds.end() && *found == surfaceId && charts.size() == surfaceIds.size(),
        "unknown/unvalidated surface");
    const size_t source = size_t(found - surfaceIds.begin());
    if (Inside(uv))
        return {surfaceId, uv, {1, 0, 0, 1}, 0, false};
    const auto componentPoint = Apply(charts[source].toComponent, uv);
    Require(std::isfinite(componentPoint[0]) && std::isfinite(componentPoint[1]), "UV overflow");
    std::vector<uint32_t> distances(surfaceIds.size(), UINT32_MAX);
    distances[source] = 0;
    std::queue<size_t> pending;
    pending.push(source);
    while (!pending.empty())
    {
        const auto current = pending.front();
        pending.pop();
        for (const auto& link : links)
            if (link.sourceSurfaceId == surfaceIds[current])
            {
                const auto target = size_t(std::lower_bound(surfaceIds.begin(), surfaceIds.end(),
                    link.targetSurfaceId) - surfaceIds.begin());
                if (distances[target] == UINT32_MAX)
                {
                    distances[target] = distances[current] + 1;
                    pending.push(target);
                }
            }
    }
    TerrainCompositedAdjacencyAddress result;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (size_t target = 0; target < charts.size(); ++target)
    {
        if (distances[target] == UINT32_MAX)
            continue;
        const auto transform = Compose(Inverse(charts[target].toComponent), charts[source].toComponent);
        const auto mapped = Apply(transform, uv);
        const Point clamped{std::clamp(mapped[0], 0., 1.), std::clamp(mapped[1], 0., 1.)};
        const auto candidate = Apply(charts[target].toComponent, clamped);
        const double distance = std::hypot(candidate[0] - componentPoint[0], candidate[1] - componentPoint[1]);
        Require(std::isfinite(distance), "UV distance overflow");
        if (distance < bestDistance)
        {
            bestDistance = distance;
            result = {surfaceIds[target], clamped,
                {transform[0], transform[1], transform[3], transform[4]}, distances[target], !Inside(mapped)};
        }
    }
    Require(!result.surfaceId.empty(), "unresolvable topology");
    return result;
}

}

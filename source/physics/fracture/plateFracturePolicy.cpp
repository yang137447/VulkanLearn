#include "physics/fracture/plateFracturePolicy.h"

#include <algorithm>
#include <cmath>

namespace VL::Physics::Fracture
{
namespace
{

double SegmentDistance(const Eigen::Vector2d& point,
    const Eigen::Vector2d& first, const Eigen::Vector2d& second)
{
    const Eigen::Vector2d edge = second - first;
    const double lengthSquared = edge.squaredNorm();
    if (lengthSquared <= 1e-12) return (point - first).norm();
    const double t = std::clamp(
        (point - first).dot(edge) / lengthSquared, 0.0, 1.0);
    return (point - (first + edge * t)).norm();
}

bool Adjacent(const Polygon2& first, const Polygon2& second, double tolerance)
{
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        for (std::size_t j = 0; j < second.size(); ++j)
        {
            if ((first[i] - second[j]).norm() < tolerance) return true;
            if (SegmentDistance(first[i], second[j],
                    second[(j + 1) % second.size()]) < tolerance)
                return true;
            if (SegmentDistance(second[j], first[i],
                    first[(i + 1) % first.size()]) < tolerance)
                return true;
        }
    }
    return false;
}

bool TouchesBorder(const Polygon2& polygon,
    double halfWidth, double halfHeight, double tolerance)
{
    for (const Eigen::Vector2d& point : polygon)
    {
        if (std::abs(point.x()) >= halfWidth - tolerance ||
            std::abs(point.y()) >= halfHeight - tolerance)
            return true;
    }
    return false;
}

} // namespace

double ComputePlateDamageRadius(double width, double height,
    float impactSpeed, const DestructibleProfile& profile)
{
    const double maxDimension = std::max(width, height);
    const float speedScale = std::clamp(impactSpeed / 8.0f, 0.0f, 1.0f);
    return std::clamp(
        maxDimension * (profile.damageRadiusBase +
            profile.damageRadiusSpeedScale * speedScale),
        maxDimension * profile.damageRadiusMinFraction,
        maxDimension * profile.damageRadiusMaxFraction);
}

bool IsPlateShardAttached(double distanceFromImpact, double damageRadius,
    const DestructibleProfile& profile)
{
    return distanceFromImpact >=
        damageRadius * profile.attachedDamageRadiusScale;
}

SubfractureDecision EvaluateSubfracture(double worldArea, int depth,
    float impactSpeed, bool projectileImpact,
    const DestructibleProfile& profile)
{
    if (depth >= profile.maxSubfractureDepth) return {};
    if (impactSpeed < profile.minSubfractureSpeed) return {};
    const double minimumArea = projectileImpact
        ? profile.minProjectileSubfractureArea
        : profile.minImpactSubfractureArea;
    if (worldArea < minimumArea) return {};
    const int crackCount =
        worldArea < profile.minImpactSubfractureArea ? 1 : 2;
    return {true, crackCount};
}

std::vector<std::size_t> FindUnsupportedPlateIndices(
    const std::vector<Polygon2>& paneOutlines,
    const std::vector<double>& worldAreas,
    double width, double height, double scale,
    const DestructibleProfile& profile)
{
    std::vector<std::size_t> dropped;
    if (paneOutlines.empty() || paneOutlines.size() != worldAreas.size())
        return dropped;
    const double safeScale = std::max(scale, 1.0e-4);
    // profile 容差是世界厘米；polygon 是局部坐标，比较前必须除以 scale。
    const double borderTolerance =
        profile.supportBorderTolerance / safeScale;
    const double adjacencyTolerance =
        profile.supportAdjacencyTolerance / safeScale;
    const double halfWidth = width * 0.5;
    const double halfHeight = height * 0.5;

    std::vector<bool> visited(paneOutlines.size(), false);
    std::vector<std::size_t> queue;
    for (std::size_t start = 0; start < paneOutlines.size(); ++start)
    {
        if (visited[start]) continue;
        std::vector<std::size_t> component;
        visited[start] = true;
        queue.clear();
        queue.push_back(start);
        for (std::size_t head = 0; head < queue.size(); ++head)
        {
            const std::size_t current = queue[head];
            component.push_back(current);
            for (std::size_t candidate = 0;
                candidate < paneOutlines.size(); ++candidate)
            {
                if (visited[candidate]) continue;
                if (!Adjacent(paneOutlines[current], paneOutlines[candidate],
                        adjacencyTolerance))
                    continue;
                visited[candidate] = true;
                queue.push_back(candidate);
            }
        }

        bool anchored = false;
        double componentArea = 0.0;
        for (std::size_t index : component)
        {
            componentArea += worldAreas[index];
            anchored = anchored || TouchesBorder(paneOutlines[index],
                halfWidth, halfHeight, borderTolerance);
        }
        if (!anchored || componentArea < profile.minAnchoredIslandArea)
            dropped.insert(dropped.end(), component.begin(), component.end());
    }
    std::sort(dropped.begin(), dropped.end());
    return dropped;
}

} // namespace VL::Physics::Fracture

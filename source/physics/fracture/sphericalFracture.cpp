#include "physics/fracture/sphericalFracture.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <Eigen/Geometry>

namespace VL::Physics::Fracture
{
namespace
{

constexpr double Pi = 3.14159265358979323846;
constexpr int MaxSites = 128;
constexpr double DirectionEpsilon = 1e-9;

double Clamp01(double value)
{
    return std::max(0.0, std::min(1.0, value));
}

struct SplitMix64
{
    uint64_t state = 0;

    uint64_t Next()
    {
        uint64_t value = (state += 0x9E3779B97F4A7C15ull);
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }

    double Uniform()
    {
        return static_cast<double>(Next() >> 11) *
            (1.0 / 9007199254740992.0);
    }
};

Eigen::Vector3d NormalizeOrThrow(const Eigen::Vector3d& direction, const char* message)
{
    const double length = direction.norm();
    if (length <= DirectionEpsilon)
        throw std::invalid_argument(message);
    return direction / length;
}

bool NearlySameDirection(const Eigen::Vector3d& first, const Eigen::Vector3d& second,
    double cosineThreshold)
{
    return first.dot(second) >= cosineThreshold;
}

void AddUniqueSite(std::vector<Eigen::Vector3d>& sites, const Eigen::Vector3d& candidate,
    double cosineThreshold)
{
    const Eigen::Vector3d unit = NormalizeOrThrow(candidate, "Spherical fracture site is zero");
    for (const Eigen::Vector3d& existing : sites)
        if (NearlySameDirection(existing, unit, cosineThreshold)) return;
    sites.push_back(unit);
}

std::vector<Eigen::Vector3d> BuildSites(const SphericalFractureSettings& settings)
{
    const Eigen::Vector3d impact = NormalizeOrThrow(
        settings.impactDirection, "Spherical fracture impact direction is zero");
    const int requested = std::max(4, std::min(settings.siteCount, MaxSites));
    SplitMix64 random{settings.seed};

    // 球面角去重阈值随精度固定；种子数很多时也不允许两个几乎重合的 site 产生退化 cell。
    const double cosineThreshold = std::cos(1e-4);
    std::vector<Eigen::Vector3d> sites;
    sites.reserve(static_cast<size_t>(requested));
    sites.push_back(impact);

    const int biasedCount = static_cast<int>(std::llround(
        static_cast<double>(requested - 1) * Clamp01(settings.impactBias)));
    if (biasedCount > 0)
    {
        const Eigen::Vector3d tangent = impact.unitOrthogonal();
        const Eigen::Vector3d bitangent = impact.cross(tangent).normalized();
        // 气球撕裂像从破口长出来的橡皮带：近密远疏，外圈种子更少，
        // 让 cell 沿经线拉长，并在外圈自然出现分叉/合并，而不是等宽橘子瓣。
        const int ringCount = std::max(2, std::min(4, biasedCount / 6));
        const int weightSum = ringCount * (ringCount + 1) / 2;
        int remaining = biasedCount;
        for (int ring = 0; ring < ringCount && remaining > 0; ++ring)
        {
            const int ringCountNow = (ring + 1 == ringCount)
                ? remaining
                : std::max(1, std::min(remaining,
                    static_cast<int>(std::llround(
                        static_cast<double>(biasedCount) *
                        static_cast<double>(ringCount - ring) /
                        static_cast<double>(weightSum)))));
            // 环角最多铺到约 135°，保证冲击附近更密、背面仍有覆盖。
            const double coneAngle = Pi * 0.75 *
                static_cast<double>(ring + 1) / static_cast<double>(ringCount + 1);
            const double angleOffset = random.Uniform() * 2.0 * Pi;
            for (int index = 0; index < ringCountNow && remaining > 0; ++index, --remaining)
            {
                const double step = 2.0 * Pi / static_cast<double>(ringCountNow);
                const double jitter = (random.Uniform() - 0.5) *
                    step * Clamp01(settings.angularJitter / 0.5);
                const double azimuth = angleOffset + step * static_cast<double>(index) + jitter;
                const Eigen::Vector3d candidate =
                    std::cos(coneAngle) * impact +
                    std::sin(coneAngle) * (std::cos(azimuth) * tangent +
                        std::sin(azimuth) * bitangent);
                AddUniqueSite(sites, candidate, cosineThreshold);
            }
        }
    }

    // 剩余种子用 Fibonacci 球均匀铺开；它们负责覆盖冲击环没有覆盖到的背面。
    const int uniformCount = std::max(0, requested - static_cast<int>(sites.size()));
    const double goldenAngle = Pi * (3.0 - std::sqrt(5.0));
    for (int index = 0; index < uniformCount; ++index)
    {
        const double z = 1.0 - 2.0 * (static_cast<double>(index) + 0.5) /
            static_cast<double>(uniformCount);
        const double radial = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double angle = goldenAngle * static_cast<double>(index);
        const Eigen::Vector3d candidate(
            radial * std::cos(angle),
            radial * std::sin(angle),
            z);
        AddUniqueSite(sites, candidate, cosineThreshold);
    }

    if (sites.size() < 4)
        throw std::runtime_error("Spherical fracture needs at least four distinct sites");
    return sites;
}

std::vector<Eigen::Vector3d> BuildCellBoundary(
    const std::vector<Eigen::Vector3d>& sites,
    size_t siteIndex)
{
    const Eigen::Vector3d& site = sites[siteIndex];
    std::vector<Eigen::Vector3d> vertices;
    for (size_t first = 0; first < sites.size(); ++first)
    {
        if (first == siteIndex) continue;
        const Eigen::Vector3d firstPlane = (site - sites[first]).normalized();
        for (size_t second = first + 1; second < sites.size(); ++second)
        {
            if (second == siteIndex) continue;
            const Eigen::Vector3d secondPlane = (site - sites[second]).normalized();
            const Eigen::Vector3d line = firstPlane.cross(secondPlane);
            if (line.norm() <= 1e-12) continue;
            const Eigen::Vector3d direction = line.normalized();
            const Eigen::Vector3d candidates[2] = {direction, -direction};
            for (const Eigen::Vector3d& candidate : candidates)
            {
                bool inside = true;
                for (size_t other = 0; other < sites.size(); ++other)
                {
                    if (other == siteIndex) continue;
                    if (candidate.dot(site - sites[other]) < -1e-8)
                    {
                        inside = false;
                        break;
                    }
                }
                if (!inside) continue;
                bool duplicate = false;
                for (const Eigen::Vector3d& existing : vertices)
                    if (NearlySameDirection(existing, candidate, std::cos(1e-7)))
                    {
                        duplicate = true;
                        break;
                    }
                if (!duplicate) vertices.push_back(candidate);
            }
        }
    }
    if (vertices.size() < 3) return {};

    // 以 site 为极点建立切平面坐标，按角度排序得到球面 CCW 边界。
    const Eigen::Vector3d tangent = site.unitOrthogonal();
    const Eigen::Vector3d bitangent = site.cross(tangent).normalized();
    std::sort(vertices.begin(), vertices.end(),
        [&tangent, &bitangent](const Eigen::Vector3d& left,
            const Eigen::Vector3d& right)
        {
            const double leftAngle = std::atan2(left.dot(bitangent), left.dot(tangent));
            const double rightAngle = std::atan2(right.dot(bitangent), right.dot(tangent));
            return leftAngle < rightAngle;
        });
    return vertices;
}

double SphericalTriangleArea(
    const Eigen::Vector3d& first,
    const Eigen::Vector3d& second,
    const Eigen::Vector3d& third)
{
    const double numerator = std::abs(first.dot(second.cross(third)));
    const double denominator = 1.0 + first.dot(second) + second.dot(third) +
        third.dot(first);
    return 2.0 * std::atan2(numerator, std::max(0.0, denominator));
}

double CellArea(const SphericalShard& shard)
{
    double area = 0.0;
    for (size_t index = 0; index < shard.boundary.size(); ++index)
        area += SphericalTriangleArea(
            shard.siteDirection,
            shard.boundary[index],
            shard.boundary[(index + 1) % shard.boundary.size()]);
    return area;
}

void AppendVertex(
    FragmentMesh& mesh,
    const Eigen::Vector3d& position,
    const Eigen::Vector3d& normal)
{
    mesh.positions.push_back(position.cast<float>());
    mesh.normals.push_back(normal.cast<float>());
}

void AppendTriangle(FragmentMesh& mesh, uint32_t first, uint32_t second, uint32_t third)
{
    mesh.indices.push_back(first);
    mesh.indices.push_back(second);
    mesh.indices.push_back(third);
}

} // namespace

SphericalPattern GenerateSphericalBurstPattern(
    const SphericalFractureSettings& settings)
{
    if (!std::isfinite(settings.radius) || settings.radius <= 0.0)
        throw std::invalid_argument("Spherical fracture radius must be positive");
    if (!std::isfinite(settings.thickness) || settings.thickness <= 0.0)
        throw std::invalid_argument("Spherical fracture thickness must be positive");
    if (settings.thickness >= settings.radius)
        throw std::invalid_argument("Spherical fracture thickness must be smaller than radius");

    const std::vector<Eigen::Vector3d> sites = BuildSites(settings);
    SphericalPattern pattern;
    pattern.shards.reserve(sites.size());
    for (size_t siteIndex = 0; siteIndex < sites.size(); ++siteIndex)
    {
        SphericalShard shard;
        shard.siteDirection = sites[siteIndex];
        shard.boundary = BuildCellBoundary(sites, siteIndex);
        if (shard.boundary.size() < 3) continue;
        const double unitArea = CellArea(shard);
        if (unitArea <= 1e-12) continue;
        shard.area = unitArea * settings.radius * settings.radius;
        shard.patternIndex = static_cast<uint32_t>(pattern.shards.size());
        pattern.coveredArea += shard.area;
        pattern.shards.push_back(std::move(shard));
    }
    if (pattern.shards.empty())
        throw std::runtime_error("Spherical fracture layout produced no cells");

    // 球面 Voronoi 应覆盖 4πR²。阈值只拦明显错误的拓扑，不把浮点边界当成失败。
    const double expected = 4.0 * Pi * settings.radius * settings.radius;
    if (pattern.coveredArea < expected * 0.98)
        throw std::runtime_error("Spherical fracture layout does not cover the sphere");
    return pattern;
}

FragmentMesh ExtrudeSphericalShard(
    const SphericalShard& shard,
    double radius,
    double thickness)
{
    if (shard.boundary.size() < 3)
        throw std::invalid_argument("Cannot extrude an empty spherical shard");
    if (!std::isfinite(radius) || radius <= 0.0 ||
        !std::isfinite(thickness) || thickness <= 0.0 ||
        thickness >= radius)
        throw std::invalid_argument("Spherical shard extrusion requires 0 < thickness < radius");

    const double innerRadius = radius - thickness;
    FragmentMesh mesh;
    Eigen::Vector3d center = Eigen::Vector3d::Zero();
    size_t centerCount = 0;

    // 先收集唯一凸包点，并求薄壳片的近似中心；渲染顶点随后按面复制。
    const Eigen::Vector3d outerCenter = shard.siteDirection * radius;
    const Eigen::Vector3d innerCenter = shard.siteDirection * innerRadius;
    mesh.convexHullPoints.push_back(outerCenter.cast<float>());
    mesh.convexHullPoints.push_back(innerCenter.cast<float>());
    center += outerCenter + innerCenter;
    centerCount += 2;
    for (const Eigen::Vector3d& direction : shard.boundary)
    {
        const Eigen::Vector3d outer = direction * radius;
        const Eigen::Vector3d inner = direction * innerRadius;
        mesh.convexHullPoints.push_back(outer.cast<float>());
        mesh.convexHullPoints.push_back(inner.cast<float>());
        center += outer + inner;
        centerCount += 2;
    }
    mesh.localCenter = (center / static_cast<double>(centerCount)).cast<float>();
    const Eigen::Vector3d local = mesh.localCenter.cast<double>();

    // 外表面：site 到边界的扇形三角化，法线沿球面径向朝外。
    for (size_t index = 0; index < shard.boundary.size(); ++index)
    {
        const Eigen::Vector3d first = shard.boundary[index];
        const Eigen::Vector3d second = shard.boundary[(index + 1) % shard.boundary.size()];
        const uint32_t start = static_cast<uint32_t>(mesh.positions.size());
        AppendVertex(mesh, outerCenter - local, shard.siteDirection);
        AppendVertex(mesh, first * radius - local, first);
        AppendVertex(mesh, second * radius - local, second);
        AppendTriangle(mesh, start, start + 1, start + 2);
    }

    // 内表面：反向绕序，法线朝球心。
    for (size_t index = 0; index < shard.boundary.size(); ++index)
    {
        const Eigen::Vector3d first = shard.boundary[index];
        const Eigen::Vector3d second = shard.boundary[(index + 1) % shard.boundary.size()];
        const uint32_t start = static_cast<uint32_t>(mesh.positions.size());
        AppendVertex(mesh, innerCenter - local, -shard.siteDirection);
        AppendVertex(mesh, second * innerRadius - local, -second);
        AppendVertex(mesh, first * innerRadius - local, -first);
        AppendTriangle(mesh, start, start + 1, start + 2);
    }

    // 侧壁顺序与外表面一致：outer_i -> inner_i -> inner_next -> outer_next。
    for (size_t index = 0; index < shard.boundary.size(); ++index)
    {
        const Eigen::Vector3d first = shard.boundary[index];
        const Eigen::Vector3d second = shard.boundary[(index + 1) % shard.boundary.size()];
        const Eigen::Vector3d outerFirst = first * radius;
        const Eigen::Vector3d outerSecond = second * radius;
        const Eigen::Vector3d innerFirst = first * innerRadius;
        const Eigen::Vector3d innerSecond = second * innerRadius;
        const Eigen::Vector3d normal = (innerFirst - outerFirst).cross(
            innerSecond - innerFirst).normalized();
        const uint32_t start = static_cast<uint32_t>(mesh.positions.size());
        AppendVertex(mesh, outerFirst - local, normal);
        AppendVertex(mesh, innerFirst - local, normal);
        AppendVertex(mesh, innerSecond - local, normal);
        AppendVertex(mesh, outerSecond - local, normal);
        AppendTriangle(mesh, start, start + 1, start + 2);
        AppendTriangle(mesh, start, start + 2, start + 3);
    }

    // 球壳片的真实体积由外球面面积对应的立体角沿径向积分得到，
    // 不能直接用 area * thickness：厚壳时两者会相差一个 O(t/R) 项。
    const double solidAngle = shard.area / (radius * radius);
    mesh.volume = solidAngle / 3.0 *
        (radius * radius * radius - innerRadius * innerRadius * innerRadius);
    return mesh;
}

} // namespace VL::Physics::Fracture

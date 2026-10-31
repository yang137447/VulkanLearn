#include "physics/fracture/planarFracture.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VL::Physics::Fracture
{
namespace
{

constexpr double Pi = 3.14159265358979323846;
constexpr double AreaEpsilon = 1e-12;
constexpr int MaxSites = 256;

double Clamp01(double value)
{
    return std::max(0.0, std::min(1.0, value));
}

bool NearlyEqual(const Point2& first, const Point2& second, double epsilon)
{
    return (first - second).squaredNorm() <= epsilon * epsilon;
}

bool PointInConvexPolygon(const Polygon2& polygon, const Point2& point, double tolerance)
{
    if (polygon.size() < 3) return false;
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const Point2& first = polygon[index];
        const Point2& second = polygon[(index + 1) % polygon.size()];
        const Point2 edge = second - first;
        const double cross = edge.x() * (point.y() - first.y()) -
            edge.y() * (point.x() - first.x());
        if (cross < -tolerance) return false;
    }
    return true;
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

Point2 ClampInsideDomain(const Polygon2& domain, const Point2& point, const Point2& fallback)
{
    if (PointInConvexPolygon(domain, point, 1e-9)) return point;

    // 凸域内从域外点向内部点做二分，避免把冲击种子直接夹到包围盒角上。
    Point2 low = fallback;
    Point2 high = point;
    if (!PointInConvexPolygon(domain, low, 1e-9))
        throw std::invalid_argument("Fracture domain has no valid interior point");
    for (int iteration = 0; iteration < 40; ++iteration)
    {
        const Point2 middle = (low + high) * 0.5;
        if (PointInConvexPolygon(domain, middle, 1e-9)) low = middle;
        else high = middle;
    }
    return low;
}

Point2 RandomPointInDomain(const Polygon2& domain, const Point2& fallback, SplitMix64& random)
{
    Point2 minimum = domain.front();
    Point2 maximum = domain.front();
    for (const Point2& point : domain)
    {
        minimum = minimum.cwiseMin(point);
        maximum = maximum.cwiseMax(point);
    }
    const Point2 extent = maximum - minimum;
    for (int attempt = 0; attempt < 256; ++attempt)
    {
        const Point2 candidate(
            minimum.x() + random.Uniform() * extent.x(),
            minimum.y() + random.Uniform() * extent.y());
        if (PointInConvexPolygon(domain, candidate, 1e-9)) return candidate;
    }
    return fallback;
}

double MaximumRadiusFrom(const Polygon2& domain, const Point2& origin)
{
    double radius = 0.0;
    for (const Point2& point : domain)
        radius = std::max(radius, (point - origin).norm());
    return radius;
}

std::vector<Point2> BuildSites(
    const Polygon2& domain,
    const Point2& impact,
    const PlanarFractureSettings& settings)
{
    SplitMix64 random{settings.seed};
    const Point2 fallback = PolygonCentroid(domain);
    const Point2 center = ClampInsideDomain(domain, impact, fallback);
    const double maxRadius = MaximumRadiusFrom(domain, center);
    const int requested = std::max(2, std::min(settings.siteCount, MaxSites));

    std::vector<Point2> sites;
    sites.reserve(static_cast<size_t>(requested));
    sites.push_back(center);

    const int radialCount = static_cast<int>(std::llround(
        static_cast<double>(requested - 1) * Clamp01(settings.radialBias)));
    if (radialCount > 0 && maxRadius > 1e-9)
    {
        // 环数随种子数增长但封顶，避免少量种子被摊成一条几乎重合的圆周。
        const int ringCount = std::max(1, std::min(4, radialCount / 4));
        int remaining = radialCount;
        for (int ring = 0; ring < ringCount && remaining > 0; ++ring)
        {
            const int ringsLeft = ringCount - ring;
            const int ringCountNow = std::max(1, remaining / ringsLeft);
            const double ringFraction = static_cast<double>(ring + 1) /
                static_cast<double>(ringCount + 1);
            const double radius = maxRadius * ringFraction *
                (0.85 + 0.30 * random.Uniform());
            const double angleOffset = random.Uniform() * 2.0 * Pi;
            for (int index = 0; index < ringCountNow && remaining > 0; ++index, --remaining)
            {
                const double step = 2.0 * Pi / static_cast<double>(ringCountNow);
                const double jitter = (random.Uniform() - 0.5) *
                    step * Clamp01(settings.jitterFraction);
                const double angle = angleOffset + step * static_cast<double>(index) + jitter;
                const Point2 candidate = center + Point2(
                    std::cos(angle) * radius,
                    std::sin(angle) * radius);
                sites.push_back(ClampInsideDomain(domain, candidate, fallback));
            }
        }
    }

    while (static_cast<int>(sites.size()) < requested)
        sites.push_back(RandomPointInDomain(domain, fallback, random));

    // 完全重复的种子会让 bisector 退化；保留最后出现的位置即可。
    std::vector<Point2> unique;
    unique.reserve(sites.size());
    for (const Point2& site : sites)
    {
        bool duplicate = false;
        for (const Point2& existing : unique)
            if (NearlyEqual(existing, site, 1e-10 * (1.0 + maxRadius)))
            {
                duplicate = true;
                break;
            }
        if (!duplicate) unique.push_back(site);
    }
    if (unique.size() < 2)
        throw std::runtime_error("Fracture layout needs at least two distinct sites");
    return unique;
}

void ValidateDomain(const Polygon2& domain)
{
    if (domain.size() < 3)
        throw std::invalid_argument("Fracture domain requires at least three points");
    if (!IsConvex(domain, 1e-9))
        throw std::invalid_argument("Fracture domain must be convex");
    if (SignedArea(domain) <= AreaEpsilon)
        throw std::invalid_argument("Fracture domain must be CCW with positive area");
}

Polygon2 ClipToConvexDomain(Polygon2 polygon, const Polygon2& domain)
{
    for (size_t index = 0; index < domain.size() && polygon.size() >= 3; ++index)
    {
        const Point2& first = domain[index];
        const Point2& second = domain[(index + 1) % domain.size()];
        const Point2 edge = second - first;
        // CCW 域的左侧是内部：keep (edge.y, -edge.x)·p <= (edge.y, -edge.x)·first。
        const Point2 normal(edge.y(), -edge.x());
        polygon = ClipPolygonByHalfPlane(polygon, normal, normal.dot(first));
    }
    return polygon;
}

void AppendShard(PlanarPattern& pattern, Polygon2 cell, double degenerateArea)
{
    if (cell.size() < 3) return;
    const double signedArea = SignedArea(cell);
    if (std::abs(signedArea) <= degenerateArea) return;
    if (signedArea < 0.0) std::reverse(cell.begin(), cell.end());

    PlanarShard shard;
    shard.outline = std::move(cell);
    shard.area = std::abs(signedArea);
    shard.centroid = PolygonCentroid(shard.outline);
    shard.patternIndex = static_cast<uint32_t>(pattern.shards.size());
    pattern.coveredArea += shard.area;
    pattern.shards.push_back(std::move(shard));
}

Point2 PolarPoint(const Point2& center, double angle, double radius)
{
    return center + Point2(std::cos(angle) * radius, std::sin(angle) * radius);
}

// 一个扇区 × 一个环带的 cell：内弧/外弧用弦近似，保持凸多边形和零缝隙拼接。
void AppendRadialCell(
    PlanarPattern& pattern,
    const Polygon2& domain,
    const Point2& center,
    double firstAngle,
    double secondAngle,
    double innerRadius,
    double outerRadius,
    double degenerateArea)
{
    if (outerRadius <= innerRadius + 1e-9 || secondAngle <= firstAngle + 1e-9)
        return;
    Polygon2 cell;
    if (innerRadius <= 1e-9)
    {
        cell = {center,
            PolarPoint(center, firstAngle, outerRadius),
            PolarPoint(center, secondAngle, outerRadius)};
    }
    else
    {
        cell = {PolarPoint(center, firstAngle, innerRadius),
            PolarPoint(center, firstAngle, outerRadius),
            PolarPoint(center, secondAngle, outerRadius),
            PolarPoint(center, secondAngle, innerRadius)};
    }
    cell = ClipToConvexDomain(std::move(cell), domain);
    AppendShard(pattern, std::move(cell), degenerateArea);
}

} // namespace

double SignedArea(const Polygon2& polygon)
{
    if (polygon.size() < 3) return 0.0;
    double twiceArea = 0.0;
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const Point2& first = polygon[index];
        const Point2& second = polygon[(index + 1) % polygon.size()];
        twiceArea += first.x() * second.y() - second.x() * first.y();
    }
    return twiceArea * 0.5;
}

Point2 PolygonCentroid(const Polygon2& polygon)
{
    if (polygon.empty()) return Point2::Zero();
    if (polygon.size() < 3) return polygon.front();

    const double area = SignedArea(polygon);
    if (std::abs(area) <= AreaEpsilon)
    {
        Point2 average = Point2::Zero();
        for (const Point2& point : polygon) average += point;
        return average / static_cast<double>(polygon.size());
    }

    Point2 centroid = Point2::Zero();
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const Point2& first = polygon[index];
        const Point2& second = polygon[(index + 1) % polygon.size()];
        const double cross = first.x() * second.y() - second.x() * first.y();
        centroid += (first + second) * cross;
    }
    return centroid / (6.0 * area);
}

bool IsConvex(const Polygon2& polygon, double tolerance)
{
    if (polygon.size() < 3) return false;
    double sign = 0.0;
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const Point2 first = polygon[(index + 1) % polygon.size()] - polygon[index];
        const Point2 second = polygon[(index + 2) % polygon.size()] -
            polygon[(index + 1) % polygon.size()];
        const double cross = first.x() * second.y() - first.y() * second.x();
        if (std::abs(cross) <= tolerance) continue;
        if (sign == 0.0) sign = cross;
        else if (cross * sign < 0.0) return false;
    }
    return true;
}

Polygon2 ClipPolygonByHalfPlane(
    const Polygon2& polygon,
    const Point2& normal,
    double offset)
{
    if (polygon.empty()) return {};
    const double length = normal.norm();
    if (length <= 1e-15) throw std::invalid_argument("Fracture clip plane has zero normal");
    const Point2 unitNormal = normal / length;
    const double unitOffset = offset / length;

    Polygon2 output;
    output.reserve(polygon.size() + 1);
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const Point2& first = polygon[index];
        const Point2& second = polygon[(index + 1) % polygon.size()];
        const double firstDistance = unitNormal.dot(first) - unitOffset;
        const double secondDistance = unitNormal.dot(second) - unitOffset;
        const bool firstInside = firstDistance <= 1e-12;
        const bool secondInside = secondDistance <= 1e-12;

        if (firstInside) output.push_back(first);
        if (firstInside != secondInside)
        {
            const double denominator = firstDistance - secondDistance;
            if (std::abs(denominator) > 1e-15)
            {
                const double fraction = firstDistance / denominator;
                output.push_back(first + (second - first) * fraction);
            }
        }
    }

    // 连续重复点会破坏后续面积/质心和三角化；裁剪后统一清一遍。
    Polygon2 cleaned;
    cleaned.reserve(output.size());
    for (const Point2& point : output)
        if (cleaned.empty() || !NearlyEqual(cleaned.back(), point, 1e-12))
            cleaned.push_back(point);
    if (cleaned.size() > 1 && NearlyEqual(cleaned.front(), cleaned.back(), 1e-12))
        cleaned.pop_back();
    return cleaned;
}

PlanarPattern GenerateImpactPattern(
    const Polygon2& domain,
    const Point2& impact,
    const PlanarFractureSettings& settings)
{
    ValidateDomain(domain);
    const std::vector<Point2> sites = BuildSites(domain, impact, settings);

    PlanarPattern pattern;
    pattern.shards.reserve(sites.size());
    for (size_t siteIndex = 0; siteIndex < sites.size(); ++siteIndex)
    {
        Polygon2 cell = domain;
        for (size_t otherIndex = 0; otherIndex < sites.size() && cell.size() >= 3; ++otherIndex)
        {
            if (siteIndex == otherIndex) continue;
            const Point2 normal = sites[otherIndex] - sites[siteIndex];
            const double length = normal.norm();
            if (length <= 1e-15) continue;
            const Point2 unitNormal = normal / length;
            const double offset = unitNormal.dot((sites[siteIndex] + sites[otherIndex]) * 0.5);
            cell = ClipPolygonByHalfPlane(cell, unitNormal, offset);
        }
        if (cell.size() < 3) continue;
        const double area = SignedArea(cell);
        if (area <= settings.degenerateArea) continue;
        if (area < 0.0) std::reverse(cell.begin(), cell.end());

        PlanarShard shard;
        shard.outline = std::move(cell);
        shard.area = std::abs(area);
        shard.centroid = PolygonCentroid(shard.outline);
        shard.patternIndex = static_cast<uint32_t>(pattern.shards.size());
        pattern.coveredArea += shard.area;
        pattern.shards.push_back(std::move(shard));
    }
    if (pattern.shards.empty())
        throw std::runtime_error("Fracture layout produced no non-degenerate shards");
    return pattern;
}

PlanarPattern SplitPlanarShard(
    const PlanarShard& shard,
    const PlanarFractureSettings& settings)
{
    if (shard.outline.size() < 3 || shard.area <= AreaEpsilon)
        throw std::invalid_argument("Cannot split an empty fracture shard");
    return GenerateImpactPattern(shard.outline, shard.centroid, settings);
}

PlanarPattern GenerateRadialImpactPattern(
    const Polygon2& domain,
    const Point2& impact,
    const RadialFractureSettings& settings)
{
    ValidateDomain(domain);
    SplitMix64 random{settings.seed};
    const Point2 fallback = PolygonCentroid(domain);
    const Point2 center = ClampInsideDomain(domain, impact, fallback);
    double maxRadius = MaximumRadiusFrom(domain, center) * 1.02;
    if (maxRadius <= 1e-9)
        throw std::runtime_error("Radial fracture needs a non-degenerate domain");

    const int cracks = std::max(3, std::min(settings.radialCracks, 64));
    const int ringLimit = std::max(1, std::min(settings.ringCount, 10));
    const double growth = std::max(1.05, std::min(settings.ringGrowth, 4.0));
    const double angularJitter = Clamp01(settings.angularJitter);
    const double ringJitter = std::max(0.0, std::min(settings.ringJitter, 0.8));
    const double dicingProbability = Clamp01(settings.dicingProbability);
    double coreRadius = std::max(settings.coreRadius, maxRadius * 1e-4);
    // 粉碎区过大时所有环都会被压到外侧，失去“内密外疏”的尺寸梯度。
    coreRadius = std::min(coreRadius, maxRadius * 0.35);
    // 0 = 一直覆盖到边界；> 0 则在伤害半径处收口，圈外由调用方保持完整。
    const double stopRadius =
        settings.stopRadius > 0.0
            ? std::clamp(settings.stopRadius,
                coreRadius * 1.05, maxRadius * 0.98)
            : maxRadius;
    // 环半径在 core..stop 之间重新按几何级数分配，保证最后一圈正好落在伤害边界。
    const double effectiveGrowth =
        stopRadius < maxRadius * 0.999
            ? std::pow(stopRadius / coreRadius, 1.0 / static_cast<double>(ringLimit))
            : growth;

    // 主裂纹角度：等距基础上做抖动，再把宽度归一到 2π，保证覆盖整个圆周。
    std::vector<double> widths(static_cast<size_t>(cracks));
    double totalWidth = 0.0;
    for (double& width : widths)
    {
        width = std::max(0.15, 1.0 + angularJitter * (random.Uniform() * 2.0 - 1.0));
        totalWidth += width;
    }
    // 环带用弦近似圆弧；宽扇区的弦会切掉域角点，按最大半角放大覆盖半径。
    double maximumHalfAngle = 0.0;
    for (double width : widths)
        maximumHalfAngle = std::max(maximumHalfAngle, Pi * width / totalWidth);
    maxRadius = MaximumRadiusFrom(domain, center) /
        std::max(0.2, std::cos(maximumHalfAngle)) * 1.02;
    std::vector<double> angles(static_cast<size_t>(cracks + 1));
    angles[0] = random.Uniform() * 2.0 * Pi;
    for (int index = 0; index < cracks; ++index)
    {
        angles[static_cast<size_t>(index + 1)] =
            angles[static_cast<size_t>(index)] +
            2.0 * Pi * widths[static_cast<size_t>(index)] / totalWidth;
    }

    PlanarPattern pattern;
    for (int sector = 0; sector < cracks; ++sector)
    {
        const double firstAngle = angles[static_cast<size_t>(sector)];
        const double secondAngle = angles[static_cast<size_t>(sector + 1)];
        const double sectorWidth = secondAngle - firstAngle;

        // 母扇区可以永久分成 2-3 个细支；细支角度对所有半径一致，
        // 因此相邻环和冲击点三角共享完全相同的边界，不会产生重叠。
        std::vector<double> cuts{firstAngle};
        const int extraCracks =
            random.Uniform() < dicingProbability
                ? (random.Uniform() < dicingProbability * 0.25 ? 2 : 1)
                : 0;
        for (int extra = 0; extra < extraCracks; ++extra)
        {
            cuts.push_back(firstAngle + sectorWidth *
                (0.20 + 0.60 * random.Uniform()));
        }
        cuts.push_back(secondAngle);
        std::sort(cuts.begin(), cuts.end());

        // 冲击点粉碎区：细支在核心半径内各形成一个三角。
        for (size_t cut = 0; cut + 1 < cuts.size(); ++cut)
        {
            AppendRadialCell(pattern, domain, center,
                cuts[cut], cuts[cut + 1], 0.0, coreRadius,
                settings.degenerateArea);
        }

        int rings = ringLimit;
        if (random.Uniform() < 0.25) ++rings;
        if (random.Uniform() < 0.35) --rings;
        rings = std::max(1, std::min(rings, ringLimit + 1));

        double inner = coreRadius;
        for (int ring = 0; ring < rings && inner < maxRadius; ++ring)
        {
            double outer = inner * effectiveGrowth *
                (1.0 + ringJitter * (random.Uniform() * 2.0 - 1.0));
            outer = std::min(outer, stopRadius);
            if (outer <= inner * 1.02)
                outer = std::min(inner * 1.05 + 1e-6, maxRadius);
            if (outer <= inner + 1e-9) break;
            for (size_t cut = 0; cut + 1 < cuts.size(); ++cut)
            {
                AppendRadialCell(pattern, domain, center,
                    cuts[cut], cuts[cut + 1], inner, outer,
                    settings.degenerateArea);
            }
            inner = outer;
        }
        // 最外侧必须补到域边界，否则扇区之间会留下未覆盖的环形空隙。
        if (inner < maxRadius)
        {
            for (size_t cut = 0; cut + 1 < cuts.size(); ++cut)
            {
                AppendRadialCell(pattern, domain, center,
                    cuts[cut], cuts[cut + 1], inner, maxRadius,
                    settings.degenerateArea);
            }
        }
    }
    if (pattern.shards.empty())
        throw std::runtime_error("Radial fracture layout produced no non-degenerate shards");
    return pattern;
}

PlanarPattern SplitPlanarShardByCracks(
    const PlanarShard& shard,
    int cutCount,
    uint64_t seed)
{
    if (shard.outline.size() < 3 || shard.area <= AreaEpsilon)
        throw std::invalid_argument("Cannot split an empty fracture shard");
    SplitMix64 random{seed};
    std::vector<Polygon2> pieces{shard.outline};
    const int cuts = std::max(1, std::min(cutCount, 8));
    for (int cut = 0; cut < cuts; ++cut)
    {
        size_t target = 0;
        double targetArea = -1.0;
        for (size_t index = 0; index < pieces.size(); ++index)
        {
            const double area = std::abs(SignedArea(pieces[index]));
            if (area > targetArea)
            {
                targetArea = area;
                target = index;
            }
        }
        if (targetArea <= AreaEpsilon) break;
        const Polygon2 piece = pieces[target];
        const Point2 centroid = PolygonCentroid(piece);
        bool split = false;
        for (int attempt = 0; attempt < 6 && !split; ++attempt)
        {
            const double extent = std::sqrt(targetArea);
            const double anchorAngle = random.Uniform() * 2.0 * Pi;
            const double anchorRadius = extent * 0.22 * random.Uniform();
            const Point2 anchor = ClampInsideDomain(piece,
                centroid + Point2(std::cos(anchorAngle) * anchorRadius,
                    std::sin(anchorAngle) * anchorRadius),
                centroid);
            const double lineAngle = random.Uniform() * 2.0 * Pi;
            const Point2 normal(std::cos(lineAngle), std::sin(lineAngle));
            const double offset = normal.dot(anchor);
            Polygon2 first = ClipPolygonByHalfPlane(piece, normal, offset);
            Polygon2 second = ClipPolygonByHalfPlane(piece, -normal, -offset);
            if (std::abs(SignedArea(first)) <= AreaEpsilon ||
                std::abs(SignedArea(second)) <= AreaEpsilon)
                continue;
            pieces[target] = std::move(first);
            pieces.push_back(std::move(second));
            split = true;
        }
        if (!split) break;
    }

    PlanarPattern pattern;
    for (Polygon2& piece : pieces)
        AppendShard(pattern, std::move(piece), 1e-10);
    if (pattern.shards.empty())
        throw std::runtime_error("Crack split produced no non-degenerate shards");
    return pattern;
}

} // namespace VL::Physics::Fracture

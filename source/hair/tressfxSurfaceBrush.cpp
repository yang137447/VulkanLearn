#include "hair/tressfxSurfaceBrush.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace VL::Hair
{
namespace
{

constexpr float kEpsilon = 1.0e-7f;

Eigen::Vector3f SafeNormalize(const Eigen::Vector3f& value)
{
    const float length = value.norm();
    if (!std::isfinite(length) || length <= kEpsilon) return Eigen::Vector3f::Zero();
    return value / length;
}

float MaxScale(const Eigen::Matrix4f& localToWorld)
{
    const Eigen::Matrix3f linear = localToWorld.block<3, 3>(0, 0);
    return std::max(1.0e-6f,
        std::max(linear.col(0).norm(), std::max(linear.col(1).norm(), linear.col(2).norm())));
}

// edge0 > edge1 时定义反向 ramp：ratio=0 返回 1，ratio≥1 返回 0。
float SmoothStep(float edge0, float edge1, float value)
{
    if (std::abs(edge1 - edge0) <= kEpsilon) return value <= edge0 ? 1.0f : 0.0f;
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool FitPositionPlane(
    const std::vector<uint32_t>& candidates,
    const std::vector<Eigen::Vector3f>& positions,
    const std::vector<float>& weights,
    Eigen::Vector3f& center,
    Eigen::Vector3f& normal)
{
    if (candidates.size() < 3) return false;
    float weightSum = 0.0f;
    center.setZero();
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        center += positions[index] * weights[index];
        weightSum += weights[index];
    }
    if (weightSum <= kEpsilon) return false;
    center /= weightSum;

    Eigen::Matrix3f covariance = Eigen::Matrix3f::Zero();
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        const Eigen::Vector3f delta = positions[index] - center;
        covariance += weights[index] * (delta * delta.transpose());
    }
    covariance /= weightSum;
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> solver(covariance);
    if (solver.info() != Eigen::Success) return false;
    const Eigen::Vector3f eigenvalues = solver.eigenvalues();
    if (!eigenvalues.allFinite()) return false;
    const float scale = std::max(1.0e-12f, std::abs(eigenvalues[2]));
    if (std::abs(eigenvalues[1]) <= 1.0e-7f * scale) return false;
    normal = SafeNormalize(solver.eigenvectors().col(0));
    return !normal.isZero();
}

// 屏幕坐标 -> 相机射线。Sample 与 SelectLasso 必须共用同一套 NDC/FOV 约定，
// 否则套索多边形与笔刷 stamp 会落在不同的世界平面上。
bool BuildCameraRay(
    const Eigen::Vector2f& cursor,
    const BrushViewport& viewport,
    const BrushCamera& camera,
    Eigen::Vector3f& direction)
{
    if (viewport.width == 0 || viewport.height == 0) return false;
    const Eigen::Vector3f forward = SafeNormalize(camera.forward);
    const Eigen::Vector3f right = SafeNormalize(camera.right);
    const Eigen::Vector3f up = SafeNormalize(camera.up);
    if (forward.isZero() || right.isZero() || up.isZero()) return false;

    const float aspect = static_cast<float>(viewport.width) / static_cast<float>(viewport.height);
    const float tanHalfHorizontal = std::tan(
        camera.horizontalFovDegrees * 3.14159265358979323846f / 360.0f);
    const float tanHalfVertical = tanHalfHorizontal / std::max(aspect, kEpsilon);
    const float ndcX = 2.0f *
        (cursor.x() - static_cast<float>(viewport.x)) /
        static_cast<float>(viewport.width) - 1.0f;
    // UI 坐标 y 向下；相机 up 对应 NDC +y。
    const float ndcY = 1.0f - 2.0f *
        (cursor.y() - static_cast<float>(viewport.y)) /
        static_cast<float>(viewport.height);
    direction = SafeNormalize(
        forward + right * (ndcX * tanHalfHorizontal) +
        up * (ndcY * tanHalfVertical));
    return !direction.isZero();
}

// 屏幕点沿相机射线落到 brush plane；depth 超出近远裁剪时失败。
bool IntersectBrushPlane(
    const Eigen::Vector2f& cursor,
    const BrushViewport& viewport,
    const BrushCamera& camera,
    const Eigen::Vector3f& planePoint,
    const Eigen::Vector3f& planeNormal,
    Eigen::Vector3f& hit)
{
    Eigen::Vector3f ray;
    if (!BuildCameraRay(cursor, viewport, camera, ray)) return false;
    const float denominator = ray.dot(planeNormal);
    if (std::abs(denominator) <= 1.0e-5f) return false;
    const float depth = (planePoint - camera.position).dot(planeNormal) / denominator;
    if (!std::isfinite(depth) || depth <= camera.clipNear || depth >= camera.clipFar)
        return false;
    hit = camera.position + ray * depth;
    return true;
}

bool PointInPolygon(const std::vector<Eigen::Vector2f>& polygon, const Eigen::Vector2f& point)
{
    if (polygon.size() < 3) return false;
    bool inside = false;
    for (size_t index = 0, previous = polygon.size() - 1; index < polygon.size();
         previous = index++)
    {
        const Eigen::Vector2f& a = polygon[previous];
        const Eigen::Vector2f& b = polygon[index];
        const Eigen::Vector2f edge = b - a;
        const Eigen::Vector2f offset = point - a;
        const float cross = edge.x() * offset.y() - edge.y() * offset.x();
        const float lengthSquared = edge.squaredNorm();
        // 包含边界，避免套索边缘上的发根因浮点误差被随机排除。
        if (std::abs(cross) <= 1.0e-6f && lengthSquared > kEpsilon)
        {
            const float projection = offset.dot(edge) / lengthSquared;
            if (projection >= 0.0f && projection <= 1.0f) return true;
        }
        const bool crosses = ((a.y() > point.y()) != (b.y() > point.y()));
        if (crosses)
        {
            const float t = (point.y() - a.y()) / (b.y() - a.y());
            if (point.x() < a.x() + t * edge.x()) inside = !inside;
        }
    }
    return inside;
}

} // namespace

BrushStamp SurfaceBrush::Sample(
    const Eigen::Vector2f& cursor,
    const BrushViewport& viewport,
    const BrushCamera& camera,
    const TressfxRootCloud& roots,
    const Eigen::Matrix4f& rootLocalToWorld,
    const BrushSettings& settings) const
{
    BrushStamp stamp;
    if (viewport.width == 0 || viewport.height == 0 || roots.RootCount() == 0) return stamp;
    Eigen::Vector3f rayDirection;
    if (!BuildCameraRay(cursor, viewport, camera, rayDirection)) return stamp;
    stamp.rayDirection = rayDirection;

    const float aspect = static_cast<float>(viewport.width) / static_cast<float>(viewport.height);
    const float tanHalfHorizontal = std::tan(
        camera.horizontalFovDegrees * 3.14159265358979323846f / 360.0f);
    const float tanHalfVertical = tanHalfHorizontal / std::max(aspect, kEpsilon);
    const float scale = MaxScale(rootLocalToWorld);
    const float autoRadius = roots.CellSize() * 6.0f * scale;
    const float searchRadius = settings.planeSearchRadiusWorld > 0.0f
        ? settings.planeSearchRadiusWorld : autoRadius;
    const std::vector<uint32_t> candidateIndices = roots.QueryRay(
        camera.position, rayDirection, settings.maxRayDistanceWorld, searchRadius,
        rootLocalToWorld, settings.maxPlaneCandidates);
    stamp.candidateCount = static_cast<uint32_t>(candidateIndices.size());
    if (candidateIndices.empty()) return stamp;

    const std::vector<TressfxRootSample>& allRoots = roots.Roots();
    const Eigen::Matrix3f normalTransform =
        rootLocalToWorld.block<3, 3>(0, 0).inverse().transpose();
    const float depthBand = std::max(roots.CellSize() * 2.5f * scale, searchRadius * 0.35f);

    struct Candidate
    {
        Eigen::Vector3f position;
        Eigen::Vector3f normal;
        float depth;
        float distanceToRay;
        float weight;
        bool hasNormal;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(candidateIndices.size());
    for (uint32_t index : candidateIndices)
    {
        const TressfxRootSample& root = allRoots[index];
        const Eigen::Vector3f worldPosition = (rootLocalToWorld * Eigen::Vector4f(
            root.position.x(), root.position.y(), root.position.z(), 1.0f)).head<3>();
        const Eigen::Vector3f offset = worldPosition - camera.position;
        const float depth = offset.dot(rayDirection);
        if (depth <= camera.clipNear || depth >= camera.clipFar) continue;
        const Eigen::Vector3f closest = camera.position + rayDirection * depth;
        const float distanceToRay = (worldPosition - closest).norm();
        Eigen::Vector3f normal = root.hasNormal
            ? SafeNormalize(normalTransform * root.normal)
            : Eigen::Vector3f::Zero();
        const bool hasNormal = !normal.isZero();
        // 只把朝向相机的表面纳入平面拟合；背侧根点在同一 ray 上会污染平均值。
        if (hasNormal && normal.dot(-rayDirection) <= 0.05f) continue;
        const float facingWeight = hasNormal
            ? std::clamp(normal.dot(-rayDirection), 0.0f, 1.0f) : 0.25f;
        const float positionWeight = (0.2f + 0.8f * facingWeight) /
            (1.0f + (distanceToRay * distanceToRay) /
                std::max(searchRadius * searchRadius, kEpsilon));
        candidates.push_back({worldPosition, normal, depth, distanceToRay,
            positionWeight, hasNormal});
    }
    if (candidates.empty()) return stamp;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
    {
        if (left.depth != right.depth) return left.depth < right.depth;
        return left.distanceToRay < right.distanceToRay;
    });
    const float frontDepth = candidates.front().depth;
    std::vector<size_t> front;
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        if (candidates[index].depth > frontDepth + depthBand) break;
        front.push_back(index);
    }
    // 平面拟合只取 ray tube 内最近的一小圈根点：整块前半球会把平面的中心
    // 拉到头皮内部，近邻圈才是笔刷真正接触的局部切平面。
    std::sort(front.begin(), front.end(), [&](size_t left, size_t right)
    {
        if (candidates[left].distanceToRay != candidates[right].distanceToRay)
            return candidates[left].distanceToRay < candidates[right].distanceToRay;
        return candidates[left].depth < candidates[right].depth;
    });
    constexpr size_t MaxPlaneFitRoots = 96;
    if (front.size() > MaxPlaneFitRoots) front.resize(MaxPlaneFitRoots);

    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> normals;
    std::vector<float> weights;
    positions.reserve(front.size());
    normals.reserve(front.size());
    weights.reserve(front.size());
    Eigen::Vector3f weightedNormal = Eigen::Vector3f::Zero();
    float normalWeightSum = 0.0f;
    for (size_t index : front)
    {
        const Candidate& candidate = candidates[index];
        positions.push_back(candidate.position);
        normals.push_back(candidate.normal);
        weights.push_back(candidate.weight);
        if (candidate.hasNormal)
        {
            weightedNormal += candidate.normal * candidate.weight;
            normalWeightSum += candidate.weight;
        }
    }

    Eigen::Vector3f center = Eigen::Vector3f::Zero();
    float positionWeightSum = 0.0f;
    for (size_t index = 0; index < positions.size(); ++index)
    {
        center += positions[index] * weights[index];
        positionWeightSum += weights[index];
    }
    if (positionWeightSum <= kEpsilon) return stamp;
    center /= positionWeightSum;

    Eigen::Vector3f normal = normalWeightSum > kEpsilon
        ? SafeNormalize(weightedNormal) : Eigen::Vector3f::Zero();
    bool fallback = false;
    bool usedNearestRootFallback = false;
    if (normal.isZero())
    {
        std::vector<uint32_t> allIndices;
        allIndices.reserve(front.size());
        for (size_t index : front) allIndices.push_back(static_cast<uint32_t>(index));
        if (FitPositionPlane(allIndices, positions, weights, center, normal))
            fallback = true;
        else
        {
            // 位置 PCA 也退化：使用最近根点 + 相机朝向。不能调用 rootNormal 的零值。
            const Candidate& nearest = candidates.front();
            center = nearest.position;
            normal = -rayDirection;
            fallback = true;
            usedNearestRootFallback = true;
        }
    }
    if (normal.dot(-rayDirection) < 0.0f) normal = -normal;

    const float denominator = rayDirection.dot(normal);
    float depth = 0.0f;
    if (std::abs(denominator) > 1.0e-5f)
    {
        depth = (center - camera.position).dot(normal) / denominator;
    }
    else
    {
        depth = (center - camera.position).dot(rayDirection);
    }
    if (!std::isfinite(depth) || depth <= camera.clipNear || depth >= camera.clipFar)
        return stamp;
    Eigen::Vector3f hit = camera.position + rayDirection * depth;
    // “最近根点 + -viewDir” 的退化平面可能不与 ray 精确相交；最近根点就是 stamp center。
    if (usedNearestRootFallback) hit = center;

    const float radiusWorld = std::max(0.0f,
        settings.radiusPixels * 2.0f * tanHalfVertical * depth /
            static_cast<float>(viewport.height));
    if (!std::isfinite(radiusWorld)) return stamp;

    stamp.valid = true;
    stamp.position = hit;
    stamp.normal = normal;
    stamp.depth = depth;
    stamp.radiusWorld = radiusWorld;
    stamp.fallback = fallback;
    return stamp;
}

std::vector<RootHit> RootCloudSelector::Select(
    const TressfxRootCloud& roots,
    const BrushStamp& stamp,
    const Eigen::Matrix4f& rootLocalToWorld,
    const BrushSettings& settings) const
{
    std::vector<RootHit> result;
    if (!stamp.valid || stamp.radiusWorld <= 0.0f) return result;

    const float scale = MaxScale(rootLocalToWorld);
    const float angle = std::clamp(settings.maxNormalAngleDegrees, 0.0f, 180.0f);
    const float cosMaxAngle = std::cos(angle * 3.14159265358979323846f / 180.0f);
    const float thickness = settings.planeThicknessWorld > 0.0f
        ? settings.planeThicknessWorld
        : std::max(roots.CellSize() * 2.0f * scale, stamp.radiusWorld * 0.15f);
    const float queryRadius = std::sqrt(
        stamp.radiusWorld * stamp.radiusWorld + thickness * thickness);
    const std::vector<uint32_t> candidates = roots.QuerySphere(
        stamp.position, queryRadius, rootLocalToWorld, 0);
    const Eigen::Matrix3f normalTransform =
        rootLocalToWorld.block<3, 3>(0, 0).inverse().transpose();
    const Eigen::Vector3f brushNormal = SafeNormalize(stamp.normal);
    if (brushNormal.isZero()) return result;

    result.reserve(candidates.size());
    for (uint32_t index : candidates)
    {
        const TressfxRootSample& root = roots.Roots()[index];
        const Eigen::Vector3f worldPosition = (rootLocalToWorld * Eigen::Vector4f(
            root.position.x(), root.position.y(), root.position.z(), 1.0f)).head<3>();
        // 根法线缺失时只做 normalGate 的相机朝向回退；growthDirection 仍不参与。
        const Eigen::Vector3f rootNormal = root.hasNormal
            ? SafeNormalize(normalTransform * root.normal)
            : -stamp.rayDirection;
        if (rootNormal.isZero()) continue;
        const float normalAlignment = rootNormal.dot(brushNormal);
        if (normalAlignment < cosMaxAngle) continue;
        const Eigen::Vector3f delta = worldPosition - stamp.position;
        const float planeDistance = std::abs(delta.dot(brushNormal));
        if (planeDistance > thickness) continue;
        const Eigen::Vector3f projected = worldPosition - brushNormal * delta.dot(brushNormal);
        const float inPlane = (projected - stamp.position).norm();
        if (inPlane > stamp.radiusWorld) continue;

        const float ratio = inPlane / std::max(stamp.radiusWorld, kEpsilon);
        const float falloff = SmoothStep(1.0f, 0.55f, ratio);
        const float normalWeight = SmoothStep(cosMaxAngle, 1.0f, normalAlignment);
        const float depthWeight = SmoothStep(thickness, 0.0f, planeDistance);
        const float weight = falloff * normalWeight * depthWeight;
        if (!std::isfinite(weight) || weight <= settings.selectionThreshold) continue;
        result.push_back({index, weight});
    }
    std::sort(result.begin(), result.end(), [](const RootHit& left, const RootHit& right)
    {
        return left.guideIndex < right.guideIndex;
    });
    return result;
}

std::vector<RootHit> RootCloudSelector::SelectLasso(
    const TressfxRootCloud& roots,
    const BrushStamp& stamp,
    const Eigen::Matrix4f& rootLocalToWorld,
    const BrushViewport& viewport,
    const BrushCamera& camera,
    const std::vector<Eigen::Vector2f>& screenPolygon,
    const BrushSettings& settings) const
{
    std::vector<RootHit> result;
    if (!stamp.valid || screenPolygon.size() < 3) return result;
    const Eigen::Vector3f brushNormal = SafeNormalize(stamp.normal);
    if (brushNormal.isZero()) return result;

    // 平面内基：T 取任意与法线不平行的轴，B 由右手系补齐。
    const Eigen::Vector3f reference =
        std::abs(brushNormal.dot(Eigen::Vector3f::UnitX())) < 0.9f
            ? Eigen::Vector3f::UnitX() : Eigen::Vector3f::UnitY();
    const Eigen::Vector3f tangent =
        SafeNormalize(reference - brushNormal * reference.dot(brushNormal));
    if (tangent.isZero()) return result;
    const Eigen::Vector3f bitangent = brushNormal.cross(tangent).normalized();

    std::vector<Eigen::Vector2f> polygon;
    polygon.reserve(screenPolygon.size());
    for (const Eigen::Vector2f& screenPoint : screenPolygon)
    {
        Eigen::Vector3f hit;
        if (!IntersectBrushPlane(screenPoint, viewport, camera, stamp.position,
                brushNormal, hit))
            return result;
        const Eigen::Vector3f delta = hit - stamp.position;
        polygon.emplace_back(delta.dot(tangent), delta.dot(bitangent));
    }
    // 退化多边形（三点共线/面积为零）不产生选择。
    float doubleArea = 0.0f;
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const Eigen::Vector2f& a = polygon[index];
        const Eigen::Vector2f& b = polygon[(index + 1) % polygon.size()];
        doubleArea += a.x() * b.y() - b.x() * a.y();
    }
    float maxRadius = 0.0f;
    for (const Eigen::Vector2f& point : polygon)
        maxRadius = std::max(maxRadius, point.norm());
    if (std::abs(doubleArea) <= 1.0e-6f || maxRadius <= kEpsilon) return result;

    const float scale = MaxScale(rootLocalToWorld);
    const float angle = std::clamp(settings.maxNormalAngleDegrees, 0.0f, 180.0f);
    const float cosMaxAngle = std::cos(angle * 3.14159265358979323846f / 180.0f);
    const float thickness = settings.planeThicknessWorld > 0.0f
        ? settings.planeThicknessWorld
        : std::max(roots.CellSize() * 2.0f * scale,
            std::max(stamp.radiusWorld, 1.0f) * 0.15f);
    // QuerySphere 会同时覆盖多边形平面内范围与薄层厚度。
    const std::vector<uint32_t> candidates = roots.QuerySphere(
        stamp.position, std::sqrt(maxRadius * maxRadius + thickness * thickness),
        rootLocalToWorld, 0);
    const Eigen::Matrix3f normalTransform =
        rootLocalToWorld.block<3, 3>(0, 0).inverse().transpose();

    result.reserve(candidates.size());
    for (uint32_t index : candidates)
    {
        const TressfxRootSample& root = roots.Roots()[index];
        const Eigen::Vector3f worldPosition = (rootLocalToWorld * Eigen::Vector4f(
            root.position.x(), root.position.y(), root.position.z(), 1.0f)).head<3>();
        const Eigen::Vector3f rootNormal = root.hasNormal
            ? SafeNormalize(normalTransform * root.normal)
            : -stamp.rayDirection;
        if (rootNormal.isZero()) continue;
        const float normalAlignment = rootNormal.dot(brushNormal);
        if (normalAlignment < cosMaxAngle) continue;
        const Eigen::Vector3f delta = worldPosition - stamp.position;
        const float planeDistance = std::abs(delta.dot(brushNormal));
        if (planeDistance > thickness) continue;
        const Eigen::Vector2f planar(delta.dot(tangent), delta.dot(bitangent));
        if (!PointInPolygon(polygon, planar)) continue;

        const float normalWeight = SmoothStep(cosMaxAngle, 1.0f, normalAlignment);
        const float depthWeight = SmoothStep(thickness, 0.0f, planeDistance);
        // 不使用屏幕距离：同一屏幕像素在倾斜平面上的世界距离差异会污染权重。
        const float weight = normalWeight * depthWeight;
        if (!std::isfinite(weight) || weight <= settings.selectionThreshold) continue;
        result.push_back({index, weight});
    }
    std::sort(result.begin(), result.end(), [](const RootHit& left, const RootHit& right)
    {
        return left.guideIndex < right.guideIndex;
    });
    return result;
}

} // namespace VL::Hair

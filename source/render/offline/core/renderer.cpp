#include "core/renderer.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace VL::Offline {

void aimAreaLight(Scene& scene, Vec3 target)
{
    const Vec3 delta = target - scene.lightCenter;
    if (length(delta) < 1e-8) throw std::invalid_argument("area target equals center");
    const Vec3 n = normalize(delta);
    scene.areaU = normalize(cross(std::abs(n.y) < 0.99 ? Vec3{0, 1, 0}
                                                                     : Vec3{0, 0, 1}, n));
    scene.areaV = cross(n, scene.areaU);
}

bool intersectAreaLight(const Scene& scene, Ray ray, double& distance)
{
    const Vec3 n = cross(scene.areaU, scene.areaV);
    const double cosine = dot(n, ray.direction);
    if (std::abs(cosine) < 1e-12) return false;
    distance = dot(n, scene.lightCenter - ray.origin) / cosine;
    if (distance <= kEpsilon) return false;
    const Vec3 local = ray.origin + ray.direction * distance - scene.lightCenter;
    return std::abs(dot(local, scene.areaU)) <= 0.5 * scene.lightWidth &&
           std::abs(dot(local, scene.areaV)) <= 0.5 * scene.lightDepth;
}

double areaLightPdf(const Scene& scene, Vec3 position, Vec3 direction)
{
    double distance;
    if (!intersectAreaLight(scene, {position, direction}, distance)) return 0;
    const double cosine = dot(cross(scene.areaU, scene.areaV), -direction);
    return cosine > 1e-12
        ? distance * distance / (cosine * scene.lightWidth * scene.lightDepth) : 0;
}

void resampleYarn(YarnMaterial& yarn, int count)
{
    if (count < 1 || count > kMaxTangentSamples)
        throw std::logic_error("invalid tangent sample count");
    yarn.sampleCount = count;
    double total = 0.0;
    for (int i = 0; i < yarn.controlPoints - 1; ++i) {
        if (yarn.lengths[i] < 0.0)
            throw std::logic_error("negative yarn segment length");
        total += yarn.lengths[i];
    }
    if (total <= 0.0) throw std::logic_error("empty yarn tangent curve");
    for (int sample = 0; sample < count; ++sample) {
        const double distance = (sample + 0.5) * total / count;
        double start = 0.0;
        for (int segment = 0; segment < yarn.controlPoints - 1; ++segment) {
            const double segmentLength = yarn.lengths[segment];
            const double end = start + segmentLength;
            if (segmentLength > 0.0 && (distance < end ||
                segment == yarn.controlPoints - 2)) {
                const double t = std::clamp((distance - start) / segmentLength,
                                            0.0, 1.0);
                yarn.samples[sample] = yarn.offsets[segment] * (1.0 - t) +
                                       yarn.offsets[segment + 1] * t;
                break;
            }
            start = end;
        }
        const double radians = yarn.samples[sample] * kPi / 180.0;
        yarn.rotated[sample] = {std::cos(radians), std::sin(radians)};
    }
}

double component(Vec3 v, int axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

void expand(Bounds& bounds, Vec3 point)
{
    bounds.low = {std::min(bounds.low.x, point.x),
                  std::min(bounds.low.y, point.y),
                  std::min(bounds.low.z, point.z)};
    bounds.high = {std::max(bounds.high.x, point.x),
                   std::max(bounds.high.y, point.y),
                   std::max(bounds.high.z, point.z)};
}

void expand(Bounds& bounds, const Bounds& other)
{
    expand(bounds, other.low);
    expand(bounds, other.high);
}

Bounds triangleBounds(const Triangle& triangle)
{
    Bounds bounds;
    for (Vec3 vertex : triangle.vertices) expand(bounds, vertex);
    return bounds;
}

int buildBvh(Mesh& mesh, std::size_t first, std::size_t count,
             const std::atomic<std::uint64_t>* latestGeneration,
             std::uint64_t expectedGeneration)
{
    // 旧代准备在递归节点边界退出；无 token 的构建仍保持相同排序。
    if (latestGeneration &&
        latestGeneration->load(std::memory_order_relaxed) != expectedGeneration)
        throw std::runtime_error("Offline mesh prepare canceled");
    const int index = static_cast<int>(mesh.nodes.size());
    mesh.nodes.emplace_back();
    Bounds bounds;
    Bounds centers;
    for (std::size_t i = first; i < first + count; ++i) {
        const Bounds tri = triangleBounds(mesh.triangles[mesh.order[i]]);
        expand(bounds, tri);
        expand(centers, (tri.low + tri.high) * 0.5);
    }
    mesh.nodes[index].bounds = bounds;
    if (count <= 8) {
        mesh.nodes[index].first = first;
        mesh.nodes[index].count = count;
        return index;
    }
    const Vec3 extent = centers.high - centers.low;
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0
                   : extent.y >= extent.z ? 1 : 2;
    const std::size_t middle = first + count / 2;
    std::nth_element(mesh.order.begin() + first, mesh.order.begin() + middle,
                     mesh.order.begin() + first + count,
                     [&](std::size_t a, std::size_t b) {
                         const Bounds aa = triangleBounds(mesh.triangles[a]);
                         const Bounds bb = triangleBounds(mesh.triangles[b]);
                         return component(aa.low + aa.high, axis) <
                                component(bb.low + bb.high, axis);
                     });
    const int left = buildBvh(mesh, first, middle - first,
                              latestGeneration, expectedGeneration);
    const int right = buildBvh(mesh, middle, first + count - middle,
                               latestGeneration, expectedGeneration);
    mesh.nodes[index].left = left;
    mesh.nodes[index].right = right;
    return index;
}

int buildBvh(Mesh& mesh, std::size_t first, std::size_t count)
{
    return buildBvh(mesh, first, count, nullptr, 0);
}

bool intersectsBounds(const Bounds& bounds, Ray ray, double maxDistance)
{
    double near = kEpsilon;
    double far = maxDistance;
    for (int axis = 0; axis < 3; ++axis) {
        const double origin = component(ray.origin, axis);
        const double direction = component(ray.direction, axis);
        const double low = component(bounds.low, axis);
        const double high = component(bounds.high, axis);
        if (std::abs(direction) < 1e-12) {
            if (origin < low || origin > high) return false;
            continue;
        }
        double a = (low - origin) / direction;
        double b = (high - origin) / direction;
        if (a > b) std::swap(a, b);
        near = std::max(near, a);
        far = std::min(far, b);
        // 平面 section 的包围盒可以零厚度；near == far 是有效的接触点。
        if (near > far) return false;
    }
    return true;
}

void intersectTriangle(const Triangle& triangle, Ray ray, double roughness, Hit& hit)
{
    const Vec3 a = triangle.vertices[0];
    const Vec3 e1 = triangle.vertices[1] - a;
    const Vec3 e2 = triangle.vertices[2] - a;
    const Vec3 p = cross(ray.direction, e2);
    const double det = dot(e1, p);
    if (std::abs(det) < 1e-12) return;
    const double inv = 1.0 / det;
    const Vec3 t = ray.origin - a;
    const double u = dot(t, p) * inv;
    if (u < 0.0 || u > 1.0) return;
    const Vec3 q = cross(t, e1);
    const double v = dot(ray.direction, q) * inv;
    if (v < 0.0 || u + v > 1.0) return;
    const double distance = dot(e2, q) * inv;
    if (distance <= kEpsilon || distance >= hit.distance) return;
    Vec3 geometric = normalize(cross(e1, e2));
    Vec3 shading = triangle.normals[0] * (1.0 - u - v) +
                   triangle.normals[1] * u + triangle.normals[2] * v;
    if (length(shading) < 1e-12) shading = geometric;
    else shading = normalize(shading);
    // 薄布双面可见，沿几何法线定侧；平滑法线只参与 BRDF，不作为射线离面偏移方向。
    if (dot(geometric, ray.direction) > 0.0) geometric = -geometric;
    if (dot(shading, geometric) < 0.0) shading = -shading;
    if (dot(shading, ray.direction) >= 0.0) shading = geometric;
    hit.distance = distance;
    hit.position = ray.origin + ray.direction * distance;
    hit.normal = shading;
    hit.geometricNormal = geometric;
    hit.dpdu = triangle.dpdu;
    hit.dpdv = triangle.dpdv;
    hit.uv = {triangle.uv[0].x * (1.0 - u - v) +
                  triangle.uv[1].x * u + triangle.uv[2].x * v,
              triangle.uv[0].y * (1.0 - u - v) +
                  triangle.uv[1].y * u + triangle.uv[2].y * v};
    hit.roughness = roughness;
}

void intersectBvh(const Mesh& mesh, int index, Ray ray, double roughness, Hit& hit)
{
    const BvhNode& node = mesh.nodes[index];
    if (!intersectsBounds(node.bounds, ray, hit.distance)) return;
    if (node.left < 0) {
        for (std::size_t i = node.first; i < node.first + node.count; ++i)
            intersectTriangle(mesh.triangles[mesh.order[i]], ray, roughness, hit);
        return;
    }
    intersectBvh(mesh, node.left, ray, roughness, hit);
    intersectBvh(mesh, node.right, ray, roughness, hit);
}

namespace
{
Vec3 ApplyBasis(const std::array<Vec3, 3>& basis, Vec3 vector)
{
    return basis[0] * vector.x + basis[1] * vector.y + basis[2] * vector.z;
}

Vec3 ApplyTranspose(const std::array<Vec3, 3>& basis, Vec3 vector)
{
    return {dot(basis[0], vector), dot(basis[1], vector), dot(basis[2], vector)};
}

void IntersectInstance(const MeshInstance& instance, Ray ray, Hit& hit)
{
    const Ray local{ApplyBasis(instance.worldToLocal, ray.origin - instance.translation),
                    ApplyBasis(instance.worldToLocal, ray.direction)};
    if (!intersectsBounds(instance.mesh->bounds, local, hit.distance)) return;
    Hit candidate;
    candidate.distance = hit.distance;
    intersectBvh(*instance.mesh, 0, local, instance.roughness, candidate);
    if (candidate.distance >= hit.distance) return;

    candidate.position = ApplyBasis(instance.localToWorld, candidate.position) +
                         instance.translation;
    const double determinant = dot(
        instance.localToWorld[0],
        cross(instance.localToWorld[1], instance.localToWorld[2]));
    Vec3 geometric = normalize(ApplyTranspose(instance.worldToLocal,
                                               candidate.geometricNormal));
    if (determinant < 0.0) geometric = -geometric;
    if (dot(geometric, ray.direction) > 0.0) geometric = -geometric;
    Vec3 shading = normalize(ApplyTranspose(instance.worldToLocal, candidate.normal));
    if (dot(shading, geometric) < 0.0) shading = -shading;
    if (dot(shading, ray.direction) >= 0.0) shading = geometric;
    candidate.geometricNormal = geometric;
    candidate.normal = shading;
    candidate.dpdu = ApplyBasis(instance.localToWorld, candidate.dpdu);
    candidate.dpdv = ApplyBasis(instance.localToWorld, candidate.dpdv);
    candidate.instance = &instance;
    hit = candidate;
}
} // namespace

bool intersect(const Scene& scene, Ray ray, double maxDistance, Hit& hit)
{
    hit = {};
    hit.distance = maxDistance;
    // 唯一几何入口是 VL 冻结的实例；空列表必须直接未命中。
    for (const MeshInstance& instance : scene.instances)
        IntersectInstance(instance, ray, hit);
    return hit.distance < maxDistance;
}

// Estévez & Kulla 2017 式 (2)-(4)：直接散射沿用光侧柔化，
// 分层反照率表仍积分未柔化的倒数几何项。
Vec3 evaluateBsdf(const AlbedoTable& albedo, Hit hit, Vec3 view, Vec3 light)
{
    const MeshInstance& instance = *hit.instance;
    const Vec3 sampledBase = instance.baseColorMap
        ? instance.baseColor * sampleColorMap(*instance.baseColorMap, hit.uv)
        : instance.baseColor;
    const Vec3 sampledSheen = instance.sheenColorMap
        ? instance.sheenColor * sampleColorMap(*instance.sheenColorMap, hit.uv)
        : instance.sheenColor;
    if (dot(hit.normal, view) <= 0.0 || dot(hit.normal, light) <= 0.0) return {};
    if (instance.model == SurfaceModel::Lambert)
        return sampledBase / kPi;
    if (instance.model == SurfaceModel::AshikhminVelvet)
        return velvetBrdf(*instance.velvet, hit.normal, view, light);
    const double eV = albedo.sample(dot(hit.normal, view), hit.roughness);
    const double eL = albedo.sample(dot(hit.normal, light), hit.roughness);
    const double removed = std::max(eV, eL);
    return sampledBase * (Vec3{1.0, 1.0, 1.0} - sampledSheen * removed) / kPi +
           sampledSheen * sheenBrdf(hit.normal, view, light, hit.roughness,
                                  true);
}

Vec3 directLight(const Scene& scene, const AlbedoTable& albedo, Hit hit, Vec3 view,
                 int lightSamples, Random& rng)
{
    const bool yarn = hit.instance->model == SurfaceModel::ClothFiber;
    const FabricMaterial* fabric = hit.instance->fabric;
    // 面灯的所有材质都使用同一 MIS 估计器；纱线为 K/pdf，
    // Lambert/Sheen 为 f*cos/pdf，不能按命中材质切换灯光测度。
    if (scene.hasAreaEmitter)
        return directFabricAreaLight(scene, albedo, hit, view, lightSamples, rng);
    if (!scene.directionalLight) return {};
    const double noL = std::max(0.0, dot(hit.normal, scene.toLight));
    const double geometricSide = dot(hit.geometricNormal, scene.toLight);
    if ((noL <= 0.0 && !yarn) || (geometricSide <= 0.0 && !yarn))
        return {};
    Hit blocker;
    // Delta 方向光没有面积 PDF 或距离衰减，每个命中点仅需一次无限远遮挡检测。
    const Vec3 offset = hit.geometricNormal *
                        (geometricSide < 0.0 ? -kEpsilon : kEpsilon);
    if (intersect(scene, {hit.position + offset, scene.toLight},
                  std::numeric_limits<double>::infinity(), blocker))
        return {};
    return (yarn
        ? microcylinderResponse(hit, view, scene.toLight, *fabric) *
              scene.directionalIrradiance
        : evaluateBsdf(albedo, hit, view, scene.toLight) *
              (scene.directionalIrradiance * noL)) * scene.directionalColor;
}


DirectionSample sampleDirection(Hit hit, Random& rng)
{
    // Sheen 掠射瓣用非零均匀分量覆盖，Lambert 则只需余弦采样。
    const double cosineWeight =
        hit.instance->model == SurfaceModel::Lambert ? 1.0 : 0.5;
    const bool cosine = rng.next() < cosineWeight;
    const double phi = 2.0 * kPi * rng.next();
    const double u = rng.next();
    const double noL = cosine ? std::sqrt(1.0 - u) : u;
    const double sinTheta = std::sqrt(std::max(0.0, 1.0 - noL * noL));
    const Vec3 tangent = normalize(cross(
        std::abs(hit.normal.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{0.0, 1.0, 0.0},
        hit.normal));
    const Vec3 bitangent = cross(hit.normal, tangent);
    const Vec3 direction = tangent * (sinTheta * std::cos(phi)) +
                           bitangent * (sinTheta * std::sin(phi)) + hit.normal * noL;
    return {direction, directionPdf(hit, direction)};
}

double directionPdf(Hit hit, Vec3 direction)
{
    const double noL = dot(hit.normal, direction);
    if (noL <= 0.0) return 0.0;
    const double cosineWeight =
        hit.instance->model == SurfaceModel::Lambert ? 1.0 : 0.5;
    return cosineWeight * noL / kPi +
           (1.0 - cosineWeight) / (2.0 * kPi);
}

double powerWeight(double a, double b)
{
    if (a <= 0.0) return 0.0;
    const double ratio = b / a;
    return 1.0 / (1.0 + ratio * ratio);
}

Vec3 directFabricAreaLight(const Scene& scene, const AlbedoTable& albedo,
                          Hit hit, Vec3 view, int lightSamples, Random& rng)
{
    const bool yarn = hit.instance->model == SurfaceModel::ClothFiber;
    const FabricMaterial* fabric = hit.instance->fabric;
    const auto kernel = [&](Vec3 l) {
        return yarn ? microcylinderResponse(hit, view, l, *fabric)
                    : evaluateBsdf(albedo, hit, view, l) *
                          std::max(0.0, dot(hit.normal, l));
    };
    const auto pdf = [&](Vec3 l) {
        return yarn ? microcylinderDirectionPdf(hit, view, l, *fabric)
                    : directionPdf(hit, l);
    };
    const auto visible = [&](Vec3 l, double distance) {
        const double side = dot(hit.geometricNormal, l);
        if (!yarn && side <= 0.0) return false;
        Hit blocker;
        const Vec3 origin = hit.position + hit.geometricNormal *
            (side < 0.0 ? -kEpsilon : kEpsilon);
        return !intersect(scene, {origin, l}, distance - kEpsilon, blocker);
    };
    Vec3 result{};
    const int strata = static_cast<int>(std::sqrt(lightSamples));
    const bool stratified = strata * strata == lightSamples;
    for (int i = 0; i < lightSamples; ++i) {
        double u = rng.next(), v = rng.next();
        if (stratified) {
            u = (i % strata + u) / strata;
            v = (i / strata + v) / strata;
        }
        const Vec3 position = scene.lightCenter +
            scene.areaU * ((u - 0.5) * scene.lightWidth) +
            scene.areaV * ((v - 0.5) * scene.lightDepth);
        const Vec3 delta = position - hit.position;
        const double distance = length(delta);
        if (distance <= kEpsilon) continue;
        const Vec3 l = delta / distance;
        const double cosine = dot(cross(scene.areaU, scene.areaV), -l);
        if (cosine <= 1e-12) continue;
        const double lightPdf = distance * distance /
            (cosine * scene.lightWidth * scene.lightDepth);
        if (!visible(l, distance)) continue;
        const double weight = powerWeight(lightSamples * lightPdf, pdf(l));
        result += kernel(l) * scene.areaColor *
                  (scene.areaRadiance * weight / (lightSamples * lightPdf));
    }
    // 两路直采共用 power heuristic；续射撞灯不能再次加发光。
    const DirectionSample sample = yarn
        ? sampleMicrocylinderDirection(hit, view, *fabric, rng)
        : sampleDirection(hit, rng);
    double distance;
    const double lightPdf = areaLightPdf(scene, hit.position, sample.direction);
    if (lightPdf > 0.0 && sample.pdf > 0.0 &&
        intersectAreaLight(scene, {hit.position, sample.direction}, distance) &&
        visible(sample.direction, distance)) {
        const double weight = powerWeight(sample.pdf, lightSamples * lightPdf);
        result += kernel(sample.direction) * scene.areaColor *
                  (scene.areaRadiance * weight / sample.pdf);
    }
    return result;
}

PathResult trace(const Scene& scene, const AlbedoTable& albedo, Ray ray,
                 int bounces, int lightSamples, Random& rng)
{
    PathResult result{};
    Vec3 throughput{1.0, 1.0, 1.0};
    for (int depth = 0; depth <= bounces; ++depth) {
        Hit hit;
        const bool found = intersect(scene, ray, std::numeric_limits<double>::infinity(), hit);
        double emitterDistance;
        if (scene.hasAreaEmitter &&
            intersectAreaLight(scene, ray, emitterDistance) &&
            (!found || emitterDistance < hit.distance)) {
            // 面光源直采已按 MIS 计入该顶点；续射命中不能再次加入发光。
            if (depth == 0 && dot(cross(scene.areaU, scene.areaV), -ray.direction) > 0)
                result.direct += scene.areaColor * scene.areaRadiance;
            break;
        }
        if (!found) {
            // 背景仅为首段显示色，不作为环境照明参与间接路径。
            if (depth == 0) result.direct += scene.backdrop;
            break;
        }
        if (hit.instance && hit.instance->model == SurfaceModel::ClothFiber)
            hit = orientYarns(hit, hit.instance->groom,
                              hit.instance->yarnRotationDegrees);
        const Vec3 contribution = throughput *
            directLight(scene, albedo, hit, -ray.direction, lightSamples, rng);
        if (depth == 0) result.direct += contribution;
        else result.indirect += contribution;
        if (depth == bounces) break;

        if (depth >= 2) {
            const double survival = std::clamp(
                std::max({throughput.x, throughput.y, throughput.z}), 0.05, 0.95);
            if (rng.next() >= survival) break;
            throughput = throughput / survival;
        }
        const bool yarn = hit.instance->model == SurfaceModel::ClothFiber;
        const FabricMaterial* fabric = hit.instance->fabric;
        const DirectionSample sample = yarn
            ? sampleMicrocylinderDirection(hit, -ray.direction, *fabric, rng)
            : sampleDirection(hit, rng);
        const double noL = std::max(0.0, dot(hit.normal, sample.direction));
        const double geometricSide = dot(hit.geometricNormal, sample.direction);
        if (sample.pdf <= 0.0 ||
            (!yarn && (noL == 0.0 || geometricSide <= 0.0))) break;
        // A 臂和 Lambert 为 f*NoL/pdf；B 臂的 K 已含纱线方向测度，只除 pdf。
        throughput = throughput * (yarn
            ? microcylinderResponse(hit, -ray.direction, sample.direction,
                                    *fabric) /
                  sample.pdf
            : evaluateBsdf(albedo, hit, -ray.direction, sample.direction) *
                  (noL / sample.pdf));
        if (std::max({throughput.x, throughput.y, throughput.z}) <= 0.0) break;
        const Vec3 offset = hit.geometricNormal *
                            (yarn && geometricSide < 0.0 ? -kEpsilon : kEpsilon);
        ray = {hit.position + offset, sample.direction};
    }
    return result;
}

} // namespace VL::Offline

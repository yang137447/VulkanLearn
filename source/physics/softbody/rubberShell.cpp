#include "physics/softbody/rubberShell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <Eigen/Geometry>

namespace VL::Physics::Softbody
{
namespace
{

using Face = std::array<uint32_t, 3>;

struct BurstRng
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

double TriangleSignedVolume(
    const Eigen::Vector3d& first,
    const Eigen::Vector3d& second,
    const Eigen::Vector3d& third)
{
    return first.dot(second.cross(third)) / 6.0;
}

uint64_t EdgeKey(uint32_t first, uint32_t second)
{
    const uint32_t low = std::min(first, second);
    const uint32_t high = std::max(first, second);
    return (static_cast<uint64_t>(low) << 32) | high;
}

std::pair<uint32_t, uint32_t> DecodeEdgeKey(uint64_t key)
{
    return {
        static_cast<uint32_t>(key >> 32),
        static_cast<uint32_t>(key & 0xffffffffu)};
}

std::vector<Eigen::Vector3d> BuildIcosahedron()
{
    const double golden = (1.0 + std::sqrt(5.0)) * 0.5;
    return {
        {-1.0, golden, 0.0}, {1.0, golden, 0.0},
        {-1.0, -golden, 0.0}, {1.0, -golden, 0.0},
        {0.0, -1.0, golden}, {0.0, 1.0, golden},
        {0.0, -1.0, -golden}, {0.0, 1.0, -golden},
        {golden, 0.0, -1.0}, {golden, 0.0, 1.0},
        {-golden, 0.0, -1.0}, {-golden, 0.0, 1.0}};
}

std::vector<Face> BuildIcosahedronFaces()
{
    return {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
        {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
        {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
}

uint32_t Midpoint(
    uint32_t first,
    uint32_t second,
    std::vector<Eigen::Vector3d>& vertices,
    std::map<uint64_t, uint32_t>& cache)
{
    const uint64_t key = EdgeKey(first, second);
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    Eigen::Vector3d midpoint = (vertices[first] + vertices[second]) * 0.5;
    midpoint.normalize();
    const uint32_t index = static_cast<uint32_t>(vertices.size());
    vertices.push_back(midpoint);
    cache.emplace(key, index);
    return index;
}

void SolveDistanceConstraint(
    ShellParticle& first,
    ShellParticle& second,
    const ShellDistanceConstraint& constraint,
    double deltaTime,
    double strainStiffening)
{
    // pinned 粒子在约束求解中也必须有零有效逆质量，否则边/弯曲约束会把它拖走。
    const double firstWeight = first.pinned ? 0.0 : first.inverseMass;
    const double secondWeight = second.pinned ? 0.0 : second.inverseMass;
    const double weightSum = firstWeight + secondWeight;
    if (weightSum <= 0.0) return;
    const Eigen::Vector3d delta = second.position - first.position;
    const double length = delta.norm();
    if (length <= 1e-12) return;
    const Eigen::Vector3d direction = delta / length;
    const double strain = constraint.restLength <= 1e-12 ? 0.0 :
        std::abs(length - constraint.restLength) / constraint.restLength;
    const double effectiveCompliance = constraint.compliance /
        (1.0 + std::max(0.0, strainStiffening) * strain);
    const double alpha = effectiveCompliance / (deltaTime * deltaTime);
    const double lambda = -(length - constraint.restLength) / (weightSum + alpha);
    first.position -= firstWeight * lambda * direction;
    second.position += secondWeight * lambda * direction;
}

struct ClipVertex
{
    Eigen::Vector3d position = Eigen::Vector3d::Zero();
    Eigen::Vector3d velocity = Eigen::Vector3d::Zero();
    Eigen::Vector3d barycentric = Eigen::Vector3d::Zero();
};

struct ClipPlane
{
    Eigen::Vector3d normal = Eigen::Vector3d::UnitZ();
};

std::vector<ClipPlane> BuildCellPlanes(const Fracture::SphericalShard& shard)
{
    std::vector<ClipPlane> planes;
    planes.reserve(shard.boundary.size());
    for (size_t index = 0; index < shard.boundary.size(); ++index)
    {
        const Eigen::Vector3d& first = shard.boundary[index];
        const Eigen::Vector3d& second =
            shard.boundary[(index + 1) % shard.boundary.size()];
        Eigen::Vector3d normal = first.cross(second);
        if (normal.norm() <= 1e-12) continue;
        normal.normalize();
        if (normal.dot(shard.siteDirection) < 0.0) normal = -normal;
        planes.push_back({normal});
    }
    return planes;
}

std::vector<ClipVertex> ClipPolygonByPlane(
    const std::vector<ClipVertex>& polygon,
    const ClipPlane& plane)
{
    std::vector<ClipVertex> output;
    output.reserve(polygon.size() + 1);
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const ClipVertex& first = polygon[index];
        const ClipVertex& second = polygon[(index + 1) % polygon.size()];
        const double firstDistance = plane.normal.dot(first.position);
        const double secondDistance = plane.normal.dot(second.position);
        const bool firstInside = firstDistance >= -1e-9;
        const bool secondInside = secondDistance >= -1e-9;
        if (firstInside) output.push_back(first);
        if (firstInside != secondInside)
        {
            const double denominator = firstDistance - secondDistance;
            if (std::abs(denominator) <= 1e-15) continue;
            const double fraction = firstDistance / denominator;
            ClipVertex intersection;
            intersection.position = first.position +
                (second.position - first.position) * fraction;
            intersection.velocity = first.velocity +
                (second.velocity - first.velocity) * fraction;
            intersection.barycentric = first.barycentric +
                (second.barycentric - first.barycentric) * fraction;
            output.push_back(intersection);
        }
    }

    std::vector<ClipVertex> cleaned;
    cleaned.reserve(output.size());
    for (const ClipVertex& vertex : output)
    {
        if (!cleaned.empty() &&
            (cleaned.back().position - vertex.position).norm() <= 1e-9)
            continue;
        cleaned.push_back(vertex);
    }
    if (cleaned.size() > 1 &&
        (cleaned.front().position - cleaned.back().position).norm() <= 1e-9)
        cleaned.pop_back();
    return cleaned;
}

using VertexKey = std::tuple<long long, long long, long long>;

VertexKey MakeVertexKey(const Eigen::Vector3d& position, double epsilon)
{
    return {
        static_cast<long long>(std::llround(position.x() / epsilon)),
        static_cast<long long>(std::llround(position.y() / epsilon)),
        static_cast<long long>(std::llround(position.z() / epsilon))};
}

} // namespace

RubberShell RubberShell::BuildIcosphere(const RubberShellSettings& settings)
{
    if (!std::isfinite(settings.radius) || settings.radius <= 0.0)
        throw std::invalid_argument("Rubber shell radius must be positive");
    if (settings.subdivisions < 0 || settings.subdivisions > 4)
        throw std::invalid_argument("Rubber shell subdivisions must be in [0, 4]");
    if (!std::isfinite(settings.totalMass) || settings.totalMass <= 0.0)
        throw std::invalid_argument("Rubber shell total mass must be positive");
    if (settings.constraintIterations < 1 || settings.constraintIterations > 32)
        throw std::invalid_argument("Rubber shell constraint iterations must be in [1, 32]");
    if (!std::isfinite(settings.strainStiffening) || settings.strainStiffening < 0.0)
        throw std::invalid_argument("Rubber shell strain stiffening must be non-negative");
    if (!std::isfinite(settings.edgeDamping) || settings.edgeDamping < 0.0)
        throw std::invalid_argument("Rubber shell edge damping must be non-negative");

    std::vector<Eigen::Vector3d> vertices = BuildIcosahedron();
    std::vector<Face> faces = BuildIcosahedronFaces();
    for (int subdivision = 0; subdivision < settings.subdivisions; ++subdivision)
    {
        std::map<uint64_t, uint32_t> cache;
        std::vector<Face> next;
        next.reserve(faces.size() * 4);
        for (const Face& face : faces)
        {
            const uint32_t first = Midpoint(face[0], face[1], vertices, cache);
            const uint32_t second = Midpoint(face[1], face[2], vertices, cache);
            const uint32_t third = Midpoint(face[2], face[0], vertices, cache);
            next.push_back({face[0], first, third});
            next.push_back({face[1], second, first});
            next.push_back({face[2], third, second});
            next.push_back({first, second, third});
        }
        faces = std::move(next);
    }
    for (Eigen::Vector3d& vertex : vertices) vertex = vertex.normalized() * settings.radius;

    // 标准 icosahedron 面表约定为外向；仍用有向体积校验，避免后续改动悄悄反转绕序。
    double signedVolume = 0.0;
    for (const Face& face : faces)
        signedVolume += TriangleSignedVolume(vertices[face[0]], vertices[face[1]], vertices[face[2]]);
    if (signedVolume < 0.0)
        for (Face& face : faces) std::swap(face[1], face[2]);

    RubberShell shell;
    shell.settings = settings;
    shell.triangles.reserve(faces.size());
    for (const Face& face : faces)
        shell.triangles.push_back({face[0], face[1], face[2]});

    const double inverseMass = static_cast<double>(vertices.size()) / settings.totalMass;
    shell.particles.reserve(vertices.size());
    for (const Eigen::Vector3d& vertex : vertices)
    {
        ShellParticle particle;
        particle.position = vertex;
        particle.previousPosition = vertex;
        particle.inverseMass = inverseMass;
        shell.particles.push_back(particle);
    }

    // 结构边和弯曲约束分开记录；弯曲约束连接共边两个三角面的对顶点。
    std::set<uint64_t> edges;
    std::map<uint64_t, std::vector<uint32_t>> edgeOpposites;
    for (const ShellTriangle& triangle : shell.triangles)
    {
        const uint32_t indices[3] = {triangle.first, triangle.second, triangle.third};
        for (int index = 0; index < 3; ++index)
        {
            const uint32_t first = indices[index];
            const uint32_t second = indices[(index + 1) % 3];
            const uint32_t opposite = indices[(index + 2) % 3];
            edges.insert(EdgeKey(first, second));
            edgeOpposites[EdgeKey(first, second)].push_back(opposite);
        }
    }
    for (uint64_t key : edges)
    {
        const auto [first, second] = DecodeEdgeKey(key);
        shell.constraints.push_back({
            first, second,
            (shell.particles[first].position - shell.particles[second].position).norm(),
            settings.edgeCompliance, true});
    }
    for (const auto& entry : edgeOpposites)
    {
        if (entry.second.size() != 2) continue;
        const uint32_t first = entry.second[0];
        const uint32_t second = entry.second[1];
        if (first == second) continue;
        shell.constraints.push_back({
            first, second,
            (shell.particles[first].position - shell.particles[second].position).norm(),
            settings.bendCompliance, false});
    }
    return shell;
}

std::vector<RubberShell> RubberShell::TearBySphericalPattern(
    const RubberShell& shell,
    const Fracture::SphericalPattern& pattern,
    const RubberTearSettings& tearSettings)
{
    if (pattern.shards.empty())
        throw std::invalid_argument("Rubber tear requires at least one spherical cell");
    if (!std::isfinite(tearSettings.restContraction) ||
        tearSettings.restContraction <= 0.0 ||
        tearSettings.restContraction > 1.0)
        throw std::invalid_argument("Rubber tear rest contraction must be in (0, 1]");
    if (!std::isfinite(tearSettings.fragmentVelocityScale) ||
        tearSettings.fragmentVelocityScale < 0.0)
        throw std::invalid_argument("Rubber tear velocity scale must be non-negative");

    std::vector<std::vector<uint32_t>> cellTriangles(pattern.shards.size());
    for (uint32_t triangleIndex = 0;
        triangleIndex < static_cast<uint32_t>(shell.triangles.size());
        ++triangleIndex)
    {
        const ShellTriangle& triangle = shell.triangles[triangleIndex];
        const Eigen::Vector3d center = (shell.particles[triangle.first].position +
            shell.particles[triangle.second].position +
            shell.particles[triangle.third].position) / 3.0;
        if (center.norm() <= 1e-12)
            throw std::runtime_error("Rubber tear triangle has zero center");
        const Eigen::Vector3d direction = center.normalized();
        size_t bestCell = 0;
        double bestDot = -2.0;
        for (size_t cell = 0; cell < pattern.shards.size(); ++cell)
        {
            const double dot = direction.dot(pattern.shards[cell].siteDirection);
            if (dot > bestDot)
            {
                bestDot = dot;
                bestCell = cell;
            }
        }
        cellTriangles[bestCell].push_back(triangleIndex);
    }

    std::vector<RubberShell> fragments;
    fragments.reserve(pattern.shards.size());
    for (const std::vector<uint32_t>& sourceTriangles : cellTriangles)
    {
        if (sourceTriangles.empty()) continue;
        RubberShell fragment;
        fragment.settings = shell.settings;
        std::map<uint32_t, uint32_t> remap;
        for (uint32_t sourceTriangleIndex : sourceTriangles)
        {
            const ShellTriangle& source = shell.triangles[sourceTriangleIndex];
            const uint32_t sourceIndices[3] = {
                source.first, source.second, source.third};
            uint32_t localIndices[3] = {};
            for (int corner = 0; corner < 3; ++corner)
            {
                const auto found = remap.find(sourceIndices[corner]);
                if (found != remap.end())
                {
                    localIndices[corner] = found->second;
                    continue;
                }
                const uint32_t local = static_cast<uint32_t>(fragment.particles.size());
                remap.emplace(sourceIndices[corner], local);
                ShellParticle particle = shell.particles[sourceIndices[corner]];
                particle.previousPosition = particle.position;
                particle.velocity *= tearSettings.fragmentVelocityScale;
                fragment.particles.push_back(particle);
                localIndices[corner] = local;
            }
            fragment.triangles.push_back({
                localIndices[0], localIndices[1], localIndices[2]});
        }

        fragment.RebuildConstraints(tearSettings.restContraction);
        if (!fragment.particles.empty() && !fragment.triangles.empty())
            fragments.push_back(std::move(fragment));
    }
    return fragments;
}

std::vector<RubberShell> RubberShell::TearBySphericalPatternExact(
    const RubberShell& shell,
    const Fracture::SphericalPattern& pattern,
    const RubberTearSettings& tearSettings)
{
    if (pattern.shards.empty())
        throw std::invalid_argument("Rubber tear requires at least one spherical cell");
    if (!std::isfinite(tearSettings.restContraction) ||
        tearSettings.restContraction <= 0.0 ||
        tearSettings.restContraction > 1.0)
        throw std::invalid_argument("Rubber tear rest contraction must be in (0, 1]");
    if (!std::isfinite(tearSettings.fragmentVelocityScale) ||
        tearSettings.fragmentVelocityScale < 0.0)
        throw std::invalid_argument("Rubber tear velocity scale must be non-negative");

    std::vector<RubberShell> fragments;
    fragments.reserve(pattern.shards.size());
    for (const Fracture::SphericalShard& shard : pattern.shards)
    {
        const std::vector<ClipPlane> planes = BuildCellPlanes(shard);
        if (planes.empty()) continue;

        RubberShell fragment;
        fragment.settings = shell.settings;
        const double keyEpsilon = std::max(1e-6, shell.settings.radius * 1e-7);
        std::map<VertexKey, uint32_t> remap;

        for (const ShellTriangle& triangle : shell.triangles)
        {
            const ShellParticle& firstParticle = shell.particles[triangle.first];
            const ShellParticle& secondParticle = shell.particles[triangle.second];
            const ShellParticle& thirdParticle = shell.particles[triangle.third];
            std::vector<ClipVertex> polygon = {
                {firstParticle.position, firstParticle.velocity, {1.0, 0.0, 0.0}},
                {secondParticle.position, secondParticle.velocity, {0.0, 1.0, 0.0}},
                {thirdParticle.position, thirdParticle.velocity, {0.0, 0.0, 1.0}}};
            for (const ClipPlane& plane : planes)
            {
                polygon = ClipPolygonByPlane(polygon, plane);
                if (polygon.size() < 3) break;
            }
            if (polygon.size() < 3) continue;

            std::vector<uint32_t> local;
            local.reserve(polygon.size());
            for (const ClipVertex& vertex : polygon)
            {
                Eigen::Vector3d weights = vertex.barycentric;
                const double weightSum = weights.sum();
                if (std::abs(weightSum) <= 1e-12) continue;
                weights /= weightSum;
                const double radius =
                    weights.x() * firstParticle.position.norm() +
                    weights.y() * secondParticle.position.norm() +
                    weights.z() * thirdParticle.position.norm();
                if (vertex.position.norm() <= 1e-12 || radius <= 1e-12) continue;
                const Eigen::Vector3d position = vertex.position.normalized() * radius;
                const VertexKey key = MakeVertexKey(position, keyEpsilon);
                const auto found = remap.find(key);
                if (found != remap.end())
                {
                    local.push_back(found->second);
                    continue;
                }
                ShellParticle particle;
                particle.position = position;
                particle.previousPosition = position;
                particle.velocity =
                    (weights.x() * firstParticle.velocity +
                        weights.y() * secondParticle.velocity +
                        weights.z() * thirdParticle.velocity) *
                    tearSettings.fragmentVelocityScale;
                particle.inverseMass =
                    weights.x() * firstParticle.inverseMass +
                    weights.y() * secondParticle.inverseMass +
                    weights.z() * thirdParticle.inverseMass;
                const uint32_t localIndex = static_cast<uint32_t>(fragment.particles.size());
                remap.emplace(key, localIndex);
                fragment.particles.push_back(particle);
                local.push_back(localIndex);
            }
            if (local.size() < 3) continue;

            // 裁剪后仍是凸多边形，直接围绕第一个顶点扇形三角化。
            for (size_t index = 1; index + 1 < local.size(); ++index)
            {
                const Eigen::Vector3d& first = fragment.particles[local[0]].position;
                const Eigen::Vector3d& second = fragment.particles[local[index]].position;
                const Eigen::Vector3d& third = fragment.particles[local[index + 1]].position;
                if ((second - first).cross(third - first).norm() <= 1e-12) continue;
                fragment.triangles.push_back({local[0], local[index], local[index + 1]});
            }
        }

        fragment.RebuildConstraints(tearSettings.restContraction);
        if (!fragment.particles.empty() && !fragment.triangles.empty())
            fragments.push_back(std::move(fragment));
    }
    return fragments;
}

void RubberShell::RebuildConstraints(double restLengthScale)
{
    constraints.clear();
    std::set<uint64_t> edges;
    std::map<uint64_t, std::vector<uint32_t>> edgeOpposites;
    for (const ShellTriangle& triangle : triangles)
    {
        const uint32_t indices[3] = {
            triangle.first, triangle.second, triangle.third};
        for (int index = 0; index < 3; ++index)
        {
            const uint32_t first = indices[index];
            const uint32_t second = indices[(index + 1) % 3];
            const uint32_t opposite = indices[(index + 2) % 3];
            edges.insert(EdgeKey(first, second));
            edgeOpposites[EdgeKey(first, second)].push_back(opposite);
        }
    }
    for (uint64_t key : edges)
    {
        const auto [first, second] = DecodeEdgeKey(key);
        const double length = (particles[first].position - particles[second].position).norm();
        constraints.push_back({
            first, second, length * restLengthScale,
            settings.edgeCompliance, true});
    }
    for (const auto& entry : edgeOpposites)
    {
        if (entry.second.size() != 2) continue;
        const uint32_t first = entry.second[0];
        const uint32_t second = entry.second[1];
        if (first == second) continue;
        const double length = (particles[first].position - particles[second].position).norm();
        constraints.push_back({
            first, second, length * restLengthScale,
            settings.bendCompliance, false});
    }
}

void RubberShell::Step(double dt, double pressure)
{
    if (!std::isfinite(dt) || dt <= 0.0) return;
    if (!std::isfinite(pressure)) throw std::invalid_argument("Rubber shell pressure is not finite");

    // 压力先累积到速度里，再统一预测；这样约束求解只处理位置误差，速度在最后从位移恢复。
    if (pressure != 0.0)
    {
        for (const ShellTriangle& triangle : triangles)
        {
            ShellParticle& first = particles[triangle.first];
            ShellParticle& second = particles[triangle.second];
            ShellParticle& third = particles[triangle.third];
            const Eigen::Vector3d normal = (second.position - first.position).cross(
                third.position - first.position);
            const double area = normal.norm() * 0.5;
            if (area <= 1e-12) continue;
            const Eigen::Vector3d direction = normal / normal.norm();
            const Eigen::Vector3d acceleration = direction * (pressure * area / 3.0);
            if (!first.pinned) first.velocity += acceleration * first.inverseMass * dt;
            if (!second.pinned) second.velocity += acceleration * second.inverseMass * dt;
            if (!third.pinned) third.velocity += acceleration * third.inverseMass * dt;
        }
    }

    for (ShellParticle& particle : particles)
    {
        if (particle.pinned)
        {
            particle.velocity.setZero();
            particle.previousPosition = particle.position;
            continue;
        }
        particle.velocity += settings.gravity * dt;
        particle.previousPosition = particle.position;
        particle.position += particle.velocity * dt;
    }

    for (int iteration = 0; iteration < settings.constraintIterations; ++iteration)
        for (const ShellDistanceConstraint& constraint : constraints)
            SolveDistanceConstraint(
                particles[constraint.first],
                particles[constraint.second],
                constraint,
                dt,
                settings.strainStiffening);

    const double dampingFactor = 1.0 / (1.0 + settings.damping * dt);
    for (ShellParticle& particle : particles)
    {
        if (particle.pinned)
        {
            particle.velocity.setZero();
            continue;
        }
        particle.velocity = (particle.position - particle.previousPosition) / dt;
        particle.velocity *= dampingFactor;
    }

    // 粘弹阻尼只作用在结构边的相对速度上，保留刚体平移/整体旋转，
    // 同时让橡皮膜在拉伸/回弹时比纯弹性更快损失能量。
    if (settings.edgeDamping > 0.0)
    {
        const double dampingAmount = 1.0 - std::exp(-settings.edgeDamping * dt);
        for (const ShellDistanceConstraint& constraint : constraints)
        {
            if (!constraint.structural) continue;
            ShellParticle& first = particles[constraint.first];
            ShellParticle& second = particles[constraint.second];
            const double weightSum = first.inverseMass + second.inverseMass;
            if (weightSum <= 0.0) continue;
            const Eigen::Vector3d delta = second.position - first.position;
            const double length = delta.norm();
            if (length <= 1e-12) continue;
            const Eigen::Vector3d direction = delta / length;
            const double relativeVelocity =
                (second.velocity - first.velocity).dot(direction);
            const double impulse = dampingAmount * relativeVelocity / weightSum;
            if (!first.pinned) first.velocity += first.inverseMass * impulse * direction;
            if (!second.pinned) second.velocity -= second.inverseMass * impulse * direction;
        }
    }
}

double RubberShell::ComputeVolume() const
{
    double volume = 0.0;
    for (const ShellTriangle& triangle : triangles)
        volume += TriangleSignedVolume(
            particles[triangle.first].position,
            particles[triangle.second].position,
            particles[triangle.third].position);
    return volume;
}

SoftBodyRenderMesh RubberShell::BuildRenderMesh() const
{
    SoftBodyRenderMesh mesh;
    mesh.positions.reserve(particles.size());
    mesh.normals.reserve(particles.size());
    for (const ShellParticle& particle : particles)
    {
        mesh.positions.push_back(particle.position.cast<float>());
        mesh.normals.push_back(Eigen::Vector3f::Zero());
    }
    mesh.indices.reserve(triangles.size() * 3);
    const double holeCosine = std::cos(renderHoleAngle);
    for (const ShellTriangle& triangle : triangles)
    {
        const Eigen::Vector3d first = particles[triangle.first].position;
        const Eigen::Vector3d second = particles[triangle.second].position;
        const Eigen::Vector3d third = particles[triangle.third].position;
        if (renderHoleAngle > 0.0)
        {
            // 破口内的三角面不进入渲染索引；物理壳本身仍然闭合。
            const Eigen::Vector3d centroid = (first + second + third) / 3.0;
            if (centroid.norm() > 1e-9 &&
                centroid.normalized().dot(renderHoleDirection) > holeCosine)
                continue;
        }
        mesh.indices.push_back(triangle.first);
        mesh.indices.push_back(triangle.second);
        mesh.indices.push_back(triangle.third);
        const Eigen::Vector3d normal = (second - first).cross(third - first);
        if (normal.norm() <= 1e-15) continue;
        const Eigen::Vector3f accumulated = normal.cast<float>();
        mesh.normals[triangle.first] += accumulated;
        mesh.normals[triangle.second] += accumulated;
        mesh.normals[triangle.third] += accumulated;
    }
    for (size_t index = 0; index < mesh.normals.size(); ++index)
    {
        const float length = mesh.normals[index].norm();
        if (length > 1e-8f)
        {
            mesh.normals[index] /= length;
            continue;
        }
        // 退化三角形附近没有稳定面法线；用径向方向兜底，保持输出有限且可渲染。
        const Eigen::Vector3f radial = mesh.positions[index];
        mesh.normals[index] = radial.norm() > 1e-8f ? radial.normalized() : Eigen::Vector3f::UnitZ();
    }
    return mesh;
}

void RubberShell::ApplyGroundPlane(float localGroundY, float restitution)
{
    restitution = std::max(0.0f, std::min(1.0f, restitution));
    for (ShellParticle& particle : particles)
    {
        if (particle.position.y() >= localGroundY) continue;
        particle.position.y() = localGroundY;
        if (particle.velocity.y() < 0.0) particle.velocity.y() = -particle.velocity.y() * restitution;
        // 接触面切向速度只做轻微耗散，保留软片在地面上的滑移/翻卷。
        particle.velocity.x() *= 0.92;
        particle.velocity.z() *= 0.92;
    }
}

void RubberShell::SetDamping(double value)
{
    settings.damping = std::max(0.0, value);
}

void RubberShell::ApplyBurstImpulse(const Eigen::Vector3d& center,
    double radialSpeed, double tangentialSpeed, uint64_t seed,
    const Eigen::Vector3d& punctureDirection, double punctureBoost)
{
    radialSpeed = std::max(0.0, radialSpeed);
    tangentialSpeed = std::max(0.0, tangentialSpeed);
    // 破口增强只放大离命中点最近的膜片：cos² 权重让影响局限在半球内，
    // 避免把整个气球都当成“从破口吹出去”。
    Eigen::Vector3d puncture = punctureDirection;
    double boost = punctureBoost;
    if (boost <= 0.0 || puncture.norm() <= 1e-9)
    {
        boost = 0.0;
    }
    else
    {
        puncture.normalize();
    }
    BurstRng random{seed};
    for (ShellParticle& particle : particles)
    {
        const Eigen::Vector3d delta = particle.position - center;
        const double length = delta.norm();
        if (length <= 1e-12) continue;
        const Eigen::Vector3d normal = delta / length;
        const Eigen::Vector3d tangent = normal.unitOrthogonal();
        const Eigen::Vector3d bitangent = normal.cross(tangent).normalized();
        const double angle = random.Uniform() * 6.283185307179586;
        const double magnitude = tangentialSpeed *
            (0.25 + 0.75 * random.Uniform());
        const double facing = boost > 0.0
            ? std::max(0.0, normal.dot(puncture))
            : 0.0;
        particle.velocity += normal * radialSpeed * (1.0 + boost * facing * facing);
        particle.velocity +=
            (std::cos(angle) * tangent + std::sin(angle) * bitangent) * magnitude;
    }
}

void RubberShell::ApplyDeflateImpulse(const Eigen::Vector3d& center,
    double inwardSpeed, double tangentialSpeed, uint64_t seed)
{
    inwardSpeed = std::max(0.0, inwardSpeed);
    tangentialSpeed = std::max(0.0, tangentialSpeed);
    BurstRng random{seed};
    for (ShellParticle& particle : particles)
    {
        if (particle.pinned) continue;
        const Eigen::Vector3d delta = particle.position - center;
        const double length = delta.norm();
        if (length <= 1e-12) continue;
        const Eigen::Vector3d normal = delta / length;
        const Eigen::Vector3d tangent = normal.unitOrthogonal();
        const Eigen::Vector3d bitangent = normal.cross(tangent).normalized();
        const double angle = random.Uniform() * 6.283185307179586;
        const double magnitude =
            tangentialSpeed * (0.25 + 0.75 * random.Uniform());
        // 漏气时壳向内塌陷，速度在后续 XPBD 步里被阻尼和碰撞吃掉。
        particle.velocity -= normal * inwardSpeed *
            (0.55 + 0.45 * random.Uniform());
        particle.velocity +=
            (std::cos(angle) * tangent + std::sin(angle) * bitangent) * magnitude;
    }
}

void RubberShell::ApplyAerodynamicThrust(const Eigen::Vector3d& direction,
    double acceleration, const Eigen::Vector3d& spinAxis,
    double angularSpeed, double dt)
{
    if (dt <= 0.0 || !std::isfinite(dt)) return;
    Eigen::Vector3d thrust = direction;
    if (thrust.norm() > 1e-9) thrust.normalize();
    else thrust.setZero();
    Eigen::Vector3d axis = spinAxis;
    if (axis.norm() > 1e-9) axis.normalize();
    else axis.setZero();

    // 旋转围绕当前质心；气球漏气时整体翻跟头，而不是每个粒子各自抖动。
    Eigen::Vector3d center = Eigen::Vector3d::Zero();
    int moving = 0;
    for (const ShellParticle& particle : particles)
    {
        if (particle.pinned) continue;
        center += particle.position;
        ++moving;
    }
    if (moving > 0) center /= static_cast<double>(moving);

    for (ShellParticle& particle : particles)
    {
        if (particle.pinned) continue;
        particle.velocity += thrust * acceleration * dt;
        if (axis.squaredNorm() > 0.0)
            particle.velocity += axis.cross(particle.position - center) *
                angularSpeed * dt;
    }
}

void RubberShell::SetRenderHole(
    const Eigen::Vector3d& direction, double angularRadius)
{
    if (direction.norm() <= 1e-9 || !(angularRadius > 0.0))
    {
        renderHoleAngle = 0.0;
        return;
    }
    renderHoleDirection = direction.normalized();
    // 上限约 69°：再大就不再像破口，而会把半个气球删掉。
    renderHoleAngle = std::clamp(angularRadius, 0.0, 1.2);
}

void RubberShell::ApplyRigidTransform(const Eigen::Matrix4f& transform)
{
    const Eigen::Matrix4d matrix = transform.cast<double>();
    const Eigen::Matrix3d linear = matrix.block<3, 3>(0, 0);
    for (ShellParticle& particle : particles)
    {
        particle.position = (matrix * particle.position.homogeneous()).head<3>();
        particle.previousPosition =
            (matrix * particle.previousPosition.homogeneous()).head<3>();
        particle.velocity = linear * particle.velocity;
    }
}

void RubberShell::PinNearest(const Eigen::Vector3d& point, int count)
{
    count = std::max(0, std::min(count, static_cast<int>(particles.size())));
    std::vector<uint32_t> indices(particles.size());
    for (uint32_t index = 0; index < indices.size(); ++index) indices[index] = index;
    std::sort(indices.begin(), indices.end(),
        [&](uint32_t first, uint32_t second)
        {
            const double firstDistance =
                (particles[first].position - point).squaredNorm();
            const double secondDistance =
                (particles[second].position - point).squaredNorm();
            return firstDistance < secondDistance ||
                (firstDistance == secondDistance && first < second);
        });
    for (int index = 0; index < count; ++index)
    {
        ShellParticle& particle = particles[indices[static_cast<size_t>(index)]];
        particle.pinned = true;
        particle.velocity.setZero();
    }
}

double RubberShell::MaximumStructuralStrain() const
{
    double maximum = 0.0;
    for (const ShellDistanceConstraint& constraint : constraints)
    {
        if (!constraint.structural || constraint.restLength <= 1e-12) continue;
        const double length = (particles[constraint.first].position -
            particles[constraint.second].position).norm();
        maximum = std::max(maximum, std::abs(length - constraint.restLength) /
            constraint.restLength);
    }
    return maximum;
}

bool RubberShell::ShouldBurst() const
{
    return MaximumStructuralStrain() >= settings.burstStrain;
}

} // namespace VL::Physics::Softbody

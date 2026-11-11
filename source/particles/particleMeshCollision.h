#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "particles/particleSdf.h"

namespace VL
{

// 三角网格碰撞在加载期一次性烘成有限体素 SDF，运行时与 collision.type=sdf
// 走同一条 GPU/CPU 路径，不在模拟循环里做三角形求交。
// 上限是防御性预算：16384 顶点 / 4096 三角形 / 每轴 48 体素，
// 且体素数 × 三角形数不得超过 64M。
constexpr size_t kMaxParticleCollisionMeshVertices = 16384;
constexpr size_t kMaxParticleCollisionMeshTriangles = 4096;
// 体素 × 三角形的最坏工作量上限（约 64M 次最近点/奇偶测试）。
// 默认 24^3 + 4096 三角形仍在预算内；48^3 的高分辨率网格必须减面。
constexpr uint64_t kMaxParticleCollisionMeshSdfWork =
    64ull * 1024ull * 1024ull;
constexpr int kMinParticleCollisionMeshResolution = 2;
constexpr int kMaxParticleCollisionMeshResolution = 48;
constexpr float kDefaultParticleCollisionMeshPadding = 5.0f;

struct ParticleCollisionMeshAsset
{
    std::vector<Eigen::Vector3f> vertices;
    std::vector<uint32_t> indices;
    Eigen::Vector3i resolution = Eigen::Vector3i(24, 24, 24);
    float padding = kDefaultParticleCollisionMeshPadding;
};

namespace ParticleMeshCollisionDetail
{

struct Triangle
{
    Eigen::Vector3d a;
    Eigen::Vector3d b;
    Eigen::Vector3d c;
    Eigen::Vector3d ab;
    Eigen::Vector3d ac;
};

// Ericson《Real-Time Collision Detection》5.1.5：返回三角形上离 p 最近的点。
inline Eigen::Vector3d ClosestPointOnTriangle(
    const Triangle& triangle,
    const Eigen::Vector3d& p)
{
    const Eigen::Vector3d& a = triangle.a;
    const Eigen::Vector3d& b = triangle.b;
    const Eigen::Vector3d& c = triangle.c;
    const Eigen::Vector3d& ab = triangle.ab;
    const Eigen::Vector3d& ac = triangle.ac;
    const Eigen::Vector3d ap = p - a;
    const double d1 = ab.dot(ap);
    const double d2 = ac.dot(ap);
    if (d1 <= 0.0 && d2 <= 0.0)
    {
        return a;
    }

    const Eigen::Vector3d bp = p - b;
    const double d3 = ab.dot(bp);
    const double d4 = ac.dot(bp);
    if (d3 >= 0.0 && d4 <= d3)
    {
        return b;
    }

    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        const double v = d1 / (d1 - d3);
        return a + ab * v;
    }

    const Eigen::Vector3d cp = p - c;
    const double d5 = ab.dot(cp);
    const double d6 = ac.dot(cp);
    if (d6 >= 0.0 && d5 <= d6)
    {
        return c;
    }

    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        const double w = d2 / (d2 - d6);
        return a + ac * w;
    }

    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w;
    }

    const double denominator = va + vb + vc;
    if (!std::isfinite(denominator) ||
        std::abs(denominator) <= 1.0e-18)
    {
        // 退化三角形按最近顶点处理，避免除零产生 NaN。
        const double da = (p - a).squaredNorm();
        const double db = (p - b).squaredNorm();
        const double dc = (p - c).squaredNorm();
        if (da <= db && da <= dc)
        {
            return a;
        }
        return db <= dc ? b : c;
    }
    const double v = vb / denominator;
    const double w = vc / denominator;
    return a + ab * v + ac * w;
}

// 固定非轴对齐射线 + 去重交点计数。闭合网格的奇偶性即内外判定；
// 固定方向保证同一位置每次烘出的符号完全一致。
inline bool PointInsideClosedTriangleMesh(
    const std::vector<Triangle>& triangles,
    const Eigen::Vector3d& point)
{
    const Eigen::Vector3d direction =
        Eigen::Vector3d(1.0, 0.4142135623730950, 0.3183098861837907)
            .normalized();
    std::vector<double> intersections;
    intersections.reserve(8);
    for (const Triangle& triangle : triangles)
    {
        const Eigen::Vector3d edge1 =
            triangle.b - triangle.a;
        const Eigen::Vector3d edge2 =
            triangle.c - triangle.a;
        const Eigen::Vector3d h = direction.cross(edge2);
        const double determinant = edge1.dot(h);
        // 射线与三角形平面平行：没有唯一交点。
        if (!std::isfinite(determinant) ||
            std::abs(determinant) <= 1.0e-12)
        {
            continue;
        }
        const double inverse = 1.0 / determinant;
        const Eigen::Vector3d s = point - triangle.a;
        const double u = inverse * s.dot(h);
        if (u < -1.0e-7 || u > 1.0 + 1.0e-7)
        {
            continue;
        }
        const Eigen::Vector3d q = s.cross(edge1);
        const double v = inverse * direction.dot(q);
        if (v < -1.0e-7 || u + v > 1.0 + 1.0e-7)
        {
            continue;
        }
        const double t = inverse * edge2.dot(q);
        if (!std::isfinite(t) || t <= 1.0e-7)
        {
            continue;
        }
        // 射线穿过共享边时会同时命中两个三角形，按 t 去重只计一次。
        bool duplicate = false;
        for (const double existing : intersections)
        {
            if (std::abs(existing - t) <= 1.0e-6)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            intersections.push_back(t);
        }
    }
    return (intersections.size() % 2u) == 1u;
}

} // namespace ParticleMeshCollisionDetail

inline ParticleCollisionMeshAsset ParseParticleCollisionMeshJson(
    const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("type") ||
        !json.at("type").is_string() ||
        json.at("type").get<std::string>() != "particleCollisionMesh")
    {
        throw std::invalid_argument(
            "Particle collision mesh asset requires "
            "type=particleCollisionMesh");
    }

    ParticleCollisionMeshAsset mesh;
    if (!json.contains("vertices") || !json.at("vertices").is_array() ||
        json.at("vertices").empty())
    {
        throw std::invalid_argument(
            "Particle collision mesh requires a non-empty vertices array");
    }
    const nlohmann::json& vertices = json.at("vertices");
    if (vertices.size() > kMaxParticleCollisionMeshVertices)
    {
        throw std::invalid_argument(
            "Particle collision mesh supports at most " +
            std::to_string(kMaxParticleCollisionMeshVertices) +
            " vertices");
    }
    mesh.vertices.resize(vertices.size());
    for (size_t index = 0; index < vertices.size(); ++index)
    {
        const nlohmann::json& vertex = vertices.at(index);
        if (!vertex.is_array() || vertex.size() != 3)
        {
            throw std::invalid_argument(
                "Particle collision mesh vertex must be [x,y,z]: " +
                std::to_string(index));
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            const nlohmann::json& component = vertex.at(axis);
            if (!component.is_number() ||
                !std::isfinite(component.get<float>()))
            {
                throw std::invalid_argument(
                    "Particle collision mesh vertex must be finite: " +
                    std::to_string(index));
            }
            mesh.vertices[index][axis] = component.get<float>();
        }
    }

    if (!json.contains("indices") || !json.at("indices").is_array() ||
        json.at("indices").empty())
    {
        throw std::invalid_argument(
            "Particle collision mesh requires a non-empty indices array");
    }
    const nlohmann::json& indices = json.at("indices");
    if (indices.size() % 3 != 0)
    {
        throw std::invalid_argument(
            "Particle collision mesh indices must contain complete triangles");
    }
    const size_t triangleCount = indices.size() / 3;
    if (triangleCount > kMaxParticleCollisionMeshTriangles)
    {
        throw std::invalid_argument(
            "Particle collision mesh supports at most " +
            std::to_string(kMaxParticleCollisionMeshTriangles) +
            " triangles");
    }
    mesh.indices.resize(indices.size());
    for (size_t index = 0; index < indices.size(); ++index)
    {
        const nlohmann::json& value = indices.at(index);
        if (!value.is_number_integer() && !value.is_number_unsigned())
        {
            throw std::invalid_argument(
                "Particle collision mesh index must be an integer: " +
                std::to_string(index));
        }
        uint64_t integer = 0;
        if (value.is_number_unsigned())
        {
            integer = value.get<uint64_t>();
        }
        else
        {
            const int64_t signedValue = value.get<int64_t>();
            if (signedValue < 0)
            {
                throw std::invalid_argument(
                    "Particle collision mesh index is out of range: " +
                    std::to_string(index));
            }
            integer = static_cast<uint64_t>(signedValue);
        }
        if (integer >= mesh.vertices.size())
        {
            throw std::invalid_argument(
                "Particle collision mesh index is out of range: " +
                std::to_string(index));
        }
        mesh.indices[index] = static_cast<uint32_t>(integer);
    }

    if (json.contains("resolution"))
    {
        const nlohmann::json& resolution = json.at("resolution");
        if (!resolution.is_array() || resolution.size() != 3)
        {
            throw std::invalid_argument(
                "Particle collision mesh requires resolution [x,y,z]");
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            const nlohmann::json& value = resolution.at(axis);
            if (!value.is_number_integer() &&
                !value.is_number_unsigned())
            {
                throw std::invalid_argument(
                    "Particle collision mesh resolution must be integers");
            }
            int64_t integer = 0;
            if (value.is_number_unsigned())
            {
                const uint64_t unsignedValue = value.get<uint64_t>();
                if (unsignedValue >
                    static_cast<uint64_t>(kMaxParticleCollisionMeshResolution))
                {
                    throw std::invalid_argument(
                        "Particle collision mesh resolution must be "
                        "in [2, 48]");
                }
                integer = static_cast<int64_t>(unsignedValue);
            }
            else
            {
                integer = value.get<int64_t>();
            }
            if (integer < kMinParticleCollisionMeshResolution ||
                integer > kMaxParticleCollisionMeshResolution)
            {
                throw std::invalid_argument(
                    "Particle collision mesh resolution must be in [2, 48]");
            }
            mesh.resolution[axis] = static_cast<int>(integer);
        }
    }

    if (json.contains("padding"))
    {
        const nlohmann::json& padding = json.at("padding");
        if (!padding.is_number() ||
            !std::isfinite(padding.get<float>()))
        {
            throw std::invalid_argument(
                "Particle collision mesh padding must be finite");
        }
        mesh.padding = padding.get<float>();
        if (mesh.padding < 0.0f)
        {
            throw std::invalid_argument(
                "Particle collision mesh padding must be non-negative");
        }
    }
    return mesh;
}

inline ParticleCollisionMeshAsset ReadParticleCollisionMeshFile(
    const std::filesystem::path& resolvedPath)
{
    std::ifstream file(resolvedPath);
    if (!file.is_open())
    {
        throw std::runtime_error(
            "Failed to open particle collision mesh asset: " +
            resolvedPath.string());
    }
    nlohmann::json json;
    try
    {
        file >> json;
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to parse particle collision mesh asset: " +
            resolvedPath.string() + ": " + exception.what());
    }
    return ParseParticleCollisionMeshJson(json);
}

// 体素中心取最近三角形的无符号距离；闭合网格的奇偶性决定符号。
// 负值在实体内、正值在实体外，与 particleSdf 的约定一致。
inline ParticleSdfAsset BuildParticleSdfFromCollisionMesh(
    const ParticleCollisionMeshAsset& mesh)
{
    if (mesh.vertices.empty() || mesh.indices.empty() ||
        mesh.indices.size() % 3 != 0)
    {
        throw std::invalid_argument(
            "Particle collision mesh requires vertices and complete triangles");
    }
    if (mesh.indices.size() / 3 > kMaxParticleCollisionMeshTriangles)
    {
        throw std::invalid_argument(
            "Particle collision mesh triangle budget exceeded");
    }
    if (mesh.vertices.size() > kMaxParticleCollisionMeshVertices)
    {
        throw std::invalid_argument(
            "Particle collision mesh vertex budget exceeded");
    }
    if (!std::isfinite(mesh.padding) || mesh.padding < 0.0f)
    {
        throw std::invalid_argument(
            "Particle collision mesh padding must be finite and non-negative");
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (mesh.resolution[axis] < kMinParticleCollisionMeshResolution ||
            mesh.resolution[axis] > kMaxParticleCollisionMeshResolution)
        {
            throw std::invalid_argument(
                "Particle collision mesh resolution must be in [2, 48]");
        }
    }

    Eigen::Vector3d boundsMin =
        Eigen::Vector3d::Constant(std::numeric_limits<double>::infinity());
    Eigen::Vector3d boundsMax =
        Eigen::Vector3d::Constant(-std::numeric_limits<double>::infinity());
    for (const Eigen::Vector3f& vertex : mesh.vertices)
    {
        if (!vertex.allFinite())
        {
            throw std::invalid_argument(
                "Particle collision mesh vertex must be finite");
        }
        const Eigen::Vector3d value = vertex.cast<double>();
        boundsMin = boundsMin.cwiseMin(value);
        boundsMax = boundsMax.cwiseMax(value);
    }
    boundsMin.array() -= static_cast<double>(mesh.padding);
    boundsMax.array() += static_cast<double>(mesh.padding);
    // padding=0 时平面/退化网格会让体素空间塌缩；直接拒绝而不是产生
    // 除零或静默扩大 bounds。
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!(boundsMax[axis] > boundsMin[axis]) ||
            boundsMax[axis] - boundsMin[axis] <= 1.0e-5)
        {
            throw std::invalid_argument(
                "Particle collision mesh bounds are degenerate");
        }
    }

    std::vector<ParticleMeshCollisionDetail::Triangle> triangles;
    triangles.reserve(mesh.indices.size() / 3);
    std::vector<std::vector<size_t>> vertexTriangles(
        mesh.vertices.size());
    const double meshDiagonal = (boundsMax - boundsMin).norm();
    const double minimumTriangleArea = std::max(
        1.0e-12,
        meshDiagonal * meshDiagonal * 1.0e-12);
    // pair.first 是方向余额，pair.second 是出现次数。闭合流形的每条
    // 无向边必须恰好出现两次，且两次方向相反。
    std::map<
        std::pair<uint32_t, uint32_t>,
        std::pair<int, int>> edges;
    auto recordEdge = [&edges](uint32_t from, uint32_t to)
    {
        if (from == to)
        {
            throw std::invalid_argument(
                "Particle collision mesh contains a degenerate edge");
        }
        const uint32_t minimum = std::min(from, to);
        const uint32_t maximum = std::max(from, to);
        auto& edge = edges[{minimum, maximum}];
        edge.first += from < to ? 1 : -1;
        ++edge.second;
    };
    for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
    {
        const uint32_t i0 = mesh.indices[index];
        const uint32_t i1 = mesh.indices[index + 1];
        const uint32_t i2 = mesh.indices[index + 2];
        if (i0 >= mesh.vertices.size() ||
            i1 >= mesh.vertices.size() ||
            i2 >= mesh.vertices.size())
        {
            throw std::invalid_argument(
                "Particle collision mesh index is out of range");
        }
        ParticleMeshCollisionDetail::Triangle triangle;
        triangle.a = mesh.vertices[i0].cast<double>();
        triangle.b = mesh.vertices[i1].cast<double>();
        triangle.c = mesh.vertices[i2].cast<double>();
        triangle.ab = triangle.b - triangle.a;
        triangle.ac = triangle.c - triangle.a;
        const double twiceArea =
            triangle.ab.cross(triangle.ac).norm();
        if (!std::isfinite(twiceArea) ||
            twiceArea * 0.5 <= minimumTriangleArea)
        {
            throw std::invalid_argument(
                "Particle collision mesh contains a zero-area triangle");
        }
        recordEdge(i0, i1);
        recordEdge(i1, i2);
        recordEdge(i2, i0);
        vertexTriangles[i0].push_back(triangles.size());
        vertexTriangles[i1].push_back(triangles.size());
        vertexTriangles[i2].push_back(triangles.size());
        triangles.push_back(triangle);
    }
    for (const auto& [edge, usage] : edges)
    {
        (void)edge;
        if (usage.second != 2 || usage.first != 0)
        {
            throw std::invalid_argument(
                "Particle collision mesh must be a closed manifold mesh "
                "with consistent triangle winding");
        }
    }
    // 每条边已经满足 2-manifold；顶点处还必须是单个 link 环，避免两个
    // 闭合组件只共享一个顶点（edge-manifold 但 vertex-non-manifold）。
    for (size_t vertex = 0; vertex < vertexTriangles.size(); ++vertex)
    {
        const std::vector<size_t>& incident = vertexTriangles[vertex];
        if (incident.empty())
        {
            continue;
        }
        std::map<uint32_t, std::set<uint32_t>> link;
        for (size_t triangleIndex : incident)
        {
            const uint32_t indices[3] = {
                mesh.indices[triangleIndex * 3],
                mesh.indices[triangleIndex * 3 + 1],
                mesh.indices[triangleIndex * 3 + 2]};
            uint32_t other[2] = {0u, 0u};
            int otherCount = 0;
            for (const uint32_t index : indices)
            {
                if (index != vertex)
                {
                    other[otherCount++] = index;
                }
            }
            if (otherCount != 2)
            {
                throw std::invalid_argument(
                    "Particle collision mesh has a non-manifold vertex");
            }
            link[other[0]].insert(other[1]);
            link[other[1]].insert(other[0]);
        }
        for (const auto& [neighbor, adjacent] : link)
        {
            (void)neighbor;
            if (adjacent.size() != 2)
            {
                throw std::invalid_argument(
                    "Particle collision mesh has a non-manifold vertex");
            }
        }
        std::set<uint32_t> visited;
        std::vector<uint32_t> pending;
        pending.push_back(link.begin()->first);
        while (!pending.empty())
        {
            const uint32_t current = pending.back();
            pending.pop_back();
            if (!visited.insert(current).second)
            {
                continue;
            }
            for (const uint32_t neighbor : link.at(current))
            {
                if (visited.find(neighbor) == visited.end())
                {
                    pending.push_back(neighbor);
                }
            }
        }
        if (visited.size() != link.size())
        {
            throw std::invalid_argument(
                "Particle collision mesh has a non-manifold vertex");
        }
    }

    ParticleSdfAsset sdf;
    sdf.resolution = mesh.resolution;
    sdf.boundsMin = boundsMin.cast<float>();
    sdf.boundsMax = boundsMax.cast<float>();
    sdf.sourcePadding = mesh.padding;
    const size_t voxelCount =
        static_cast<size_t>(sdf.resolution.x()) *
        static_cast<size_t>(sdf.resolution.y()) *
        static_cast<size_t>(sdf.resolution.z());
    const uint64_t work =
        static_cast<uint64_t>(voxelCount) *
        static_cast<uint64_t>(triangles.size());
    if (work > kMaxParticleCollisionMeshSdfWork)
    {
        throw std::invalid_argument(
            "Particle collision mesh voxel/triangle work budget exceeded");
    }
    sdf.distances.resize(voxelCount);
    const Eigen::Vector3d cellSize =
        (boundsMax - boundsMin).array() /
        (sdf.resolution.array() - 1).cast<double>();
    for (int z = 0; z < sdf.resolution.z(); ++z)
    {
        for (int y = 0; y < sdf.resolution.y(); ++y)
        {
            for (int x = 0; x < sdf.resolution.x(); ++x)
            {
                const Eigen::Vector3d point(
                    boundsMin.x() + cellSize.x() * static_cast<double>(x),
                    boundsMin.y() + cellSize.y() * static_cast<double>(y),
                    boundsMin.z() + cellSize.z() * static_cast<double>(z));
                double squaredDistance =
                    std::numeric_limits<double>::infinity();
                for (const auto& triangle : triangles)
                {
                    const Eigen::Vector3d closest =
                        ParticleMeshCollisionDetail::ClosestPointOnTriangle(
                            triangle, point);
                    squaredDistance = std::min(
                        squaredDistance,
                        (point - closest).squaredNorm());
                }
                const double distance = std::sqrt(squaredDistance);
                const bool inside =
                    ParticleMeshCollisionDetail::PointInsideClosedTriangleMesh(
                        triangles, point);
                const int voxel =
                    x +
                    sdf.resolution.x() * (y + sdf.resolution.y() * z);
                sdf.distances[static_cast<size_t>(voxel)] =
                    static_cast<float>(inside ? -distance : distance);
            }
        }
    }
    return sdf;
}

} // namespace VL

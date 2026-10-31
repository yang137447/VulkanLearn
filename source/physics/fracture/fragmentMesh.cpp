#include "physics/fracture/fragmentMesh.h"

#include <cmath>
#include <stdexcept>

#include <Eigen/Geometry>

namespace VL::Physics::Fracture
{
namespace
{

void AppendVertex(
    FragmentMesh& mesh,
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& normal)
{
    mesh.positions.push_back(position);
    mesh.normals.push_back(normal);
}

void AppendTriangle(FragmentMesh& mesh, uint32_t first, uint32_t second, uint32_t third)
{
    mesh.indices.push_back(first);
    mesh.indices.push_back(second);
    mesh.indices.push_back(third);
}

} // namespace

FragmentMesh ExtrudePlanarShard(
    const PlanarShard& shard,
    const ExtrusionSettings& settings)
{
    if (shard.outline.size() < 3 || shard.area <= 1e-12)
        throw std::invalid_argument("Cannot extrude an empty fracture shard");
    if (!std::isfinite(settings.thickness) || settings.thickness <= 0.0)
        throw std::invalid_argument("Fracture extrusion thickness must be positive");

    FragmentMesh mesh;
    const size_t count = shard.outline.size();
    const float halfThickness = static_cast<float>(settings.thickness * 0.5);
    const float centerZ = static_cast<float>(settings.centerZ);

    // 面积质心作为局部原点；这样 Jolt 刚体的质心和渲染模型矩阵使用同一局部锚点。
    mesh.localCenter = Eigen::Vector3f(
        static_cast<float>(shard.centroid.x()),
        static_cast<float>(shard.centroid.y()),
        centerZ);

    const uint32_t topStart = static_cast<uint32_t>(mesh.positions.size());
    for (const Point2& point : shard.outline)
    {
        AppendVertex(mesh,
            Eigen::Vector3f(
                static_cast<float>(point.x() - shard.centroid.x()),
                static_cast<float>(point.y() - shard.centroid.y()),
                halfThickness),
            Eigen::Vector3f(0.0f, 0.0f, 1.0f));
        mesh.convexHullPoints.push_back(mesh.positions.back());
    }

    const uint32_t bottomStart = static_cast<uint32_t>(mesh.positions.size());
    for (const Point2& point : shard.outline)
    {
        AppendVertex(mesh,
            Eigen::Vector3f(
                static_cast<float>(point.x() - shard.centroid.x()),
                static_cast<float>(point.y() - shard.centroid.y()),
                -halfThickness),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));
        mesh.convexHullPoints.push_back(mesh.positions.back());
    }

    // 凸多边形用扇形三角化即可；顶面 CCW，底面反向。
    for (uint32_t index = 1; index + 1 < static_cast<uint32_t>(count); ++index)
        AppendTriangle(mesh, topStart, topStart + index, topStart + index + 1);
    for (uint32_t index = 1; index + 1 < static_cast<uint32_t>(count); ++index)
        AppendTriangle(mesh, bottomStart, bottomStart + index + 1, bottomStart + index);

    // 侧面每边四个顶点、两个三角形。CCW 轮廓的右侧就是外法线，绕序因此为
    // bottom_i -> bottom_next -> top_next -> top_i。
    for (uint32_t index = 0; index < static_cast<uint32_t>(count); ++index)
    {
        const uint32_t next = (index + 1) % static_cast<uint32_t>(count);
        const Point2 edge = shard.outline[next] - shard.outline[index];
        const Eigen::Vector3f normal = Eigen::Vector3f(
            static_cast<float>(edge.y()),
            static_cast<float>(-edge.x()),
            0.0f).normalized();
        // 先把要复用的四个角拷出来；AppendVertex 会增长 positions，不能继续
        // 持有同一 vector 内元素的引用传给 push_back。
        const Eigen::Vector3f bottomCurrent = mesh.positions[bottomStart + index];
        const Eigen::Vector3f bottomNext = mesh.positions[bottomStart + next];
        const Eigen::Vector3f topNext = mesh.positions[topStart + next];
        const Eigen::Vector3f topCurrent = mesh.positions[topStart + index];
        const uint32_t sideStart = static_cast<uint32_t>(mesh.positions.size());
        AppendVertex(mesh, bottomCurrent, normal);
        AppendVertex(mesh, bottomNext, normal);
        AppendVertex(mesh, topNext, normal);
        AppendVertex(mesh, topCurrent, normal);
        AppendTriangle(mesh, sideStart, sideStart + 1, sideStart + 2);
        AppendTriangle(mesh, sideStart, sideStart + 2, sideStart + 3);
    }

    mesh.volume = shard.area * settings.thickness;
    return mesh;
}

double SignedMeshVolume(
    const std::vector<Eigen::Vector3f>& positions,
    const std::vector<uint32_t>& indices)
{
    if (positions.empty() || indices.size() % 3 != 0)
        throw std::invalid_argument("SignedMeshVolume requires a triangle list");
    double volume = 0.0;
    for (size_t index = 0; index + 2 < indices.size(); index += 3)
    {
        if (indices[index] >= positions.size() ||
            indices[index + 1] >= positions.size() ||
            indices[index + 2] >= positions.size())
            throw std::out_of_range("SignedMeshVolume index is outside the vertex list");
        const Eigen::Vector3d first = positions[indices[index]].cast<double>();
        const Eigen::Vector3d second = positions[indices[index + 1]].cast<double>();
        const Eigen::Vector3d third = positions[indices[index + 2]].cast<double>();
        volume += first.dot(second.cross(third));
    }
    return volume / 6.0;
}

} // namespace VL::Physics::Fracture

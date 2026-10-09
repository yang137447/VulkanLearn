#include "render/offline/offlineMeshAdapter.h"

#include <cmath>
#include <numeric>
#include <stdexcept>

namespace VL
{
namespace
{
Offline::Vec3 ToCore(const Eigen::Vector3f& value)
{
    return {value.x(), value.y(), value.z()};
}

Offline::Vec2 ToCoreUv(const Eigen::Vector2f& value)
{
    return {value.x(), value.y()};
}

std::array<Offline::Vec3, 3> MatrixColumns(const Eigen::Matrix3f& matrix)
{
    return {ToCore(Eigen::Vector3f(matrix.col(0))),
            ToCore(Eigen::Vector3f(matrix.col(1))),
            ToCore(Eigen::Vector3f(matrix.col(2)))};
}

} // namespace

std::shared_ptr<const Offline::Mesh> BuildOfflineMesh(const MeshSection& section,
    const std::atomic<std::uint64_t>* latestGeneration,
    std::uint64_t expectedGeneration)
{
    if (section.indices.empty() || section.indices.size() % 3 != 0)
        throw std::invalid_argument("Offline mesh section needs triangle indices: " +
                                    section.sectionName);
    auto mesh = std::make_shared<Offline::Mesh>();
    mesh->triangles.reserve(section.indices.size() / 3);
    for (std::size_t index = 0; index < section.indices.size(); index += 3)
    {
        if (latestGeneration && (index % 768 == 0) &&
            latestGeneration->load(std::memory_order_relaxed) != expectedGeneration)
            throw std::runtime_error("Offline mesh prepare canceled");
        Offline::Triangle triangle;
        for (int corner = 0; corner < 3; ++corner)
        {
            const std::uint32_t vertexIndex = section.indices[index + corner];
            if (vertexIndex >= section.vertices.size())
                throw std::invalid_argument("Offline mesh index out of range: " +
                                            section.sectionName);
            const Vertex& vertex = section.vertices[vertexIndex];
            triangle.vertices[corner] = ToCore(vertex.position);
            triangle.normals[corner] = ToCore(vertex.normal);
            triangle.uv[corner] = ToCoreUv(vertex.texCoord);
        }
        const Offline::Vec3 edge1 = triangle.vertices[1] - triangle.vertices[0];
        const Offline::Vec3 edge2 = triangle.vertices[2] - triangle.vertices[0];
        const Offline::Vec2 duv1{triangle.uv[1].x - triangle.uv[0].x,
                                 triangle.uv[1].y - triangle.uv[0].y};
        const Offline::Vec2 duv2{triangle.uv[2].x - triangle.uv[0].x,
                                 triangle.uv[2].y - triangle.uv[0].y};
        const double determinant = duv1.x * duv2.y - duv1.y * duv2.x;
        if (std::abs(determinant) > 1e-12)
        {
            triangle.dpdu = (edge1 * duv2.y - edge2 * duv1.y) / determinant;
            triangle.dpdv = (edge2 * duv1.x - edge1 * duv2.x) / determinant;
        }
        else
        {
            // 退化 UV 不参与纱线方向推导；A/Lambert 仍可使用几何，但
            // B/Satin 的 preflight 必须在入会话前拒绝没有有效 UV 轴的 section。
            mesh->hasValidUvFrames = false;
            triangle.dpdu = edge1;
            triangle.dpdv = edge2;
        }
        mesh->triangles.push_back(triangle);
        Offline::expand(mesh->bounds, Offline::triangleBounds(triangle));
    }
    mesh->order.resize(mesh->triangles.size());
    std::iota(mesh->order.begin(), mesh->order.end(), 0);
    Offline::buildBvh(*mesh, 0, mesh->triangles.size(),
                      latestGeneration, expectedGeneration);
    return mesh;
}

Offline::MeshInstance BuildOfflineInstance(
    const Offline::Mesh& mesh,
    const Eigen::Matrix4f& model,
    Offline::SurfaceModel materialModel)
{
    const Eigen::Matrix3f linear = model.block<3, 3>(0, 0);
    if (!model.allFinite() || std::abs(linear.determinant()) < 1e-10f ||
        model.row(3) != Eigen::RowVector4f(0, 0, 0, 1))
        throw std::invalid_argument("Offline instance requires an invertible affine transform");

    Offline::MeshInstance instance;
    instance.mesh = &mesh;
    instance.localToWorld = MatrixColumns(linear);
    instance.worldToLocal = MatrixColumns(linear.inverse());
    instance.translation = ToCore(Eigen::Vector3f(model.block<3, 1>(0, 3)));
    instance.model = materialModel;
    return instance;
}

} // namespace VL

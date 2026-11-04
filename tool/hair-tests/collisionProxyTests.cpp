#include "hair/collisionProxy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{
using VL::Hair::CollisionProxy;
using VL::Hair::CollisionProxyOptions;
using VL::Hair::SdfTriangleMesh;

// 有向体积。闭合网格按外向绕序时为正，用于断言聚类没有翻转绕序。
double SignedVolume(const SdfTriangleMesh& mesh)
{
    double volume = 0.0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const Eigen::Vector3f& a = mesh.positions[mesh.indices[i]];
        const Eigen::Vector3f& b = mesh.positions[mesh.indices[i + 1]];
        const Eigen::Vector3f& c = mesh.positions[mesh.indices[i + 2]];
        volume += static_cast<double>(a.dot(b.cross(c))) / 6.0;
    }
    return volume;
}

void EnsureOutwardWinding(SdfTriangleMesh& mesh)
{
    if (SignedVolume(mesh) >= 0.0)
        return;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
}

SdfTriangleMesh MakeBox(const Eigen::Vector3f& min, const Eigen::Vector3f& max)
{
    SdfTriangleMesh mesh;
    mesh.positions = {
        {min.x(), min.y(), min.z()}, {max.x(), min.y(), min.z()},
        {max.x(), max.y(), min.z()}, {min.x(), max.y(), min.z()},
        {min.x(), min.y(), max.z()}, {max.x(), min.y(), max.z()},
        {max.x(), max.y(), max.z()}, {min.x(), max.y(), max.z()}};
    const std::array<std::array<std::uint32_t, 4>, 6> quads{{
        {{0, 3, 2, 1}}, {{4, 5, 6, 7}}, {{0, 4, 7, 3}},
        {{1, 2, 6, 5}}, {{0, 1, 5, 4}}, {{3, 7, 6, 2}}}};
    for (const auto& quad : quads)
    {
        mesh.indices.insert(mesh.indices.end(),
            {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]});
    }
    EnsureOutwardWinding(mesh);
    return mesh;
}

// 去掉 z=max 的顶面（三角形 6..11），保留 10 个三角形和 4 条开放边。
SdfTriangleMesh MakeOpenBox(const Eigen::Vector3f& min, const Eigen::Vector3f& max)
{
    SdfTriangleMesh mesh = MakeBox(min, max);
    mesh.indices.erase(mesh.indices.begin() + 6, mesh.indices.begin() + 12);
    return mesh;
}

// 轴对齐立方体表面的规则栅格：每个面 subdivisions×subdivisions 个四边形，
// 共用棱边顶点，因此输入本身是闭合流形。用于验证“格距大于小面”时的合并行为。
SdfTriangleMesh MakeLatticeCube(int subdivisions, float size)
{
    SdfTriangleMesh mesh;
    const int n = subdivisions;
    const float step = size / static_cast<float>(n);
    const int stride = n + 1;
    for (int k = 0; k <= n; ++k)
    {
        for (int j = 0; j <= n; ++j)
        {
            for (int i = 0; i <= n; ++i)
            {
                mesh.positions.push_back(
                    {step * static_cast<float>(i), step * static_cast<float>(j),
                        step * static_cast<float>(k)});
            }
        }
    }
    const auto index = [stride](int i, int j, int k) {
        return static_cast<std::uint32_t>(i + stride * (j + stride * k));
    };

    struct Face
    {
        Eigen::Vector3i origin;
        Eigen::Vector3i u;
        Eigen::Vector3i v;
    };
    // u × v 必须等于该面的外法线；否则闭合体的有向体积会被个别面拉低。
    const std::array<Face, 6> faces{{
        {{n, 0, 0}, {0, 1, 0}, {0, 0, 1}}, // +x
        {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}}, // -x
        {{0, n, 0}, {0, 0, 1}, {1, 0, 0}}, // +y
        {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}, // -y
        {{0, 0, n}, {1, 0, 0}, {0, 1, 0}}, // +z
        {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}}, // -z
    }};
    for (const Face& face : faces)
    {
        for (int b = 0; b < n; ++b)
        {
            for (int a = 0; a < n; ++a)
            {
                const Eigen::Vector3i p = face.origin + a * face.u + b * face.v;
                const Eigen::Vector3i pU = p + face.u;
                const Eigen::Vector3i pUV = p + face.u + face.v;
                const Eigen::Vector3i pV = p + face.v;
                const std::array<std::uint32_t, 4> corners{{
                    index(p.x(), p.y(), p.z()), index(pU.x(), pU.y(), pU.z()),
                    index(pUV.x(), pUV.y(), pUV.z()), index(pV.x(), pV.y(), pV.z())}};
                mesh.indices.insert(mesh.indices.end(),
                    {corners[0], corners[1], corners[2], corners[0], corners[2], corners[3]});
            }
        }
    }
    return mesh;
}

Eigen::Vector3f FaceNormal(const SdfTriangleMesh& mesh, std::size_t triangle)
{
    const Eigen::Vector3f& a = mesh.positions[mesh.indices[triangle * 3 + 0]];
    const Eigen::Vector3f& b = mesh.positions[mesh.indices[triangle * 3 + 1]];
    const Eigen::Vector3f& c = mesh.positions[mesh.indices[triangle * 3 + 2]];
    return (b - a).cross(c - a).normalized();
}

Eigen::Vector3f FaceCentroid(const SdfTriangleMesh& mesh, std::size_t triangle)
{
    return (mesh.positions[mesh.indices[triangle * 3 + 0]] +
        mesh.positions[mesh.indices[triangle * 3 + 1]] +
        mesh.positions[mesh.indices[triangle * 3 + 2]]) /
        3.0f;
}

std::vector<std::string> FixedComment()
{
    return {"vl_build_collision_proxy test fixture", "cellSize 1.0 cm", "region none"};
}
}

TEST(CollisionProxy, CubeClusteringCollapsesToCoarseShellAndPreservesOutwardWinding)
{
    // 2 cm 立方体、每面 4×4 个四边形；1 cm 格距把成对相邻顶点并到同一簇，
    // 每面只剩 2×2 个四边形的粗壳：26 个簇、48 个三角形、闭合流形。
    const SdfTriangleMesh cube = MakeLatticeCube(4, 2.0f);
    CollisionProxyOptions options;
    options.cellSize = 1.0f;
    const auto built = VL::Hair::BuildCollisionProxy(cube, options);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const CollisionProxy& proxy = built.Value();

    EXPECT_EQ(proxy.report.inputVertexCount, 125u);
    EXPECT_EQ(proxy.report.inputTriangleCount, 192u);
    EXPECT_EQ(proxy.report.clusterCount, 26u);
    EXPECT_EQ(proxy.report.outputVertexCount, 26u);
    EXPECT_EQ(proxy.report.outputTriangleCount, 48u);
    EXPECT_EQ(proxy.report.droppedDegenerateTriangleCount, 144u);
    EXPECT_EQ(proxy.report.droppedDuplicateTriangleCount, 0u);
    EXPECT_EQ(proxy.report.openEdgeCount, 0u);
    EXPECT_EQ(proxy.report.nonManifoldEdgeCount, 0u);

    // 粗壳仍是凸的轴对齐盒，所有面法线必须背离盒心；有向体积必须为正。
    const Eigen::Vector3f center =
        (proxy.report.outputBoundsMin + proxy.report.outputBoundsMax) * 0.5f;
    EXPECT_GT(SignedVolume(proxy.mesh), 0.0);
    for (std::size_t triangle = 0; triangle < proxy.mesh.indices.size() / 3; ++triangle)
    {
        EXPECT_GT(FaceNormal(proxy.mesh, triangle).dot(
                      FaceCentroid(proxy.mesh, triangle) - center), 0.0f)
            << "triangle " << triangle << " faces inward";
    }
    // 只聚类被保留三角形引用到的表面顶点：贴在外表面格层里的簇只含同一面上的
    // 顶点，因此 0 和 2 这两个极值坐标被保留，包围盒与输入一致；不会整体收缩。
    EXPECT_NEAR(proxy.report.outputBoundsMin.x(), 0.0f, 1.0e-6f);
    EXPECT_NEAR(proxy.report.outputBoundsMin.y(), 0.0f, 1.0e-6f);
    EXPECT_NEAR(proxy.report.outputBoundsMin.z(), 0.0f, 1.0e-6f);
    EXPECT_NEAR(proxy.report.outputBoundsMax.x(), 2.0f, 1.0e-6f);
    EXPECT_NEAR(proxy.report.outputBoundsMax.y(), 2.0f, 1.0e-6f);
    EXPECT_NEAR(proxy.report.outputBoundsMax.z(), 2.0f, 1.0e-6f);
}

TEST(CollisionProxy, DegenerateAndDuplicateTrianglesAreRemoved)
{
    SdfTriangleMesh mesh;
    mesh.positions = {
        {0.0f, 0.0f, 0.0f}, {3.0f, 0.0f, 0.0f}, {3.0f, 3.0f, 0.0f}, {0.0f, 3.0f, 0.0f},
        // 前两点落在同一格（floor(10/2) == floor(10.5/2) == 5），聚类后折叠。
        {10.0f, 0.0f, 0.0f}, {10.5f, 0.0f, 0.0f}, {12.0f, 1.0f, 0.0f},
        // 三个点共线且各占一格：聚类后面积精确为零。
        {20.0f, 0.0f, 0.0f}, {22.0f, 0.0f, 0.0f}, {24.0f, 0.0f, 0.0f}};
    mesh.indices = {
        0u, 1u, 2u, // 有效四边形
        0u, 2u, 3u,
        0u, 1u, 2u, // 与第一个三角形重复
        4u, 5u, 6u, // 折叠
        7u, 8u, 9u}; // 零面积

    CollisionProxyOptions options;
    options.cellSize = 2.0f;
    const auto built = VL::Hair::BuildCollisionProxy(mesh, options);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const CollisionProxy& proxy = built.Value();
    EXPECT_EQ(proxy.report.outputTriangleCount, 2u);
    EXPECT_EQ(proxy.report.outputVertexCount, 4u);
    EXPECT_EQ(proxy.report.droppedDegenerateTriangleCount, 2u);
    EXPECT_EQ(proxy.report.droppedDuplicateTriangleCount, 1u);
    EXPECT_EQ(proxy.report.openEdgeCount, 4u);
    EXPECT_EQ(proxy.report.nonManifoldEdgeCount, 0u);
}

TEST(CollisionProxy, RegionKeepsOnlyTrianglesWhollyInsideInclusiveBounds)
{
    SdfTriangleMesh mesh;
    mesh.positions = {
        {1.0f, 1.0f, 0.0f}, {2.0f, 1.0f, 0.0f}, {2.0f, 2.0f, 0.0f}, {1.0f, 2.0f, 0.0f},
        // 恰好落在 x/y=max 上：包含边界，必须保留。
        {9.0f, 9.0f, 0.0f}, {10.0f, 9.0f, 0.0f}, {10.0f, 10.0f, 0.0f},
        // 跨出 x/y=max：整块丢弃。
        {9.5f, 9.5f, 0.0f}, {10.5f, 9.5f, 0.0f}, {10.5f, 10.5f, 0.0f},
        // 完全在区域外。
        {20.0f, 20.0f, 0.0f}, {21.0f, 20.0f, 0.0f}, {21.0f, 21.0f, 0.0f}};
    mesh.indices = {
        0u, 1u, 2u, 0u, 2u, 3u,
        4u, 5u, 6u,
        7u, 8u, 9u,
        10u, 11u, 12u};

    CollisionProxyOptions options;
    options.cellSize = 2.0f;
    options.hasRegion = true;
    options.regionMin = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    options.regionMax = Eigen::Vector3f(10.0f, 10.0f, 1.0f);
    const auto built = VL::Hair::BuildCollisionProxy(mesh, options);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const CollisionProxy& proxy = built.Value();
    EXPECT_EQ(proxy.report.regionRejectedTriangleCount, 2u);
    EXPECT_EQ(proxy.report.outputTriangleCount, 3u);
    EXPECT_EQ(proxy.report.outputVertexCount, 7u);
    EXPECT_EQ(proxy.report.droppedDegenerateTriangleCount, 0u);
    EXPECT_EQ(proxy.report.droppedDuplicateTriangleCount, 0u);

    for (const Eigen::Vector3f& position : proxy.mesh.positions)
    {
        EXPECT_GE(position.x(), options.regionMin.x());
        EXPECT_GE(position.y(), options.regionMin.y());
        EXPECT_GE(position.z(), options.regionMin.z());
        EXPECT_LE(position.x(), options.regionMax.x());
        EXPECT_LE(position.y(), options.regionMax.y());
        EXPECT_LE(position.z(), options.regionMax.z());
    }
}

TEST(CollisionProxy, ProxySerializationIsByteDeterministic)
{
    const SdfTriangleMesh cube = MakeLatticeCube(4, 2.0f);
    CollisionProxyOptions options;
    options.cellSize = 1.0f;
    const auto first = VL::Hair::BuildCollisionProxy(cube, options);
    const auto second = VL::Hair::BuildCollisionProxy(cube, options);
    ASSERT_TRUE(first.IsSuccess()) << first.Error().message;
    ASSERT_TRUE(second.IsSuccess()) << second.Error().message;

    const std::string firstText = VL::Hair::SerializeCollisionProxyObj(
        first.Value(), "fixture", FixedComment());
    const std::string secondText = VL::Hair::SerializeCollisionProxyObj(
        second.Value(), "fixture", FixedComment());
    EXPECT_FALSE(firstText.empty());
    EXPECT_EQ(firstText, secondText);
    EXPECT_EQ(first.Value().mesh.positions.size(), second.Value().mesh.positions.size());
    EXPECT_EQ(first.Value().mesh.indices, second.Value().mesh.indices);
    EXPECT_EQ(std::memcmp(first.Value().mesh.positions.data(),
                  second.Value().mesh.positions.data(),
                  first.Value().mesh.positions.size() * sizeof(Eigen::Vector3f)),
        0);
}

TEST(CollisionProxy, TopologyReportCountsOpenEdgesOnOpenBox)
{
    const SdfTriangleMesh openBox = MakeOpenBox(
        Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Vector3f(10.0f, 10.0f, 10.0f));
    CollisionProxyOptions options;
    // 1 cm 格距仍把 8 个角点分到不同格，因此聚类不改变盒子的拓扑。
    options.cellSize = 1.0f;
    const auto built = VL::Hair::BuildCollisionProxy(openBox, options);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const CollisionProxy& proxy = built.Value();
    EXPECT_EQ(proxy.report.inputTriangleCount, 10u);
    EXPECT_EQ(proxy.report.outputTriangleCount, 10u);
    EXPECT_EQ(proxy.report.outputVertexCount, 8u);
    EXPECT_EQ(proxy.report.openEdgeCount, 4u);
    EXPECT_EQ(proxy.report.nonManifoldEdgeCount, 0u);
}

TEST(CollisionProxy, MinimumTriangleFloorRejectsSmallerOutput)
{
    const SdfTriangleMesh cube = MakeLatticeCube(4, 2.0f);
    CollisionProxyOptions options;
    options.cellSize = 1.0f; // 该格距产生 48 个三角形。

    options.minimumTriangleCount = 49;
    const auto rejected = VL::Hair::BuildCollisionProxy(cube, options);
    ASSERT_TRUE(rejected.IsFailure());
    EXPECT_NE(rejected.Error().message.find("48"), std::string::npos);
    EXPECT_NE(rejected.Error().message.find("49"), std::string::npos);

    options.minimumTriangleCount = 48;
    const auto accepted = VL::Hair::BuildCollisionProxy(cube, options);
    ASSERT_TRUE(accepted.IsSuccess()) << accepted.Error().message;
    EXPECT_EQ(accepted.Value().report.outputTriangleCount, 48u);
}

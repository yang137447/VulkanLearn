#include "hair/sdfBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{
using VL::Hair::SdfField;
using VL::Hair::SdfGridSpec;
using VL::Hair::SdfTriangleMesh;

constexpr float kPi = 3.14159265358979323846f;

// 有向体积。闭合网格按外向绕序时为正；用于测试夹具统一三角形方向，
// 避免手写顺序把球/盒的法线弄反，测出与算法无关的“符号错误”。
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

// 带一个洞的盒子：去掉 z=max 的顶面（第 2 个四边形，三角形 6..11），
// 保留 10 个三角形和 4 条开放边。
SdfTriangleMesh MakeOpenBox(const Eigen::Vector3f& min, const Eigen::Vector3f& max)
{
    SdfTriangleMesh mesh = MakeBox(min, max);
    mesh.indices.erase(mesh.indices.begin() + 6, mesh.indices.begin() + 12);
    return mesh;
}

// UV 球：两极共享顶点，经度方向闭合，因此是闭合流形。
SdfTriangleMesh MakeSphere(float radius, int stacks, int slices)
{
    SdfTriangleMesh mesh;
    mesh.positions.push_back({0.0f, radius, 0.0f}); // 北极
    for (int stack = 1; stack < stacks; ++stack)
    {
        const float theta = kPi * static_cast<float>(stack) / static_cast<float>(stacks);
        for (int slice = 0; slice < slices; ++slice)
        {
            const float phi = 2.0f * kPi * static_cast<float>(slice) /
                static_cast<float>(slices);
            mesh.positions.push_back({radius * std::sin(theta) * std::cos(phi),
                radius * std::cos(theta), radius * std::sin(theta) * std::sin(phi)});
        }
    }
    const std::uint32_t south = static_cast<std::uint32_t>(mesh.positions.size());
    mesh.positions.push_back({0.0f, -radius, 0.0f});
    const auto ring = [slices](int stack, int slice) {
        return 1u + static_cast<std::uint32_t>((stack - 1) * slices + (slice % slices));
    };

    for (int slice = 0; slice < slices; ++slice)
    {
        mesh.indices.insert(mesh.indices.end(),
            {0u, ring(1, slice + 1), ring(1, slice)});
        mesh.indices.insert(mesh.indices.end(),
            {south, ring(stacks - 1, slice), ring(stacks - 1, slice + 1)});
    }
    for (int stack = 1; stack + 1 < stacks; ++stack)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            const std::uint32_t a = ring(stack, slice);
            const std::uint32_t b = ring(stack, slice + 1);
            const std::uint32_t c = ring(stack + 1, slice + 1);
            const std::uint32_t d = ring(stack + 1, slice);
            mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
        }
    }
    EnsureOutwardWinding(mesh);
    return mesh;
}

// 尖端很锐的四面体：顶点处法锥张角接近 180°，是单一面法线最容易翻符号的位置。
SdfTriangleMesh MakeSharpTetrahedron()
{
    SdfTriangleMesh mesh;
    mesh.positions = {{0.0f, 0.0f, 0.0f}, {0.5f, 0.0f, 10.0f},
        {-0.25f, 0.4f, 10.0f}, {-0.25f, -0.4f, 10.0f}};
    mesh.indices = {0u, 1u, 2u, 0u, 2u, 3u, 0u, 3u, 1u, 1u, 3u, 2u};
    EnsureOutwardWinding(mesh);
    return mesh;
}

Eigen::Vector3f MeshMin(const SdfTriangleMesh& mesh)
{
    Eigen::Vector3f min = mesh.positions.front();
    for (const auto& position : mesh.positions)
        min = min.cwiseMin(position);
    return min;
}

Eigen::Vector3f MeshMax(const SdfTriangleMesh& mesh)
{
    Eigen::Vector3f max = mesh.positions.front();
    for (const auto& position : mesh.positions)
        max = max.cwiseMax(position);
    return max;
}

// 与 CLI 相同的域规则：AABB 每侧外扩 band。
SdfGridSpec GridFor(const SdfTriangleMesh& mesh, float cellSize, float band)
{
    SdfGridSpec grid;
    grid.minCorner = MeshMin(mesh) - Eigen::Vector3f::Constant(band);
    const Eigen::Vector3f extent =
        (MeshMax(mesh) + Eigen::Vector3f::Constant(band)) - grid.minCorner;
    for (int axis = 0; axis < 3; ++axis)
        grid.dims[axis] = static_cast<std::uint32_t>(std::ceil(extent[axis] / cellSize));
    grid.cellSize = cellSize;
    grid.band = band;
    return grid;
}

Eigen::Vector3f NearestVoxelCenter(const SdfField& field, const Eigen::Vector3f& point)
{
    std::uint32_t index[3] = {0, 0, 0};
    for (int axis = 0; axis < 3; ++axis)
    {
        const long raw = static_cast<long>(
            std::floor((point[axis] - field.origin[axis]) / field.cellSize));
        index[axis] = static_cast<std::uint32_t>(
            std::max(0L, std::min(raw, static_cast<long>(field.dims[axis] - 1))));
    }
    return field.VoxelCenter(index[0], index[1], index[2]);
}

float NearestVoxelDistance(const SdfField& field, const Eigen::Vector3f& point)
{
    const Eigen::Vector3f center = NearestVoxelCenter(field, point);
    std::uint32_t index[3] = {0, 0, 0};
    for (int axis = 0; axis < 3; ++axis)
    {
        index[axis] = static_cast<std::uint32_t>(
            std::lround((center[axis] - field.origin[axis]) / field.cellSize - 0.5f));
    }
    return field.Distance(index[0], index[1], index[2]);
}

float BoxDistance(const Eigen::Vector3f& point, const Eigen::Vector3f& min,
    const Eigen::Vector3f& max)
{
    const Eigen::Vector3f center = (min + max) * 0.5f;
    const Eigen::Vector3f half = (max - min) * 0.5f;
    const Eigen::Vector3f q = (point - center).cwiseAbs() - half;
    const Eigen::Vector3f outside = q.cwiseMax(Eigen::Vector3f::Zero());
    return outside.norm() + std::min(std::max(q.x(), std::max(q.y(), q.z())), 0.0f);
}

std::vector<std::uint8_t> BytesOf(const SdfField& field)
{
    return VL::Hair::SerializeSdf(field);
}
}

TEST(SdfBuilder, SphereDistancesMatchAnalyticSphereInsideTheBand)
{
    const SdfTriangleMesh mesh = MakeSphere(5.0f, 32, 64);
    const SdfGridSpec grid = GridFor(mesh, 1.0f, 2.0f);
    const auto built = VL::Hair::BuildSdf(mesh, grid);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const SdfField& field = built.Value();
    ASSERT_EQ(field.mesh.openEdgeCount, 0u)
        << "UV sphere fixture must be a closed manifold";
    ASSERT_EQ(field.mesh.nonManifoldEdgeCount, 0u);

    int compared = 0;
    int clampedInside = 0;
    int clampedOutside = 0;
    for (std::uint32_t z = 0; z < field.dims[2]; ++z)
    {
        for (std::uint32_t y = 0; y < field.dims[1]; ++y)
        {
            for (std::uint32_t x = 0; x < field.dims[0]; ++x)
            {
                const Eigen::Vector3f center = field.VoxelCenter(x, y, z);
                const float analytic = center.norm() - 5.0f;
                const float value = field.Distance(x, y, z);
                if (analytic > grid.band + 0.05f)
                {
                    EXPECT_FLOAT_EQ(value, grid.band);
                    ++clampedOutside;
                }
                else if (analytic < -grid.band - 0.05f)
                {
                    EXPECT_FLOAT_EQ(value, -grid.band);
                    ++clampedInside;
                }
                else if (std::fabs(analytic) < grid.band - 0.05f)
                {
                    // 32×64 的球面面片与理想球最大偏差约 0.006 cm。
                    EXPECT_NEAR(value, analytic, 0.02f);
                    ++compared;
                }
            }
        }
    }
    EXPECT_GT(compared, 1000);
    EXPECT_GT(clampedInside, 50);
    EXPECT_GT(clampedOutside, 1000);

    // 采样器遵守截断与域规则：球心为 -band，域外为 +band。
    EXPECT_FLOAT_EQ(VL::Hair::SampleSdf(field, Eigen::Vector3f(0.0f, 0.0f, 0.0f)), -grid.band);
    EXPECT_FLOAT_EQ(VL::Hair::SampleSdf(field, Eigen::Vector3f(0.0f, 100.0f, 0.0f)), grid.band);
}

TEST(SdfBuilder, BoxSignIsCorrectNearEdgeAndVertexWithoutFlipping)
{
    const Eigen::Vector3f min(0.0f, 0.0f, 0.0f);
    const Eigen::Vector3f max(10.0f, 10.0f, 10.0f);
    const SdfTriangleMesh mesh = MakeBox(min, max);
    const SdfGridSpec grid = GridFor(mesh, 0.5f, 2.0f);
    const auto built = VL::Hair::BuildSdf(mesh, grid);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const SdfField& field = built.Value();
    ASSERT_EQ(field.mesh.openEdgeCount, 0u);
    ASSERT_EQ(field.mesh.nonManifoldEdgeCount, 0u);

    struct Case
    {
        Eigen::Vector3f point;
        bool inside;
        const char* name;
    };
    // 所有测试点都落在 0.5 cm 体素中心上（域最小角 -2，中心为 -1.75 + 0.5k）。
    const std::array<Case, 6> cases{{
        {{10.25f, 10.25f, 5.25f}, false, "outside near edge"},
        {{10.25f, 10.25f, 10.25f}, false, "outside near vertex"},
        {{-0.25f, -0.25f, 5.25f}, false, "outside near negative edge"},
        {{9.75f, 9.75f, 5.25f}, true, "inside near edge"},
        {{9.75f, 9.75f, 9.75f}, true, "inside near vertex"},
        {{5.25f, 9.75f, 5.25f}, true, "inside near face"}}};

    for (const Case& testCase : cases)
    {
        const Eigen::Vector3f center = NearestVoxelCenter(field, testCase.point);
        ASSERT_LT((center - testCase.point).cwiseAbs().maxCoeff(), 1.0e-6f)
            << testCase.name << " is not on a voxel center";
        const float expected = BoxDistance(center, min, max);
        const float value = NearestVoxelDistance(field, testCase.point);
        EXPECT_NEAR(value, expected, 1.0e-5f) << testCase.name;
        EXPECT_EQ(value < 0.0f, testCase.inside) << testCase.name;
    }
}

TEST(SdfBuilder, SharpApexUsesAngleWeightedPseudonormal)
{
    const SdfTriangleMesh mesh = MakeSharpTetrahedron();
    const SdfGridSpec grid = GridFor(mesh, 0.25f, 2.0f);
    const auto built = VL::Hair::BuildSdf(mesh, grid);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const SdfField& field = built.Value();
    ASSERT_EQ(field.mesh.openEdgeCount, 0u);

    // 顶点处的法锥张角很大，某些入射面法线之间点积为负。沿每条“过顶点”的面法线
    // 离开顶点都在实体之外，角度加权伪法线必须仍然给出正值。底面不经过顶点，
    // 不能把顶点当作它的起点，必须排除。
    bool sawObtusePair = false;
    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3)
    {
        const std::uint32_t i0 = mesh.indices[triangle];
        const std::uint32_t i1 = mesh.indices[triangle + 1];
        const std::uint32_t i2 = mesh.indices[triangle + 2];
        if (i0 != 0 && i1 != 0 && i2 != 0)
            continue;
        const Eigen::Vector3f& a = mesh.positions[i0];
        const Eigen::Vector3f& b = mesh.positions[i1];
        const Eigen::Vector3f& c = mesh.positions[i2];
        const Eigen::Vector3f normal = (b - a).cross(c - a).normalized();
        const Eigen::Vector3f probe = normal * 0.6f; // 顶点在原点
        const float value = NearestVoxelDistance(field, probe);
        EXPECT_GT(value, 0.0f);
        EXPECT_LT(value, 0.6f + 0.35f);
        for (std::size_t other = triangle + 3; other + 2 < mesh.indices.size(); other += 3)
        {
            const std::uint32_t j0 = mesh.indices[other];
            const std::uint32_t j1 = mesh.indices[other + 1];
            const std::uint32_t j2 = mesh.indices[other + 2];
            if (j0 != 0 && j1 != 0 && j2 != 0)
                continue;
            const Eigen::Vector3f otherNormal = (mesh.positions[j1] - mesh.positions[j0])
                .cross(mesh.positions[j2] - mesh.positions[j0]).normalized();
            if (normal.dot(otherNormal) < -0.1f)
                sawObtusePair = true;
        }
    }
    EXPECT_TRUE(sawObtusePair) << "fixture should have a normal cone wider than 90 degrees";
}

TEST(SdfBuilder, OpenMeshReportsBoundaryEdgesAndStaysPlausibleInBand)
{
    const Eigen::Vector3f min(0.0f, 0.0f, 0.0f);
    const Eigen::Vector3f max(10.0f, 10.0f, 10.0f);
    const SdfTriangleMesh mesh = MakeOpenBox(min, max);
    const SdfGridSpec grid = GridFor(mesh, 0.5f, 3.0f);
    const auto built = VL::Hair::BuildSdf(mesh, grid);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const SdfField& field = built.Value();
    EXPECT_EQ(field.mesh.triangleCount, 10u);
    EXPECT_EQ(field.mesh.openEdgeCount, 4u);
    EXPECT_EQ(field.mesh.nonManifoldEdgeCount, 0u);

    // 有洞不影响闭合侧壁附近的距离与符号。
    EXPECT_NEAR(NearestVoxelDistance(field, Eigen::Vector3f(12.25f, 5.25f, 5.25f)), 2.25f,
        1.0e-5f);
    EXPECT_NEAR(NearestVoxelDistance(field, Eigen::Vector3f(9.75f, 5.25f, 5.25f)), -0.25f,
        1.0e-5f);
    EXPECT_NEAR(NearestVoxelDistance(field, Eigen::Vector3f(5.25f, 0.25f, 5.25f)), -0.25f,
        1.0e-5f);
    // 洞上方没有虚构的顶面：最近表面是 5 cm 外的开口边缘，所以距离幅值被截断到 band。
    // 但开放网格在洞口附近没有“内/外”定义：最近点落在单面邻接的边界边上，
    // 伪法线只能给出该侧壁的朝向，符号可能与该点是否在实体外不一致。这里只
    // 断言截断幅值，不把开网格的符号当作可信结论（见生成器报告的 openEdges）。
    const float aboveHole = NearestVoxelDistance(field, Eigen::Vector3f(5.25f, 5.25f, 10.25f));
    EXPECT_FLOAT_EQ(std::fabs(aboveHole), grid.band);
}

TEST(SdfBuilder, GenerationIsByteDeterministic)
{
    const SdfTriangleMesh mesh = MakeBox(Eigen::Vector3f(0.0f, 0.0f, 0.0f),
        Eigen::Vector3f(10.0f, 10.0f, 10.0f));
    const SdfGridSpec grid = GridFor(mesh, 0.5f, 2.0f);
    const auto first = VL::Hair::BuildSdf(mesh, grid);
    const auto second = VL::Hair::BuildSdf(mesh, grid);
    ASSERT_TRUE(first.IsSuccess());
    ASSERT_TRUE(second.IsSuccess());
    EXPECT_EQ(first.Value().distances.size(), second.Value().distances.size());
    EXPECT_EQ(BytesOf(first.Value()), BytesOf(second.Value()));
    EXPECT_EQ(std::memcmp(first.Value().distances.data(), second.Value().distances.data(),
                  first.Value().distances.size() * sizeof(float)), 0);
}

TEST(SdfBuilder, FileRoundTripPreservesField)
{
    const SdfTriangleMesh mesh = MakeSphere(5.0f, 16, 32);
    const SdfGridSpec grid = GridFor(mesh, 1.0f, 3.0f);
    const auto built = VL::Hair::BuildSdf(mesh, grid);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const SdfField& field = built.Value();

    const std::vector<std::uint8_t> bytes = BytesOf(field);
    ASSERT_EQ(bytes.size(), VL::Hair::SdfFileHeaderBytes + field.distances.size() * 4u);
    const auto parsed = VL::Hair::ParseSdf(bytes);
    ASSERT_TRUE(parsed.IsSuccess()) << parsed.Error().message;
    EXPECT_EQ(parsed.Value().dims, field.dims);
    EXPECT_EQ(parsed.Value().origin, field.origin);
    EXPECT_FLOAT_EQ(parsed.Value().cellSize, field.cellSize);
    EXPECT_FLOAT_EQ(parsed.Value().band, field.band);
    EXPECT_EQ(parsed.Value().distances, field.distances);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "vl_sdf_roundtrip_test.sdf";
    const auto written = VL::Hair::WriteSdfFile(field, path);
    ASSERT_TRUE(written.IsSuccess()) << written.Error().message;
    EXPECT_EQ(std::filesystem::file_size(path), bytes.size());
    const auto loaded = VL::Hair::LoadSdfFile(path);
    ASSERT_TRUE(loaded.IsSuccess()) << loaded.Error().message;
    EXPECT_EQ(loaded.Value().dims, field.dims);
    EXPECT_EQ(loaded.Value().origin, field.origin);
    EXPECT_FLOAT_EQ(loaded.Value().cellSize, field.cellSize);
    EXPECT_EQ(std::memcmp(loaded.Value().distances.data(), field.distances.data(),
                  field.distances.size() * sizeof(float)), 0);
    std::filesystem::remove(path);
}

TEST(SdfBuilder, TruncatedAndCorruptedFilesAreRejected)
{
    const SdfTriangleMesh mesh = MakeBox(Eigen::Vector3f(0.0f, 0.0f, 0.0f),
        Eigen::Vector3f(4.0f, 4.0f, 4.0f));
    const SdfGridSpec grid = GridFor(mesh, 1.0f, 1.0f);
    const auto built = VL::Hair::BuildSdf(mesh, grid);
    ASSERT_TRUE(built.IsSuccess()) << built.Error().message;
    const std::vector<std::uint8_t> bytes = BytesOf(built.Value());
    ASSERT_GT(bytes.size(), 16u);

    const std::vector<std::uint8_t> cut(bytes.begin(), bytes.end() - 8);
    EXPECT_TRUE(VL::Hair::ParseSdf(cut).IsFailure());

    const std::vector<std::uint8_t> shortHeader(bytes.begin(), bytes.begin() + 16);
    EXPECT_TRUE(VL::Hair::ParseSdf(shortHeader).IsFailure());

    std::vector<std::uint8_t> corrupted = bytes;
    corrupted[VL::Hair::SdfFileHeaderBytes] ^= 0xffu;
    EXPECT_TRUE(VL::Hair::ParseSdf(corrupted).IsFailure());

    std::vector<std::uint8_t> badMagic = bytes;
    badMagic[0] = 'X';
    EXPECT_TRUE(VL::Hair::ParseSdf(badMagic).IsFailure());
}

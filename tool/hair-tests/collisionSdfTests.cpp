#include "hair/collisionMeshObj.h"
#include "hair/tressfxSdfGrid.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
using VL::Hair::SdfTriangleMesh;
using VL::Hair::TressfxSdfLayout;

constexpr float kPi = 3.14159265358979323846f;

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

// 有符号场依赖三角形绕序；夹具统一成外向绕序，避免测到与算法无关的符号翻转。
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

SdfTriangleMesh MakeSphere(float radius, int stacks, int slices)
{
    SdfTriangleMesh mesh;
    mesh.positions.push_back({0.0f, radius, 0.0f});
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
        mesh.indices.insert(mesh.indices.end(), {0u, ring(1, slice + 1), ring(1, slice)});
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

TressfxSdfLayout Layout(const SdfTriangleMesh& mesh, float cellSize, float margin)
{
    const auto layout = VL::Hair::MakeTressfxSdfLayout(mesh, cellSize, margin);
    EXPECT_TRUE(layout.IsSuccess()) << (layout.IsFailure() ? layout.Error().message : "");
    return layout.IsSuccess() ? layout.Value() : TressfxSdfLayout{};
}

// 均匀场：所有格子同一个有符号值，中心差分为 0，用于退化梯度测试。
std::vector<std::uint32_t> UniformGrid(const TressfxSdfLayout& layout, float distance)
{
    std::vector<std::uint32_t> cells(layout.CellCount(),
        VL::Hair::TressfxSdfFloatFlip(distance));
    return cells;
}
}

TEST(CollisionMeshObj, ParsesProxySubsetAndIgnoresStandardDirectives)
{
    const std::string text =
        "# vl_build_collision_proxy offline proxy\n"
        "o female_body_proxy\n"
        "v 0 0 0\n"
        "v 10 0 0\n"
        "v 0 10 0\n"
        "v 0 0 10\n"
        "vt 0.25 0.5\n"
        "vn 0 0 1\n"
        "s off\n"
        "usemtl skin\n"
        "f 1 2 3\n"
        "f 1/1 3//1 4/1/1\n";
    const auto parsed = VL::Hair::ParseCollisionMeshObj(text, "proxy.obj");
    ASSERT_TRUE(parsed.IsSuccess()) << parsed.Error().message;
    const SdfTriangleMesh& mesh = parsed.Value();
    ASSERT_EQ(mesh.positions.size(), 4u);
    EXPECT_TRUE(mesh.positions[3].isApprox(Eigen::Vector3f(0, 0, 10)));
    ASSERT_EQ(mesh.indices.size(), 6u);
    EXPECT_EQ(mesh.indices[0], 0u);
    EXPECT_EQ(mesh.indices[1], 1u);
    EXPECT_EQ(mesh.indices[2], 2u);
    EXPECT_EQ(mesh.indices[3], 0u);
    EXPECT_EQ(mesh.indices[4], 2u);
    EXPECT_EQ(mesh.indices[5], 3u);
}

TEST(CollisionMeshObj, TriangulatesPolygonsAndResolvesNegativeIndices)
{
    const std::string quad =
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n";
    const auto parsedQuad = VL::Hair::ParseCollisionMeshObj(quad);
    ASSERT_TRUE(parsedQuad.IsSuccess()) << parsedQuad.Error().message;
    ASSERT_EQ(parsedQuad.Value().indices.size(), 6u);
    EXPECT_EQ(parsedQuad.Value().indices[3], 0u);
    EXPECT_EQ(parsedQuad.Value().indices[4], 2u);
    EXPECT_EQ(parsedQuad.Value().indices[5], 3u);

    const std::string negative =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n";
    const auto parsedNegative = VL::Hair::ParseCollisionMeshObj(negative);
    ASSERT_TRUE(parsedNegative.IsSuccess()) << parsedNegative.Error().message;
    ASSERT_EQ(parsedNegative.Value().indices.size(), 3u);
    EXPECT_EQ(parsedNegative.Value().indices[0], 0u);
    EXPECT_EQ(parsedNegative.Value().indices[1], 1u);
    EXPECT_EQ(parsedNegative.Value().indices[2], 2u);
}

TEST(CollisionMeshObj, RejectsMalformedLinesWithPathAndLineNumber)
{
    const std::string badCoordinate = "v 1 2\n";
    const auto badVertex = VL::Hair::ParseCollisionMeshObj(badCoordinate, "Maps/x/proxy.obj");
    ASSERT_TRUE(badVertex.IsFailure());
    EXPECT_NE(badVertex.Error().message.find("vertex"), std::string::npos);
    EXPECT_NE(badVertex.Error().message.find("proxy.obj:1"), std::string::npos);

    const std::string nonFinite = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv nan 0 0\nf 1 2 3\n";
    const auto badValue = VL::Hair::ParseCollisionMeshObj(nonFinite);
    ASSERT_TRUE(badValue.IsFailure());
    EXPECT_NE(badValue.Error().message.find("non-finite"), std::string::npos);
    EXPECT_NE(badValue.Error().message.find(":4"), std::string::npos);

    const std::string outOfRange = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n";
    const auto badIndex = VL::Hair::ParseCollisionMeshObj(outOfRange, "proxy.obj");
    ASSERT_TRUE(badIndex.IsFailure());
    EXPECT_NE(badIndex.Error().message.find("proxy.obj:4"), std::string::npos);

    const std::string zeroIndex = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n";
    EXPECT_TRUE(VL::Hair::ParseCollisionMeshObj(zeroIndex).IsFailure());

    const std::string tooNegative = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -4 -2 -1\n";
    EXPECT_TRUE(VL::Hair::ParseCollisionMeshObj(tooNegative).IsFailure());

    const std::string garbage = "v 0 0 0\n123 4 5\n";
    EXPECT_TRUE(VL::Hair::ParseCollisionMeshObj(garbage).IsFailure());

    EXPECT_TRUE(VL::Hair::ParseCollisionMeshObj("v 0 0 0\n").IsFailure());
    EXPECT_TRUE(VL::Hair::ParseCollisionMeshObj("").IsFailure());
}

TEST(TressfxSdfGrid, FloatFlipIsMagnitudeFirstAndPreservesSign)
{
    const std::array<float, 8> values{-10.0f, -2.0f, -0.5f, -0.01f, 0.01f, 0.5f, 2.0f,
        10.0f};
    for (float value : values)
    {
        const std::uint32_t bits = VL::Hair::TressfxSdfFloatFlip(value);
        EXPECT_FLOAT_EQ(VL::Hair::TressfxSdfIFloatFlip(bits), value);
    }
    // magnitude-first：min 先选 |d| 最小（最近表面），符号在最低位。
    EXPECT_LT(VL::Hair::TressfxSdfFloatFlip(-0.5f), VL::Hair::TressfxSdfFloatFlip(-2.0f));
    EXPECT_LT(VL::Hair::TressfxSdfFloatFlip(0.5f), VL::Hair::TressfxSdfFloatFlip(2.0f));
    EXPECT_LT(VL::Hair::TressfxSdfFloatFlip(0.5f), VL::Hair::TressfxSdfFloatFlip(-0.5f));
    // 哨兵必须比任何有限距离（包括 1e10）的 key 都大。
    EXPECT_GT(VL::Hair::TressfxSdfInitialBits,
        VL::Hair::TressfxSdfFloatFlip(VL::Hair::TressfxSdfOutOfRangeDistance));
}

TEST(TressfxSdfGrid, LayoutUsesTargetCellSizeAndFixedResolution)
{
    EXPECT_EQ(VL::Hair::TressfxSdfResolutionForCellSize(1.0f, 10.0f), 48u);
    EXPECT_EQ(VL::Hair::TressfxSdfResolutionForCellSize(0.1f, 10.0f), 64u);
    EXPECT_EQ(VL::Hair::TressfxSdfResolutionForCellSize(4.0f, 10.0f), 48u);

    const SdfTriangleMesh box = MakeBox(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(10, 10, 10));
    const TressfxSdfLayout layout = Layout(box, 1.0f, 1.0f);
    EXPECT_EQ(layout.resolution, 48u);
    EXPECT_EQ(layout.dims[0], 48u);
    EXPECT_EQ(layout.dims[1], 48u);
    EXPECT_EQ(layout.dims[2], 48u);
    EXPECT_EQ(layout.CellCount(), 48u * 48u * 48u);
    // side = (maxExtent + 2*margin) / (1 - 4/resolution)；10cm 盒、margin 1。
    const float side = (10.0f + 2.0f) / (1.0f - 4.0f / 48.0f);
    EXPECT_NEAR(layout.cellSize, side / 48.0f, 1.0e-5f);
    EXPECT_NEAR(layout.cellSize, 0.272727f, 1.0e-5f);
    EXPECT_NEAR(layout.truncation, 1.0f + 2.0f * layout.cellSize, 1.0e-5f);
    EXPECT_TRUE(layout.origin.isApprox(
        Eigen::Vector3f::Constant(5.0f - 0.5f * side), 1.0e-5f));
    EXPECT_TRUE(layout.CellCenter(0, 0, 0).isApprox(
        layout.origin + Eigen::Vector3f::Constant(0.5f * layout.cellSize), 1.0e-5f));
}

TEST(TressfxSdfGrid, DomainFollowsDeformedBoundsAndClampsDegenerateExtent)
{
    const auto bind = VL::Hair::MakeTressfxSdfDomain(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(10, 10, 10), 1.0f, 48u);
    ASSERT_TRUE(bind.IsSuccess()) << bind.Error().message;
    const auto moved = VL::Hair::MakeTressfxSdfDomain(
        Eigen::Vector3f(15, 0, 0), Eigen::Vector3f(25, 10, 10), 1.0f, 48u);
    ASSERT_TRUE(moved.IsSuccess()) << moved.Error().message;
    EXPECT_NEAR(moved.Value().origin.x(), bind.Value().origin.x() + 15.0f, 1.0e-4f);
    EXPECT_NEAR(moved.Value().cellSize, bind.Value().cellSize, 1.0e-5f);
    EXPECT_EQ(moved.Value().dims, bind.Value().dims);
    EXPECT_EQ(moved.Value().resolution, 48u);

    // 零 extent：按最小边长 1cm 建域，cellSize 有限且为正，不除零。
    const auto degenerate = VL::Hair::MakeTressfxSdfDomain(
        Eigen::Vector3f(2, 2, 2), Eigen::Vector3f(2, 2, 2), 1.0f, 48u);
    ASSERT_TRUE(degenerate.IsSuccess()) << degenerate.Error().message;
    EXPECT_GT(degenerate.Value().cellSize, 0.0f);
    EXPECT_TRUE(std::isfinite(degenerate.Value().cellSize));
    EXPECT_TRUE(degenerate.Value().origin.allFinite());
    EXPECT_EQ(degenerate.Value().dims[0], 48u);

    const auto nonFinite = VL::Hair::MakeTressfxSdfDomain(
        Eigen::Vector3f(std::numeric_limits<float>::quiet_NaN(), 0, 0),
        Eigen::Vector3f(1, 1, 1), 1.0f, 48u);
    EXPECT_TRUE(nonFinite.IsFailure());
}

TEST(TressfxSdfGrid, NearSurfacePointsArePushedToMarginAndFarPointsAreUntouched)
{
    const SdfTriangleMesh box = MakeBox(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(10, 10, 10));
    const TressfxSdfLayout layout = Layout(box, 1.0f, 1.0f);
    const std::vector<std::uint32_t> cells = VL::Hair::BuildTressfxSdfGrid(layout, box);

    // 外侧 0.5 cm：有符号距离为正且 < margin，沿 +x 推到 1 cm 外。
    Eigen::Vector3f outsideNear(10.5f, 5.0f, 5.0f);
    const float outsideDistance = VL::Hair::SampleTressfxSdfValue(layout, cells, outsideNear);
    EXPECT_GT(outsideDistance, 0.2f);
    EXPECT_LT(outsideDistance, 0.8f);
    EXPECT_TRUE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, outsideNear));
    EXPECT_NEAR(outsideNear.x(), 11.0f, 0.25f);
    EXPECT_NEAR(outsideNear.y(), 5.0f, 0.05f);
    EXPECT_NEAR(outsideNear.z(), 5.0f, 0.05f);
    EXPECT_NEAR(VL::Hair::SampleTressfxSdfValue(layout, cells, outsideNear), 1.0f, 0.25f);

    // 内侧 0.5 cm：有符号距离为负，仍必须处理，并被推到表面外侧 1 cm。
    Eigen::Vector3f insideNear(9.5f, 5.0f, 5.0f);
    EXPECT_LT(VL::Hair::SampleTressfxSdfValue(layout, cells, insideNear), -0.2f);
    EXPECT_TRUE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, insideNear));
    EXPECT_NEAR(insideNear.x(), 11.0f, 0.35f);
    EXPECT_NEAR(VL::Hair::SampleTressfxSdfValue(layout, cells, insideNear), 1.0f, 0.25f);

    // 域外/远点：距离 > margin，位置不动。
    Eigen::Vector3f farOutside(13.5f, 5.0f, 5.0f);
    const Eigen::Vector3f untouched = farOutside;
    EXPECT_FALSE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, farOutside));
    EXPECT_TRUE(farOutside.isApprox(untouched));
}

TEST(TressfxSdfGrid, SphereInteriorPointIsPushedOutsideAndOutsidePointOnlyMovesInsideMargin)
{
    const SdfTriangleMesh sphere = MakeSphere(5.0f, 8, 16);
    const SdfTriangleMesh coarseSphere = MakeSphere(5.0f, 6, 12);

    // margin 1：外侧近表面点被推到外侧 1 cm；更远的点保持不动。
    {
        const TressfxSdfLayout layout = Layout(sphere, 1.0f, 1.0f);
        const std::vector<std::uint32_t> cells =
            VL::Hair::BuildTressfxSdfGrid(layout, sphere);

        // 外侧 1.5 cm 的采样必须是正距离（远离表面）。
        EXPECT_GT(VL::Hair::SampleTressfxSdfValue(layout, cells,
                      Eigen::Vector3f(6.5f, 0.0f, 0.0f)), 0.5f);

        // 外侧 0.5 cm：距离 < margin，必须被推到 > margin；有符号场在极近
        // 表面处受格距影响，符号可能略偏负，但梯度仍指向外侧。
        Eigen::Vector3f outsideNear(5.5f, 0.0f, 0.0f);
        EXPECT_LT(VL::Hair::SampleTressfxSdfValue(layout, cells, outsideNear), 1.0f);
        ASSERT_TRUE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, outsideNear));
        EXPECT_GT(outsideNear.norm(), 5.4f);
        EXPECT_GT(VL::Hair::SampleTressfxSdfValue(layout, cells, outsideNear), 0.0f);

        // 外侧 2 cm：distance > margin，保持不动。
        Eigen::Vector3f farOutside(7.0f, 0.0f, 0.0f);
        const Eigen::Vector3f untouched = farOutside;
        EXPECT_FALSE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, farOutside));
        EXPECT_TRUE(farOutside.isApprox(untouched));
    }

    // margin 3：truncation 带覆盖 2 cm 深的内部点；它必须是负距离并被推到
    // 表面外侧（要求 distance >= margin）。
    {
        const TressfxSdfLayout layout = Layout(coarseSphere, 1.0f, 3.0f);
        const std::vector<std::uint32_t> cells =
            VL::Hair::BuildTressfxSdfGrid(layout, coarseSphere);
        Eigen::Vector3f interior(3.0f, 0.0f, 0.0f);
        const float interiorDistance =
            VL::Hair::SampleTressfxSdfValue(layout, cells, interior);
        EXPECT_LT(interiorDistance, -1.0f);
        ASSERT_TRUE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, interior));
        EXPECT_GT(interior.norm(), 5.0f + 0.9f * layout.margin);
        const float after = VL::Hair::SampleTressfxSdfValue(layout, cells, interior);
        EXPECT_GT(after, 0.0f);
        EXPECT_GE(after, 0.9f * layout.margin);
    }
}

TEST(TressfxSdfGrid, DegenerateGradientIsSkippedInsteadOfProducingNaN)
{
    const SdfTriangleMesh box = MakeBox(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(10, 10, 10));
    const TressfxSdfLayout layout = Layout(box, 1.0f, 1.0f);
    const std::vector<std::uint32_t> cells = UniformGrid(layout, 0.0f);

    Eigen::Vector3f point(5.0f, 5.0f, 5.0f);
    const Eigen::Vector3f before = point;
    EXPECT_FLOAT_EQ(VL::Hair::SampleTressfxSdfValue(layout, cells, point), 0.0f);
    EXPECT_LE(VL::Hair::TressfxSdfGradient(layout, cells, point).squaredNorm(),
        VL::Hair::TressfxSdfGradientEpsilonSquared);
    EXPECT_FALSE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, point));
    EXPECT_TRUE(point.isApprox(before));
}

TEST(TressfxSdfGrid, ProxySkinFlagRoundTripsIntoParams)
{
    const SdfTriangleMesh box = MakeBox(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(10, 10, 10));
    const TressfxSdfLayout layout = Layout(box, 1.0f, 1.0f);
    EXPECT_EQ(VL::Hair::MakeTressfxSdfParams(layout, 12).counts[1], 0u);
    const auto skinned = VL::Hair::MakeTressfxSdfParams(layout, 12, true);
    EXPECT_EQ(skinned.counts[1], 1u);
    EXPECT_EQ(skinned.counts[0], 12u);
}

TEST(TressfxSdfGrid, SkinningMatrixMovesTheFieldWithTheProxy)
{
    const SdfTriangleMesh box = MakeBox(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(10, 10, 10));
    // 域覆盖静止位置 [0,10] 与平移后的 [20,30]；splat 只喂平移后的代理。
    const SdfTriangleMesh domain = MakeBox(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(30, 10, 10));
    const TressfxSdfLayout layout = Layout(domain, 1.0f, 1.0f);
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation(0, 3) = 20.0f;
    const std::vector<std::uint32_t> cells =
        VL::Hair::BuildTressfxSdfGrid(layout, box, translation);

    // 平移后的外侧近表面点被推回 margin；静止位置上的对应点远离代理，保持不动。
    Eigen::Vector3f translated(30.5f, 5.0f, 5.0f);
    EXPECT_TRUE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, translated));
    EXPECT_NEAR(translated.x(), 31.0f, 0.3f);

    Eigen::Vector3f original(10.5f, 5.0f, 5.0f);
    const Eigen::Vector3f untouched = original;
    EXPECT_FALSE(VL::Hair::ResolveTressfxSdfCollision(layout, cells, original));
    EXPECT_TRUE(original.isApprox(untouched));
}

namespace
{
// 合成绑定姿态身体：positions + CSR 影响。测试只关心“最近顶点/权重复制/
// 归一化/确定性”，不需要真实 FBX。
struct ProxySkinBody
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<std::uint32_t> offsets;
    std::vector<VL::Hair::SdfProxySkinSourceInfluence> influences;

    VL::Hair::SdfProxySkinSource View() const
    {
        return {positions.data(), positions.size(), offsets.data(),
            influences.data()};
    }
};

void AddBodyVertex(ProxySkinBody& body, const Eigen::Vector3f& position,
    std::initializer_list<std::pair<uint32_t, float>> weights)
{
    if (body.offsets.empty())
        body.offsets.push_back(0);
    for (const auto& [joint, weight] : weights)
        body.influences.push_back({joint, weight});
    body.offsets.push_back(static_cast<uint32_t>(body.influences.size()));
    body.positions.push_back(position);
}
} // namespace

TEST(TressfxProxySkinning, CopiesNearestBodyVertexInfluences)
{
    ProxySkinBody body;
    AddBodyVertex(body, {0.0f, 0.0f, 0.0f}, {{0, 0.25f}, {1, 0.75f}});
    AddBodyVertex(body, {10.0f, 0.0f, 0.0f}, {{2, 1.0f}});
    const std::vector<Eigen::Vector3f> proxy{{0.2f, 0.0f, 0.0f}};
    const auto transfer = VL::Hair::BuildTressfxProxySkinTransfer(
        proxy, body.View(), 2.0f);
    ASSERT_EQ(transfer.vertexCount, 1u);
    ASSERT_EQ(transfer.maxInfluences, 2u);
    // CSR：header = 2 个顶点偏移；随后 (joint, weightBits) × 2。
    ASSERT_EQ(transfer.words.size(), 2u + 4u);
    EXPECT_EQ(transfer.words[0], 2u);
    EXPECT_EQ(transfer.words[1], 6u);
    EXPECT_EQ(transfer.words[2], 0u);
    float weight0 = 0.0f;
    std::memcpy(&weight0, &transfer.words[3], sizeof(weight0));
    EXPECT_NEAR(weight0, 0.25f, 1.0e-6f);
    EXPECT_EQ(transfer.uniqueBodyVertices, 1u);
    EXPECT_NEAR(transfer.maxNearestDistanceCm, 0.2f, 1.0e-5f);
}

TEST(TressfxProxySkinning, EqualDistanceTieBreaksOnLowerBodyIndex)
{
    ProxySkinBody body;
    AddBodyVertex(body, {-1.0f, 0.0f, 0.0f}, {{4, 1.0f}});
    AddBodyVertex(body, {1.0f, 0.0f, 0.0f}, {{5, 1.0f}});
    const std::vector<Eigen::Vector3f> proxy{{0.0f, 0.0f, 0.0f}};
    const auto transfer = VL::Hair::BuildTressfxProxySkinTransfer(
        proxy, body.View(), 2.0f);
    ASSERT_EQ(transfer.words.size(), 4u);
    EXPECT_EQ(transfer.words[2], 4u) << "tie must select the lower body index";
}

TEST(TressfxProxySkinning, KeepsAllInfluencesAndNormalizesWeights)
{
    ProxySkinBody body;
    // 6 个影响、权重和 3.0：CSR 布局没有 4 骨上限，保留全部并重新归一化。
    AddBodyVertex(body, {0.0f, 0.0f, 0.0f},
        {{0, 0.5f}, {1, 0.5f}, {2, 0.5f}, {3, 0.5f}, {4, 0.5f}, {5, 0.5f}});
    const std::vector<Eigen::Vector3f> proxy{{0.0f, 0.0f, 0.0f}};
    const auto transfer = VL::Hair::BuildTressfxProxySkinTransfer(
        proxy, body.View(), 1.0f);
    EXPECT_EQ(transfer.maxInfluences, 6u);
    ASSERT_EQ(transfer.words.size(), 2u + 12u);
    float sum = 0.0f;
    for (std::size_t word = 3; word < transfer.words.size(); word += 2)
    {
        float weight = 0.0f;
        std::memcpy(&weight, &transfer.words[word], sizeof(weight));
        sum += weight;
    }
    EXPECT_NEAR(sum, 1.0f, 1.0e-6f);
}

TEST(TressfxProxySkinning, RejectsProxyVerticesBeyondTransferRadius)
{
    ProxySkinBody body;
    AddBodyVertex(body, {100.0f, 0.0f, 0.0f}, {{0, 1.0f}});
    const std::vector<Eigen::Vector3f> proxy{{0.0f, 0.0f, 0.0f}};
    try
    {
        VL::Hair::BuildTressfxProxySkinTransfer(proxy, body.View(), 2.0f);
        FAIL() << "A proxy vertex far outside the transfer radius was accepted";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string(error.what()).find("nearest body vertex"),
            std::string::npos);
    }
}

TEST(TressfxProxySkinning, RecordPolicyMarksUnmatchedRootsWithoutThrowing)
{
    ProxySkinBody body;
    AddBodyVertex(body, {0.0f, 0.0f, 0.0f}, {{0, 1.0f}});
    // 发丝根点路径：第一个点可匹配，第二个点离身体 100 cm，交给调用方按根回退。
    const std::vector<Eigen::Vector3f> roots{{0.1f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}};
    const auto transfer = VL::Hair::BuildTressfxProxySkinTransfer(
        roots, body.View(), 2.0f,
        VL::Hair::TressfxSkinTransferPolicy::RecordUnmatched);
    ASSERT_EQ(transfer.vertexCount, 2u);
    ASSERT_EQ(transfer.unmatchedVertices.size(), 1u);
    EXPECT_EQ(transfer.unmatchedVertices[0], 1u);
    // CSR 仍是 1:1：未匹配点给空区间（begin == end），而不是零矩阵影响。
    EXPECT_EQ(transfer.words[0], 3u);
    EXPECT_EQ(transfer.words[1], 5u);
    EXPECT_EQ(transfer.words[2], 5u);
    EXPECT_NEAR(transfer.maxNearestDistanceCm, 0.1f, 1.0e-5f);
}

TEST(TressfxProxySkinning, RecordPolicyMarksVertexWithoutPositiveWeights)
{
    ProxySkinBody body;
    AddBodyVertex(body, {0.0f, 0.0f, 0.0f}, {{0, 0.0f}});
    const std::vector<Eigen::Vector3f> roots{{0.0f, 0.0f, 0.0f}};
    const auto transfer = VL::Hair::BuildTressfxProxySkinTransfer(
        roots, body.View(), 2.0f,
        VL::Hair::TressfxSkinTransferPolicy::RecordUnmatched);
    ASSERT_EQ(transfer.unmatchedVertices.size(), 1u);
    EXPECT_EQ(transfer.unmatchedVertices[0], 0u);
    EXPECT_EQ(transfer.words.size(), 2u);
    EXPECT_EQ(transfer.words[0], 2u);
    EXPECT_EQ(transfer.words[1], 2u);
}

TEST(TressfxProxySkinning, SkinnedPositionBlendsBoneMatrices)
{
    ProxySkinBody body;
    AddBodyVertex(body, {0.0f, 0.0f, 0.0f}, {{0, 0.5f}, {1, 0.5f}});
    const std::vector<Eigen::Vector3f> proxy{{0.0f, 0.0f, 0.0f}};
    const auto transfer = VL::Hair::BuildTressfxProxySkinTransfer(
        proxy, body.View(), 1.0f);

    std::vector<Eigen::Matrix4f> matrices(2, Eigen::Matrix4f::Identity());
    matrices[0](0, 3) = 10.0f;
    matrices[1](0, 3) = 30.0f;
    const Eigen::Vector3f skinned = VL::Hair::TressfxProxySkinnedPosition(
        transfer, 0, matrices, Eigen::Vector3f(1.0f, 0.0f, 0.0f));
    EXPECT_TRUE(skinned.isApprox(Eigen::Vector3f(21.0f, 0.0f, 0.0f), 1.0e-5f));
}

TEST(TressfxProxySkinning, SplatMatchesRigidMatrixWhenEveryWeightIsOneBone)
{
    const SdfTriangleMesh box = MakeBox(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(10, 10, 10));
    const SdfTriangleMesh domain = MakeBox(
        Eigen::Vector3f(0, 0, 0), Eigen::Vector3f(30, 10, 10));
    const TressfxSdfLayout layout = Layout(domain, 1.0f, 1.0f);

    // 每个代理顶点 100% 绑定关节 0；该关节矩阵就是单骨路径的平移矩阵。
    ProxySkinBody body;
    AddBodyVertex(body, {5.0f, 5.0f, 5.0f}, {{0, 1.0f}});
    std::vector<Eigen::Vector3f> proxyPositions = box.positions;
    const auto transfer = VL::Hair::BuildTressfxProxySkinTransfer(
        proxyPositions, body.View(), 10.0f);
    std::vector<Eigen::Matrix4f> matrices(1, Eigen::Matrix4f::Identity());
    matrices[0](0, 3) = 20.0f;

    const auto rigid = VL::Hair::BuildTressfxSdfGrid(layout, box, matrices[0]);
    const auto skinned = VL::Hair::BuildTressfxSdfGrid(layout, box, matrices[0],
        &transfer, &matrices);
    EXPECT_EQ(rigid, skinned);
}

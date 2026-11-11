#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#include "hair/tressfxGroupPaint.h"
#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxRootCloud.h"
#include "hair/tressfxSurfaceBrush.h"

namespace
{
using VL::Hair::BrushCamera;
using VL::Hair::BrushSettings;
using VL::Hair::BrushStamp;
using VL::Hair::BrushViewport;
using VL::Hair::GroupPaintController;
using VL::Hair::GroupPaintMode;
using VL::Hair::RootCloudSelector;
using VL::Hair::RootHit;
using VL::Hair::SurfaceBrush;
using VL::Hair::TressfxGroupBinding;
using VL::Hair::TressfxRootCloud;

std::shared_ptr<const TressfxRootCloud> BuildSphereCloud(
    size_t count = 900, float radius = 10.0f,
    const Eigen::Vector3f& growth = Eigen::Vector3f::UnitY())
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Vector3f> growthDirections;
    positions.reserve(count);
    growthDirections.reserve(count);
    const float goldenAngle = 3.14159265358979323846f * (3.0f - std::sqrt(5.0f));
    for (size_t index = 0; index < count; ++index)
    {
        const float y = 1.0f - 2.0f * (static_cast<float>(index) + 0.5f) /
            static_cast<float>(count);
        const float ring = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float theta = goldenAngle * static_cast<float>(index);
        positions.emplace_back(radius * ring * std::cos(theta), radius * y,
            radius * ring * std::sin(theta));
        growthDirections.push_back(growth);
    }
    return TressfxRootCloud::Build(positions, growthDirections);
}

BrushCamera FrontCamera()
{
    BrushCamera camera;
    camera.position = Eigen::Vector3f(0.0f, 0.0f, 100.0f);
    camera.forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    camera.right = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    camera.up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    camera.horizontalFovDegrees = 60.0f;
    camera.clipNear = 0.1f;
    camera.clipFar = 1000.0f;
    return camera;
}

BrushViewport Viewport()
{
    return {0, 0, 1000, 1000};
}

BrushSettings BrushDefaults()
{
    BrushSettings settings;
    settings.radiusPixels = 50.0f;
    settings.maxNormalAngleDegrees = 60.0f;
    settings.selectionThreshold = 0.05f;
    return settings;
}

BrushStamp FrontStamp()
{
    BrushStamp stamp;
    stamp.valid = true;
    stamp.position = Eigen::Vector3f(0.0f, 0.0f, 10.0f);
    stamp.normal = Eigen::Vector3f::UnitZ();
    stamp.rayDirection = -Eigen::Vector3f::UnitZ();
    stamp.radiusWorld = 6.0f;
    stamp.depth = 90.0f;
    return stamp;
}

// 40k 点云的 PCA 法线构造成本较高；同一进程内复用不可变快照。
const std::shared_ptr<const TressfxRootCloud>& FortyThousandCloud()
{
    static const std::shared_ptr<const TressfxRootCloud> cloud =
        BuildSphereCloud(40334, 10.0f);
    return cloud;
}

std::vector<uint32_t> BruteForceRay(const TressfxRootCloud& roots,
    const Eigen::Vector3f& origin, const Eigen::Vector3f& direction,
    float maxDistance, float radius)
{
    struct Entry
    {
        uint32_t index;
        float depth;
    };
    std::vector<Entry> accepted;
    for (uint32_t index = 0; index < roots.RootCount(); ++index)
    {
        const Eigen::Vector3f offset = roots.Roots()[index].position - origin;
        const float depth = offset.dot(direction);
        if (depth < 0.0f || depth > maxDistance) continue;
        const float perpendicularSquared =
            std::max(0.0f, offset.squaredNorm() - depth * depth);
        if (perpendicularSquared > radius * radius + 1.0e-6f) continue;
        accepted.push_back({index, depth});
    }
    std::sort(accepted.begin(), accepted.end(), [](const Entry& left, const Entry& right)
    {
        if (left.depth != right.depth) return left.depth < right.depth;
        return left.index < right.index;
    });
    std::vector<uint32_t> result;
    result.reserve(accepted.size());
    for (const Entry& entry : accepted) result.push_back(entry.index);
    return result;
}

// 两段 code 可能因 FMA 收缩得到 1 ulp 不同的 depth；排序契约允许近似
// 深度并列，因此回归只比较命中的 guide 集合。
std::vector<uint32_t> GuideSet(std::vector<uint32_t> candidates)
{
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}
} // namespace

TEST(TressfxSurfaceBrush, CenterCursorGeneratesForwardRayAndWorldRadius)
{
    const auto roots = BuildSphereCloud();
    BrushSettings settings = BrushDefaults();
    BrushStamp stamp = SurfaceBrush().Sample(
        Eigen::Vector2f(500.0f, 500.0f), Viewport(), FrontCamera(), *roots,
        Eigen::Matrix4f::Identity(), settings);
    ASSERT_TRUE(stamp.valid);
    EXPECT_LT((stamp.rayDirection - (-Eigen::Vector3f::UnitZ())).norm(), 1.0e-4f);
    EXPECT_NEAR(stamp.normal.z(), 1.0f, 0.15f);
    EXPECT_NEAR(stamp.position.z(), 10.0f, 2.0f);
    // r = r_px * 2*tan(fovY/2) * depth / viewportHeight；60° horizontal + 1:1 viewport。
    const float expected = 50.0f * 2.0f * std::tan(30.0f * 3.14159265358979323846f / 180.0f) *
        stamp.depth / 1000.0f;
    EXPECT_NEAR(stamp.radiusWorld, expected, 0.01f);
    EXPECT_FALSE(stamp.fallback);
}

TEST(TressfxSurfaceBrush, CursorOffsetTiltsTheCameraRay)
{
    const auto roots = BuildSphereCloud();
    BrushSettings settings = BrushDefaults();
    BrushStamp left = SurfaceBrush().Sample(
        Eigen::Vector2f(200.0f, 500.0f), Viewport(), FrontCamera(), *roots,
        Eigen::Matrix4f::Identity(), settings);
    BrushStamp right = SurfaceBrush().Sample(
        Eigen::Vector2f(800.0f, 500.0f), Viewport(), FrontCamera(), *roots,
        Eigen::Matrix4f::Identity(), settings);
    ASSERT_TRUE(left.valid);
    ASSERT_TRUE(right.valid);
    EXPECT_LT(left.rayDirection.x(), 0.0f);
    EXPECT_GT(right.rayDirection.x(), 0.0f);
    EXPECT_LT(left.position.x(), right.position.x());
}

TEST(TressfxSurfaceBrush, DegenerateRootCloudFallsBackToViewDirection)
{
    // 两根共线根点既没有 PCA 法线，也不足以拟合位置平面：必须使用 -viewDir。
    std::vector<Eigen::Vector3f> positions = {
        Eigen::Vector3f(0.0f, 0.0f, 10.0f),
        Eigen::Vector3f(0.0f, 0.0f, 11.0f)};
    const auto roots = TressfxRootCloud::Build(positions,
        {Eigen::Vector3f::UnitY(), Eigen::Vector3f::UnitY()});
    BrushSettings settings = BrushDefaults();
    settings.radiusPixels = 100.0f;
    BrushStamp stamp = SurfaceBrush().Sample(
        Eigen::Vector2f(500.0f, 500.0f), Viewport(), FrontCamera(), *roots,
        Eigen::Matrix4f::Identity(), settings);
    ASSERT_TRUE(stamp.valid);
    EXPECT_TRUE(stamp.fallback);
    EXPECT_GT(stamp.normal.dot(-stamp.rayDirection), 0.999f);
}

TEST(TressfxRootCloudSelector, NormalGateRejectsBacksideRoots)
{
    const auto roots = BuildSphereCloud();
    BrushSettings settings = BrushDefaults();
    settings.planeThicknessWorld = 20.0f;
    BrushStamp stamp = FrontStamp();
    stamp.radiusWorld = 20.0f;
    const std::vector<RootHit> hits = RootCloudSelector().Select(
        *roots, stamp, Eigen::Matrix4f::Identity(), settings);
    ASSERT_FALSE(hits.empty());
    for (const RootHit& hit : hits)
    {
        const auto& root = roots->Roots()[hit.guideIndex];
        ASSERT_TRUE(root.hasNormal);
        EXPECT_GT(root.normal.z(), 0.5f) << "backside root " << hit.guideIndex;
    }
}

TEST(TressfxRootCloudSelector, ThinPlaneGateRejectsRootsAtOtherDepths)
{
    const auto roots = BuildSphereCloud();
    BrushSettings settings = BrushDefaults();
    settings.planeThicknessWorld = 1.0f;
    BrushStamp stamp = FrontStamp();
    stamp.radiusWorld = 20.0f;
    const std::vector<RootHit> hits = RootCloudSelector().Select(
        *roots, stamp, Eigen::Matrix4f::Identity(), settings);
    ASSERT_FALSE(hits.empty());
    for (const RootHit& hit : hits)
    {
        const auto& root = roots->Roots()[hit.guideIndex];
        EXPECT_NEAR(root.position.z(), 10.0f, 1.1f);
    }
}

TEST(TressfxRootCloudSelector, WeightFallsOffWithInPlaneDistance)
{
    const auto roots = BuildSphereCloud();
    BrushSettings settings = BrushDefaults();
    settings.planeThicknessWorld = 2.0f;
    BrushStamp stamp = FrontStamp();
    stamp.radiusWorld = 8.0f;
    const std::vector<RootHit> hits = RootCloudSelector().Select(
        *roots, stamp, Eigen::Matrix4f::Identity(), settings);
    ASSERT_GE(hits.size(), 2u);
    auto weightAt = [&](const RootHit& hit)
    {
        return hit.weight;
    };
    const RootHit* center = nullptr;
    const RootHit* edge = nullptr;
    for (const RootHit& hit : hits)
    {
        const auto& root = roots->Roots()[hit.guideIndex];
        const float distance = std::sqrt(root.position.x() * root.position.x() +
            root.position.y() * root.position.y());
        if (distance < 2.0f) center = &hit;
        if (distance > 5.0f) edge = &hit;
    }
    ASSERT_NE(center, nullptr);
    ASSERT_NE(edge, nullptr);
    EXPECT_GT(weightAt(*center), weightAt(*edge));
}

TEST(TressfxRootCloudSelector, GrowthDirectionDoesNotChangeOrdinaryGrouping)
{
    const auto yGrowth = BuildSphereCloud(900, 10.0f, Eigen::Vector3f::UnitY());
    const auto xGrowth = BuildSphereCloud(900, 10.0f, Eigen::Vector3f::UnitX());
    BrushSettings settings = BrushDefaults();
    settings.planeThicknessWorld = 2.0f;
    BrushStamp stamp = FrontStamp();
    stamp.radiusWorld = 8.0f;
    const std::vector<RootHit> left = RootCloudSelector().Select(
        *yGrowth, stamp, Eigen::Matrix4f::Identity(), settings);
    const std::vector<RootHit> right = RootCloudSelector().Select(
        *xGrowth, stamp, Eigen::Matrix4f::Identity(), settings);
    ASSERT_EQ(left.size(), right.size());
    for (size_t index = 0; index < left.size(); ++index)
    {
        EXPECT_EQ(left[index].guideIndex, right[index].guideIndex);
        EXPECT_FLOAT_EQ(left[index].weight, right[index].weight);
    }
}

TEST(TressfxGroupPaint, AddRemoveReplaceAndConflictCounts)
{
    GroupPaintController controller;
    controller.BeginSession(8, {0, 0, 1, 0, 0, 0, 0, 0}, {"alpha", "beta"});
    controller.BeginStroke(2, GroupPaintMode::Add, false);
    controller.AddHits({{2, 1.0f}, {5, 0.7f}});
    auto commit = controller.CommitStroke();
    ASSERT_TRUE(commit.has_value());
    EXPECT_EQ(commit->addGuides, (std::vector<uint32_t>{5}));
    ASSERT_EQ(commit->conflicts.size(), 1u);
    EXPECT_EQ(commit->conflicts[0].groupName, "alpha");
    EXPECT_EQ(commit->conflicts[0].guideCount, 1u);
    EXPECT_EQ(controller.GuideGroupIds()[5], 2u);
    EXPECT_EQ(controller.GuideGroupIds()[2], 1u);

    controller.BeginStroke(2, GroupPaintMode::Add, true);
    controller.AddHits({{2, 1.0f}});
    commit = controller.CommitStroke();
    ASSERT_TRUE(commit.has_value());
    EXPECT_EQ(controller.GuideGroupIds()[2], 2u);
    EXPECT_EQ(commit->emptiedGroupNames, (std::vector<std::string>{"alpha"}));

    controller.BeginStroke(2, GroupPaintMode::Remove, false);
    controller.AddHits({{2, 1.0f}, {5, 1.0f}});
    commit = controller.CommitStroke();
    ASSERT_TRUE(commit.has_value());
    EXPECT_EQ(commit->removeGuides, (std::vector<uint32_t>{2, 5}));
    EXPECT_EQ(controller.GuideGroupIds()[2], 0u);
    EXPECT_EQ(controller.GuideGroupIds()[5], 0u);
    EXPECT_EQ(commit->emptiedGroupNames, (std::vector<std::string>{"beta"}));
}

TEST(TressfxGroupPaint, ConsecutiveGuidesCompressAndCancelDoesNotMutate)
{
    GroupPaintController controller;
    controller.BeginSession(10, {}, {"target"});
    controller.BeginStroke(1, GroupPaintMode::Add, false);
    controller.AddHits({{2, 1.0f}, {3, 0.8f}, {4, 0.6f}, {8, 0.5f}});
    auto commit = controller.CommitStroke();
    ASSERT_TRUE(commit.has_value());
    EXPECT_EQ(commit->addRanges,
        (std::vector<std::pair<uint32_t, uint32_t>>{{2, 5}, {8, 9}}));

    const auto before = controller.GuideGroupIds();
    controller.BeginStroke(1, GroupPaintMode::Remove, false);
    controller.AddHits({{2, 1.0f}});
    controller.CancelStroke();
    EXPECT_EQ(controller.GuideGroupIds(), before);
    EXPECT_TRUE(controller.CanUndo());
}

TEST(TressfxGroupPaint, UndoRestoresThePreviousBrushCommit)
{
    GroupPaintController controller;
    controller.BeginSession(6, {}, {"target"});
    controller.BeginStroke(1, GroupPaintMode::Add, false);
    controller.AddHits({{1, 1.0f}, {2, 1.0f}});
    ASSERT_TRUE(controller.CommitStroke().has_value());
    EXPECT_EQ(controller.GuideGroupIds()[1], 1u);
    ASSERT_TRUE(controller.CanUndo());
    ASSERT_TRUE(controller.UndoLastStroke());
    EXPECT_EQ(controller.GuideGroupIds()[1], 0u);
    EXPECT_EQ(controller.GuideGroupIds()[2], 0u);
}

TEST(TressfxSurfaceBrush, FortyThousandRootsAreReachableFromTheFront)
{
    // 回归：AABB 覆盖整份点云时不能在射线/法线过滤前截断候选，否则 40k 资产
    // 只会选到网格顺序最前面的背面根点。
    const auto roots = FortyThousandCloud();
    const auto world = Eigen::Matrix4f::Identity();
    const auto candidates = roots->QueryRay(
        Eigen::Vector3f(0.0f, 0.0f, 100.0f), Eigen::Vector3f(0.0f, 0.0f, -1.0f),
        1000.0f, 1.5f, world, 256);
    ASSERT_FALSE(candidates.empty());
    bool sawFront = false;
    for (uint32_t index : candidates)
        if (roots->Roots().at(index).position.z() > 0.0f) sawFront = true;
    EXPECT_TRUE(sawFront);

    BrushSettings settings = BrushDefaults();
    settings.radiusPixels = 40.0f;
    const BrushStamp stamp = SurfaceBrush().Sample(
        Eigen::Vector2f(500.0f, 500.0f), Viewport(), FrontCamera(), *roots, world, settings);
    ASSERT_TRUE(stamp.valid);
    EXPECT_GT(stamp.position.z(), 0.0f) << "brush must land on the front hemisphere";
    const auto hits = RootCloudSelector().Select(*roots, stamp, world, settings);
    EXPECT_FALSE(hits.empty());
}

TEST(TressfxRootCloudDda, TraversesFrontAndBackWithoutScanningTheWholeCloud)
{
    const auto roots = FortyThousandCloud();
    const auto candidates = roots->QueryRay(
        Eigen::Vector3f(0.0f, 0.0f, 100.0f), -Eigen::Vector3f::UnitZ(),
        1000.0f, 0.5f, Eigen::Matrix4f::Identity(), 0);
    ASSERT_FALSE(candidates.empty());
    // ray tube 只覆盖轴附近的一小束，不能退化成整份点云。
    EXPECT_LT(candidates.size(), roots->RootCount() / 4);

    bool sawFront = false;
    bool sawBack = false;
    float previousDepth = -1.0f;
    for (uint32_t index : candidates)
    {
        const Eigen::Vector3f& position = roots->Roots()[index].position;
        EXPECT_LE(std::sqrt(position.x() * position.x() + position.y() * position.y()),
            0.5f + 1.0e-3f);
        const float depth = 100.0f - position.z();
        EXPECT_GE(depth, previousDepth - 1.0e-4f);
        previousDepth = depth;
        if (position.z() > 5.0f) sawFront = true;
        if (position.z() < -5.0f) sawBack = true;
    }
    EXPECT_TRUE(sawFront);
    EXPECT_TRUE(sawBack);
}

TEST(TressfxRootCloudDda, EmptyRegionAndShortDistanceReturnNothing)
{
    const auto roots = BuildSphereCloud(900, 10.0f);
    const auto world = Eigen::Matrix4f::Identity();
    EXPECT_TRUE(roots->QueryRay(Eigen::Vector3f(0.0f, 1000.0f, 100.0f),
        -Eigen::Vector3f::UnitY(), 1000.0f, 1.0f, world, 0).empty());
    // 球面最前点 z=10；在 z≈11 停下的射线不应进入点云。
    EXPECT_TRUE(roots->QueryRay(Eigen::Vector3f(0.0f, 0.0f, 100.0f),
        -Eigen::Vector3f::UnitZ(), 89.0f, 0.5f, world, 0).empty());
    EXPECT_TRUE(roots->QueryRay(Eigen::Vector3f(0.0f, 0.0f, 100.0f),
        Eigen::Vector3f::UnitZ(), 1000.0f, 1.0f, world, 0).empty());
}

TEST(TressfxRootCloudDda, ZeroRadiusKeepsOnlyRootsExactlyOnTheRay)
{
    const std::vector<Eigen::Vector3f> positions = {
        Eigen::Vector3f(0.0f, 0.0f, 0.0f),
        Eigen::Vector3f(0.0f, 0.0f, 10.0f),
        Eigen::Vector3f(3.0f, 0.0f, 5.0f),
        Eigen::Vector3f(0.0f, 4.0f, 5.0f)};
    const auto roots = TressfxRootCloud::Build(positions,
        std::vector<Eigen::Vector3f>(positions.size(), Eigen::Vector3f::UnitY()));
    const auto candidates = roots->QueryRay(
        Eigen::Vector3f(0.0f, 0.0f, 100.0f), -Eigen::Vector3f::UnitZ(),
        1000.0f, 0.0f, Eigen::Matrix4f::Identity(), 0);
    ASSERT_EQ(candidates.size(), 2u);
    for (uint32_t index : candidates)
    {
        EXPECT_NEAR(roots->Roots()[index].position.x(), 0.0f, 1.0e-5f);
        EXPECT_NEAR(roots->Roots()[index].position.y(), 0.0f, 1.0e-5f);
    }
}

TEST(TressfxRootCloudDda, CandidateBudgetKeepsTheNearestRoots)
{
    const auto roots = BuildSphereCloud(900, 10.0f);
    const auto world = Eigen::Matrix4f::Identity();
    const auto all = roots->QueryRay(Eigen::Vector3f(0.0f, 0.0f, 100.0f),
        -Eigen::Vector3f::UnitZ(), 1000.0f, 3.0f, world, 0);
    ASSERT_GE(all.size(), 11u);
    const auto limited = roots->QueryRay(Eigen::Vector3f(0.0f, 0.0f, 100.0f),
        -Eigen::Vector3f::UnitZ(), 1000.0f, 3.0f, world, 10);
    ASSERT_EQ(limited.size(), 10u);
    EXPECT_TRUE(std::equal(limited.begin(), limited.end(), all.begin()));
}

TEST(TressfxRootCloudDda, MatchesBruteForceForRandomObliqueRays)
{
    const auto roots = BuildSphereCloud(900, 10.0f);
    uint32_t state = 0x12345678u;
    const auto nextRandom = [&state]()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    size_t hits = 0;
    for (int iteration = 0; iteration < 400; ++iteration)
    {
        // 显式按 x/y/z 顺序取随机数：函数参数求值顺序未指定，不能依赖。
        const float originX = nextRandom() * 80.0f - 40.0f;
        const float originY = nextRandom() * 80.0f - 40.0f;
        const float originZ = nextRandom() * 80.0f - 40.0f;
        const float targetX = nextRandom() * 16.0f - 8.0f;
        const float targetY = nextRandom() * 16.0f - 8.0f;
        const float targetZ = nextRandom() * 16.0f - 8.0f;
        const Eigen::Vector3f origin(originX, originY, originZ);
        const Eigen::Vector3f target(targetX, targetY, targetZ);
        const Eigen::Vector3f direction = (target - origin).normalized();
        const float radius = 0.2f + nextRandom() * 2.4f;
        const auto expected = BruteForceRay(*roots, origin, direction, 1000.0f, radius);
        const auto actual = roots->QueryRay(origin, direction, 1000.0f, radius,
            Eigen::Matrix4f::Identity(), 0);
        ASSERT_EQ(GuideSet(actual), GuideSet(expected)) << "oblique ray " << iteration;
        if (!actual.empty()) ++hits;
    }
    EXPECT_GT(hits, 60u);
}

TEST(TressfxRootCloudDda, MatchesBruteForceOnFortyThousandRoots)
{
    const auto roots = FortyThousandCloud();
    uint32_t state = 0x9e3779b9u;
    const auto nextRandom = [&state]()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    size_t hits = 0;
    for (int iteration = 0; iteration < 24; ++iteration)
    {
        const float originX = nextRandom() * 80.0f - 40.0f;
        const float originY = nextRandom() * 80.0f - 40.0f;
        const float originZ = nextRandom() * 80.0f - 40.0f;
        const float targetX = nextRandom() * 16.0f - 8.0f;
        const float targetY = nextRandom() * 16.0f - 8.0f;
        const float targetZ = nextRandom() * 16.0f - 8.0f;
        const Eigen::Vector3f origin(originX, originY, originZ);
        const Eigen::Vector3f target(targetX, targetY, targetZ);
        const Eigen::Vector3f direction = (target - origin).normalized();
        // cellSize≈1.15，半径覆盖 0.5~4.5 cell，专门压主轴多 slice 扩展。
        const float radius = 0.4f + nextRandom() * 4.1f;
        const auto expected = BruteForceRay(*roots, origin, direction, 1000.0f, radius);
        const auto actual = roots->QueryRay(origin, direction, 1000.0f, radius,
            Eigen::Matrix4f::Identity(), 0);
        ASSERT_EQ(GuideSet(actual), GuideSet(expected)) << "40k oblique ray " << iteration;
        if (!actual.empty()) ++hits;
    }
    EXPECT_GT(hits, 8u);
}

TEST(TressfxRootCloudDda, ObliqueRayNearCellCornerKeepsAllTubeRoots)
{
    const auto roots = BuildSphereCloud(900, 10.0f);
    const Eigen::Vector3f origin(38.97001266479492f, 36.23198986053467f,
        32.68022537231445f);
    const Eigen::Vector3f target(5.654043197631836f, -2.2760791778564453f,
        4.004482269287109f);
    const Eigen::Vector3f direction = (target - origin).normalized();
    const float radius = 2.0148251056671143f;
    const auto actual = roots->QueryRay(origin, direction, 1000.0f, radius,
        Eigen::Matrix4f::Identity(), 0);
    bool has537 = false;
    for (uint32_t index : actual) has537 |= index == 537u;
    EXPECT_TRUE(has537);
}

TEST(TressfxRootCloudDda, UniformLocalToWorldMatchesTheLocalQuery)
{
    const auto roots = BuildSphereCloud(900, 10.0f);
    const Eigen::Vector3f localOrigin(20.0f, 4.0f, 30.0f);
    const Eigen::Vector3f localTarget(-1.0f, 2.0f, 0.0f);
    const Eigen::Vector3f localDirection = (localTarget - localOrigin).normalized();
    const float localRadius = 1.7f;
    const auto local = roots->QueryRay(localOrigin, localDirection, 1000.0f,
        localRadius, Eigen::Matrix4f::Identity(), 0);

    Eigen::Matrix4f localToWorld = Eigen::Matrix4f::Identity();
    localToWorld.block<3, 3>(0, 0) *= 2.0f;
    localToWorld.block<3, 1>(0, 3) = Eigen::Vector3f(5.0f, -3.0f, 2.0f);
    const Eigen::Vector3f worldOrigin = (localToWorld * Eigen::Vector4f(
        localOrigin.x(), localOrigin.y(), localOrigin.z(), 1.0f)).head<3>();
    const Eigen::Vector3f worldDirection = (localToWorld.block<3, 3>(0, 0) *
        localDirection).normalized();
    const auto world = roots->QueryRay(worldOrigin, worldDirection, 2000.0f,
        localRadius * 2.0f, localToWorld, 0);
    EXPECT_EQ(GuideSet(world), GuideSet(local));
}

TEST(TressfxGroupPaint, SharedBindingSessionStaysReadOnlyUntilCommit)
{
    auto binding = std::make_shared<TressfxGroupBinding>();
    binding->guideGroupIds.assign(40334, 0u);
    binding->guideGroupIds[7] = 1u;
    binding->groupNames = {"alpha", "beta"};
    binding->key = "phase3-shared";
    const uint32_t* sourceIds = binding->guideGroupIds.data();

    GroupPaintController controller;
    controller.BeginSession(binding);
    ASSERT_EQ(controller.GuideCount(), 40334u);
    EXPECT_EQ(controller.GuideGroupIds().data(), sourceIds);

    // hover/AnalyzeHits 在多帧重复调用时不得复制或改写共享映射。
    for (int iteration = 0; iteration < 64; ++iteration)
    {
        const auto summary = controller.AnalyzeHits({{7, 1.0f}, {8, 1.0f}},
            2, GroupPaintMode::Add, false);
        EXPECT_EQ(summary.selectedGuides, 2u);
        EXPECT_EQ(summary.eligibleGuides, 1u);
        ASSERT_EQ(summary.conflicts.size(), 1u);
    }
    EXPECT_EQ(controller.GuideGroupIds().data(), sourceIds);
    EXPECT_EQ(binding->guideGroupIds[8], 0u);

    controller.BeginStroke(2, GroupPaintMode::Add, false);
    controller.AddHits({{7, 1.0f}, {8, 1.0f}});
    EXPECT_EQ(binding->guideGroupIds[8], 0u);
    const auto commit = controller.CommitStroke();
    ASSERT_TRUE(commit.has_value());
    EXPECT_EQ(commit->addGuides, (std::vector<uint32_t>{8}));
    EXPECT_EQ(controller.GuideGroupIds()[8], 2u);
    EXPECT_EQ(binding->guideGroupIds[8], 0u);
    EXPECT_NE(controller.GuideGroupIds().data(), sourceIds);

    ASSERT_TRUE(controller.UndoLastStroke());
    EXPECT_EQ(controller.GuideGroupIds()[8], 0u);
}

TEST(TressfxGroupPaint, OneSessionSurvivesMultipleStrokes)
{
    auto binding = std::make_shared<TressfxGroupBinding>();
    binding->guideGroupIds.assign(64, 0u);
    binding->groupNames = {"target"};
    binding->key = "multi-stroke";

    GroupPaintController controller;
    controller.BeginSession(binding);
    for (uint32_t guide = 0; guide < 8; ++guide)
    {
        controller.BeginStroke(1, GroupPaintMode::Add, false);
        controller.AddHits({{guide, 1.0f}});
        ASSERT_TRUE(controller.CommitStroke().has_value());
    }
    for (uint32_t guide = 0; guide < 8; ++guide)
        EXPECT_EQ(controller.GuideGroupIds()[guide], 1u);
    EXPECT_EQ(binding->guideGroupIds[0], 0u);
    EXPECT_TRUE(controller.CanUndo());

    for (uint32_t guide = 0; guide < 8; ++guide)
        ASSERT_TRUE(controller.UndoLastStroke());
    for (uint32_t guide = 0; guide < 8; ++guide)
        EXPECT_EQ(controller.GuideGroupIds()[guide], 0u);
}

TEST(TressfxSurfaceBrush, FortyThousandRootHoverLoopStaysWithinBudget)
{
    const auto roots = FortyThousandCloud();
    auto binding = std::make_shared<TressfxGroupBinding>();
    binding->guideGroupIds.assign(40334, 0u);
    for (size_t guide = 0; guide < 40334; guide += 5)
        binding->guideGroupIds[guide] = 1u;
    binding->groupNames = {"paint"};
    GroupPaintController controller;
    controller.BeginSession(binding);

    BrushSettings settings = BrushDefaults();
    settings.radiusPixels = 30.0f;
    const SurfaceBrush brush;
    const RootCloudSelector selector;
    const auto begin = std::chrono::steady_clock::now();
    uint32_t selected = 0;
    for (int iteration = 0; iteration < 200; ++iteration)
    {
        const Eigen::Vector2f cursor(
            500.0f + 70.0f * std::sin(static_cast<float>(iteration) * 0.11f),
            500.0f + 50.0f * std::cos(static_cast<float>(iteration) * 0.09f));
        const BrushStamp stamp = brush.Sample(cursor, Viewport(), FrontCamera(),
            *roots, Eigen::Matrix4f::Identity(), settings);
        const auto hits = selector.Select(*roots, stamp,
            Eigen::Matrix4f::Identity(), settings);
        const auto summary = controller.AnalyzeHits(hits, 1,
            GroupPaintMode::Add, false);
        selected += summary.selectedGuides;
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    EXPECT_GT(selected, 0u);
    // 宽松上限，仅防止退回“每次 hover 全云扫描 + 复制 40k 映射”。
    EXPECT_LT(elapsed, 2.0);
}

TEST(TressfxRootCloudSelector, LassoSelectsOnlyProjectedFrontRootsInsidePolygon)
{
    const auto roots = BuildSphereCloud(400);
    const BrushStamp stamp = FrontStamp();
    const std::vector<Eigen::Vector2f> inside = {
        {300.0f, 300.0f}, {700.0f, 300.0f}, {700.0f, 700.0f}, {300.0f, 700.0f}};
    const std::vector<Eigen::Vector2f> outside = {
        {780.0f, 300.0f}, {980.0f, 300.0f}, {980.0f, 700.0f}, {780.0f, 700.0f}};

    const RootCloudSelector selector;
    const auto hits = selector.SelectLasso(*roots, stamp, Eigen::Matrix4f::Identity(),
        Viewport(), FrontCamera(), inside, BrushDefaults());
    ASSERT_FALSE(hits.empty());
    for (const RootHit& hit : hits)
    {
        // 背面发根即使屏幕投影在套索内，也必须被 normalGate 剔除。
        const Eigen::Vector3f& position = roots->Roots()[hit.guideIndex].position;
        EXPECT_GT(position.z(), 0.0f);
    }
    EXPECT_TRUE(selector.SelectLasso(*roots, stamp, Eigen::Matrix4f::Identity(),
        Viewport(), FrontCamera(), outside, BrushDefaults()).empty());
}

TEST(TressfxRootCloudSelector, LassoUsesTheInclinedBrushPlane)
{
    const auto roots = BuildSphereCloud(400);
    BrushStamp stamp = FrontStamp();
    // 倾斜的 brush normal：发根必须先投到该平面，再判断是否在多边形内/薄层内。
    stamp.normal = Eigen::Vector3f(0.0f, 0.6f, 0.8f).normalized();
    const std::vector<Eigen::Vector2f> polygon = {
        {0.0f, 0.0f}, {1000.0f, 0.0f}, {1000.0f, 1000.0f}, {0.0f, 1000.0f}};

    const auto hits = RootCloudSelector().SelectLasso(*roots, stamp,
        Eigen::Matrix4f::Identity(), Viewport(), FrontCamera(), polygon, BrushDefaults());
    ASSERT_FALSE(hits.empty());
    const float thickness = std::max(roots->CellSize() * 2.0f, stamp.radiusWorld * 0.15f);
    for (const RootHit& hit : hits)
    {
        const Eigen::Vector3f& position = roots->Roots()[hit.guideIndex].position;
        const float planeDistance =
            std::abs((position - stamp.position).dot(stamp.normal));
        EXPECT_LE(planeDistance, thickness + 1.0e-4f);
    }
}

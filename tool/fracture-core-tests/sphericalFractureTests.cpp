#include <gtest/gtest.h>

#include <cmath>

#include "physics/fracture/sphericalFracture.h"

namespace
{
using namespace VL::Physics::Fracture;

constexpr double Pi = 3.14159265358979323846;

SphericalFractureSettings Settings()
{
    SphericalFractureSettings settings;
    settings.seed = 17;
    settings.siteCount = 32;
    settings.radius = 20.0;
    settings.thickness = 0.2;
    settings.impactDirection = Eigen::Vector3d(0.3, -0.5, 0.8);
    return settings;
}

double TotalArea(const SphericalPattern& pattern)
{
    double area = 0.0;
    for (const SphericalShard& shard : pattern.shards) area += shard.area;
    return area;
}

} // namespace

TEST(SphericalFracture, RuntimePatternCoversTheWholeSphere)
{
    const SphericalFractureSettings settings = Settings();
    const SphericalPattern pattern = GenerateSphericalBurstPattern(settings);
    ASSERT_GE(pattern.shards.size(), 8u);
    const double expected = 4.0 * Pi * settings.radius * settings.radius;
    EXPECT_NEAR(TotalArea(pattern), expected, expected * 1e-6);
    EXPECT_NEAR(pattern.coveredArea, TotalArea(pattern), 1e-9);
    for (const SphericalShard& shard : pattern.shards)
    {
        ASSERT_GE(shard.boundary.size(), 3u);
        EXPECT_GT(shard.area, 0.0);
        EXPECT_NEAR(shard.siteDirection.norm(), 1.0, 1e-12);
        for (const Eigen::Vector3d& point : shard.boundary)
            EXPECT_NEAR(point.norm(), 1.0, 1e-9);
    }
}

TEST(SphericalFracture, HitDirectionCellIsACapAroundTheImpact)
{
    SphericalFractureSettings settings = Settings();
    settings.impactBias = 0.88;
    const Eigen::Vector3d impact = settings.impactDirection.normalized();
    const SphericalPattern pattern = GenerateSphericalBurstPattern(settings);
    const SphericalShard* hitShard = nullptr;
    for (const SphericalShard& shard : pattern.shards)
    {
        if (shard.siteDirection.dot(impact) > 0.999999)
        {
            hitShard = &shard;
            break;
        }
    }
    ASSERT_NE(hitShard, nullptr);
    // 破口 cell 必须是命中点周围的一个小帽，而不是绕到球背面的长条：
    // 这是“裂纹从命中点开始”的拓扑前提，箭射到哪、破口就应该在哪。
    const double cosineLimit = std::cos(50.0 * Pi / 180.0);
    for (const Eigen::Vector3d& direction : hitShard->boundary)
        EXPECT_GE(direction.dot(impact), cosineLimit);
}

TEST(SphericalFracture, SameSeedProducesIdenticalRuntimeTopology)
{
    const SphericalFractureSettings settings = Settings();
    const SphericalPattern first = GenerateSphericalBurstPattern(settings);
    const SphericalPattern second = GenerateSphericalBurstPattern(settings);
    ASSERT_EQ(first.shards.size(), second.shards.size());
    for (size_t index = 0; index < first.shards.size(); ++index)
    {
        ASSERT_EQ(first.shards[index].boundary.size(),
            second.shards[index].boundary.size());
        for (size_t point = 0; point < first.shards[index].boundary.size(); ++point)
            EXPECT_TRUE(first.shards[index].boundary[point].isApprox(
                second.shards[index].boundary[point], 1e-12));
    }
}

TEST(SphericalFracture, DifferentSeedChangesRuntimeTopology)
{
    SphericalFractureSettings firstSettings = Settings();
    SphericalFractureSettings secondSettings = firstSettings;
    secondSettings.seed += 1;
    const SphericalPattern first = GenerateSphericalBurstPattern(firstSettings);
    const SphericalPattern second = GenerateSphericalBurstPattern(secondSettings);
    bool different = first.shards.size() != second.shards.size();
    if (!different)
        for (size_t index = 0; index < first.shards.size(); ++index)
            if (first.shards[index].boundary.size() != second.shards[index].boundary.size() ||
                !first.shards[index].siteDirection.isApprox(
                    second.shards[index].siteDirection, 1e-9))
            {
                different = true;
                break;
            }
    EXPECT_TRUE(different);
}

TEST(SphericalFracture, ExtrusionBuildsClosedOutwardShell)
{
    const SphericalFractureSettings settings = Settings();
    const SphericalPattern pattern = GenerateSphericalBurstPattern(settings);
    ASSERT_FALSE(pattern.shards.empty());
    // 用最小的球面片做体积检查：大片的平面扇形近似误差会随角宽增长，
    // 那属于已知的离散误差，不是挤出绕序的问题。
    const SphericalShard* smallest = &pattern.shards.front();
    for (const SphericalShard& candidate : pattern.shards)
        if (candidate.area < smallest->area) smallest = &candidate;
    const SphericalShard& shard = *smallest;
    const FragmentMesh mesh = ExtrudeSphericalShard(
        shard, settings.radius, settings.thickness);

    EXPECT_EQ(mesh.indices.size() % 3, 0u);
    EXPECT_EQ(mesh.positions.size(), mesh.normals.size());
    EXPECT_GE(mesh.convexHullPoints.size(), 8u);
    const double innerRadius = settings.radius - settings.thickness;
    const double expectedVolume = shard.area / (settings.radius * settings.radius) / 3.0 *
        (settings.radius * settings.radius * settings.radius -
            innerRadius * innerRadius * innerRadius);
    EXPECT_NEAR(mesh.volume, expectedVolume, 1e-9);
    for (const Eigen::Vector3f& normal : mesh.normals)
        EXPECT_NEAR(normal.norm(), 1.0f, 1e-5f);

    // 外表面朝外、内表面朝内。球面片用平面扇形近似，网格有向体积会略小于
    // 解析薄壳体积；质量计算使用 mesh.volume，碰撞形状使用凸包近似。
    const double signedVolume = SignedMeshVolume(mesh.positions, mesh.indices);
    EXPECT_GT(signedVolume, 0.0);
    EXPECT_NEAR(signedVolume, mesh.volume, mesh.volume * 0.10);
}

TEST(SphericalFracture, InvalidSettingsAreRejectedAtTheBoundary)
{
    SphericalFractureSettings settings = Settings();
    settings.impactDirection.setZero();
    EXPECT_THROW(GenerateSphericalBurstPattern(settings), std::invalid_argument);

    settings = Settings();
    settings.thickness = settings.radius;
    EXPECT_THROW(GenerateSphericalBurstPattern(settings), std::invalid_argument);

    settings = Settings();
    settings.siteCount = 3;
    const SphericalPattern pattern = GenerateSphericalBurstPattern(settings);
    EXPECT_GE(pattern.shards.size(), 4u);
}

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "physics/runtime/fractureImpactWorld.h"

namespace
{
using namespace VL::Physics;

std::vector<Eigen::Vector3f> CubePoints(float halfExtent)
{
    std::vector<Eigen::Vector3f> points;
    for (float x : {-halfExtent, halfExtent})
        for (float y : {-halfExtent, halfExtent})
            for (float z : {-halfExtent, halfExtent})
                points.emplace_back(x, y, z);
    return points;
}
} // namespace

TEST(FractureImpactWorld, ProjectileContactReportsNamedSphere)
{
    FractureImpactWorld world;
    world.RegisterSphereProxy("RubberBalloon", Eigen::Vector3f::Zero(), 50.0f);
    world.LaunchProjectile(
        Eigen::Vector3f(0.0f, 0.0f, 60.0f),
        Eigen::Vector3f(0.0f, 0.0f, -1200.0f),
        5.0f, 1.0f);

    std::vector<FractureImpact> impacts;
    for (int step = 0; step < 60 && impacts.empty(); ++step)
        impacts = world.Step(1.0f / 60.0f);

    ASSERT_FALSE(impacts.empty());
    EXPECT_EQ(impacts.front().type, FractureImpactType::BalloonHit);
    EXPECT_EQ(impacts.front().materialProfile,
        FractureMaterialProfile::Balloon);
    EXPECT_EQ(impacts.front().objectName, "RubberBalloon");
    EXPECT_GT(impacts.front().relativeSpeed, 0.0f);
    EXPECT_TRUE(impacts.front().point.allFinite());
    EXPECT_TRUE(impacts.front().normal.allFinite());
}

TEST(FractureImpactWorld, ProjectileContactReportsNamedPane)
{
    FractureImpactWorld world;
    world.RegisterPane("WindowGlass", Eigen::Vector3f::Zero(),
        Eigen::Quaternionf::Identity(), Eigen::Vector3f(100.0f, 60.0f, 0.5f));
    world.LaunchProjectile(
        Eigen::Vector3f(10.0f, 5.0f, 80.0f),
        Eigen::Vector3f(0.0f, 0.0f, -900.0f),
        4.0f, 0.5f);

    std::vector<FractureImpact> impacts;
    for (int step = 0; step < 60 && impacts.empty(); ++step)
        impacts = world.Step(1.0f / 60.0f);

    ASSERT_FALSE(impacts.empty());
    EXPECT_EQ(impacts.front().type, FractureImpactType::PaneHit);
    EXPECT_EQ(impacts.front().materialProfile,
        FractureMaterialProfile::Glass);
    EXPECT_EQ(impacts.front().objectName, "WindowGlass");
    EXPECT_NEAR(impacts.front().point.z(), 0.5, 2.0);
}

TEST(FractureImpactWorld, ClearRemovesProxiesAndProjectiles)
{
    FractureImpactWorld world;
    world.RegisterPane("pane", Eigen::Vector3f::Zero(),
        Eigen::Quaternionf::Identity(), Eigen::Vector3f(50.0f, 50.0f, 1.0f));
    world.LaunchProjectile(
        Eigen::Vector3f(0.0f, 0.0f, 60.0f),
        Eigen::Vector3f(0.0f, 0.0f, -100.0f),
        2.0f, 1.0f);
    world.Clear();
    for (int step = 0; step < 10; ++step)
        EXPECT_TRUE(world.Step(1.0f / 60.0f).empty());
}

TEST(FractureImpactWorld, ShardContactWithGroundReportsLanding)
{
    FractureImpactWorld world;
    world.SetGroundPlane(-100.0f);
    world.RegisterShard("WindowGlass", "WindowGlass~shard0",
        Eigen::Vector3f(0.0f, -90.0f, 0.0f), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f(0.0f, -500.0f, 0.0f), 0.2f, CubePoints(5.0f),
        FractureMaterialProfile::Glass);

    std::vector<FractureImpact> impacts;
    for (int step = 0; step < 60 && impacts.empty(); ++step)
        impacts = world.Step(1.0f / 60.0f);

    ASSERT_FALSE(impacts.empty());
    EXPECT_EQ(impacts.front().type, FractureImpactType::ShardLanding);
    EXPECT_EQ(impacts.front().materialProfile,
        FractureMaterialProfile::Glass);
    EXPECT_EQ(impacts.front().objectName, "WindowGlass");
    EXPECT_EQ(impacts.front().shardName, "WindowGlass~shard0");
}

TEST(FractureImpactWorld, BalloonShardLandingKeepsBalloonProfile)
{
    FractureImpactWorld world;
    world.SetGroundPlane(-100.0f);
    world.RegisterShard("RubberBalloon", "RubberBalloon~shard0",
        Eigen::Vector3f(0.0f, -90.0f, 0.0f), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f(0.0f, -500.0f, 0.0f), 0.2f, CubePoints(5.0f),
        FractureMaterialProfile::Balloon);

    std::vector<FractureImpact> impacts;
    for (int step = 0; step < 60 && impacts.empty(); ++step)
        impacts = world.Step(1.0f / 60.0f);

    ASSERT_FALSE(impacts.empty());
    EXPECT_EQ(impacts.front().type, FractureImpactType::ShardLanding);
    EXPECT_EQ(impacts.front().materialProfile,
        FractureMaterialProfile::Balloon);
    EXPECT_EQ(impacts.front().objectName, "RubberBalloon");
}

TEST(FractureImpactWorld, ConvexShardPairResolvesMutualContact)
{
    FractureImpactWorld world;
    world.RegisterShard("WindowGlass", "shardA",
        Eigen::Vector3f(-20.0f, 0.0f, 0.0f), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f(120.0f, 0.0f, 0.0f), 0.2f, CubePoints(5.0f),
        FractureMaterialProfile::Glass);
    world.RegisterShard("WindowGlass", "shardB",
        Eigen::Vector3f(20.0f, 0.0f, 0.0f), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f(-120.0f, 0.0f, 0.0f), 0.2f, CubePoints(5.0f),
        FractureMaterialProfile::Glass);

    std::vector<FractureImpact> impacts;
    for (int step = 0; step < 60 && impacts.empty(); ++step)
        impacts = world.Step(1.0f / 60.0f);

    ASSERT_FALSE(impacts.empty());
    EXPECT_EQ(impacts.front().type, FractureImpactType::ShardHit);
    EXPECT_GE(impacts.front().relativeSpeed, 0.0f);
}

TEST(FractureImpactWorld, ShardAngularVelocityRotatesTheBody)
{
    FractureImpactWorld world;
    world.RegisterShard("balloon", "balloon~spin",
        Eigen::Vector3f(0.0f, 200.0f, 0.0f), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f::Zero(), 0.2f, CubePoints(5.0f),
        FractureMaterialProfile::Balloon,
        Eigen::Vector3f(0.0f, 8.0f, 0.0f));
    for (int step = 0; step < 10; ++step) world.Step(1.0f / 60.0f);

    const auto pose = world.GetShardPose("balloon", "balloon~spin");
    ASSERT_TRUE(pose.has_value());
    // 8 rad/s 持续 1/6 秒约 76°，四元数 w 分量应明显偏离单位旋转。
    EXPECT_LT(std::abs(pose->rotation.w()), 0.95f);
}

TEST(FractureImpactWorld, ProjectileHitsStaticAttachedPlate)
{
    FractureImpactWorld world;
    world.RegisterShard("WindowGlass", "WindowGlass~frag0",
        Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f::Zero(), 0.2f, CubePoints(10.0f),
        FractureMaterialProfile::Glass,
        Eigen::Vector3f::Zero(), false);
    world.LaunchProjectile(
        Eigen::Vector3f(0.0f, 0.0f, 80.0f),
        Eigen::Vector3f(0.0f, 0.0f, -900.0f),
        2.0f, 0.1f);

    std::vector<FractureImpact> impacts;
    for (int step = 0; step < 60 && impacts.empty(); ++step)
        impacts = world.Step(1.0f / 60.0f);

    ASSERT_FALSE(impacts.empty());
    EXPECT_EQ(impacts.front().type, FractureImpactType::AttachedHit);
    EXPECT_EQ(impacts.front().materialProfile,
        FractureMaterialProfile::Glass);
    EXPECT_EQ(impacts.front().objectName, "WindowGlass");
    EXPECT_EQ(impacts.front().shardName, "WindowGlass~frag0");
}

TEST(FractureImpactWorld, ProjectileHitsDynamicShardReportsShardHit)
{
    FractureImpactWorld world;
    world.RegisterShard("WindowGlass", "WindowGlass~groundShard",
        Eigen::Vector3f::Zero(), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f::Zero(), 0.2f, CubePoints(10.0f),
        FractureMaterialProfile::Glass);
    const uint32_t id = world.LaunchProjectile(
        Eigen::Vector3f(0.0f, 0.0f, 80.0f),
        Eigen::Vector3f(0.0f, 0.0f, -900.0f),
        2.0f, 0.1f);

    std::vector<FractureImpact> impacts;
    for (int step = 0; step < 60 && impacts.empty(); ++step)
        impacts = world.Step(1.0f / 60.0f);

    ASSERT_FALSE(impacts.empty());
    EXPECT_EQ(impacts.front().type, FractureImpactType::ShardHit);
    EXPECT_EQ(impacts.front().materialProfile,
        FractureMaterialProfile::Glass);
    EXPECT_EQ(impacts.front().objectName, "WindowGlass");
    EXPECT_EQ(impacts.front().shardName, "WindowGlass~groundShard");
    EXPECT_EQ(impacts.front().projectileId, id);
    // 投射物命中后被消费，ProjectileRuntime 据此把可见箭矢冻结在命中点。
    EXPECT_FALSE(world.HasProjectile(id));
}

TEST(FractureImpactWorld, ProjectileHandleTracksFlyingBody)
{
    FractureImpactWorld world;
    const uint32_t id = world.LaunchProjectile(
        Eigen::Vector3f(0.0f, 0.0f, 200.0f),
        Eigen::Vector3f(0.0f, 0.0f, -100.0f),
        1.5f, 0.1f);
    ASSERT_NE(id, 0u);
    world.Step(1.0f / 60.0f);
    EXPECT_TRUE(world.HasProjectile(id));
    const auto pose = world.GetProjectilePose(id);
    ASSERT_TRUE(pose.has_value());
    EXPECT_LT(pose->position.z(), 200.0f);
    EXPECT_TRUE(world.GetProjectileVelocity(id).has_value());
}

TEST(FractureImpactWorld, HitReportCarriesProjectileHandle)
{
    FractureImpactWorld world;
    world.RegisterSphereProxy("RubberBalloon", Eigen::Vector3f::Zero(), 50.0f);
    const uint32_t id = world.LaunchProjectile(
        Eigen::Vector3f(0.0f, 0.0f, 60.0f),
        Eigen::Vector3f(0.0f, 0.0f, -1200.0f),
        5.0f, 1.0f);

    std::vector<FractureImpact> impacts;
    for (int step = 0; step < 60 && impacts.empty(); ++step)
        impacts = world.Step(1.0f / 60.0f);

    ASSERT_FALSE(impacts.empty());
    EXPECT_EQ(impacts.front().projectileId, id);
    // 命中后投射物 body 已被回收，可见层据此切换为“插在命中点”的静态姿态。
    EXPECT_FALSE(world.HasProjectile(id));
    EXPECT_FALSE(world.GetProjectilePose(id).has_value());
}

TEST(FractureImpactWorld, RemoveShardDropsBody)
{
    FractureImpactWorld world;
    world.RegisterShard("WindowGlass", "WindowGlass~shard0",
        Eigen::Vector3f(0.0f, 200.0f, 0.0f), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f::Zero(), 0.2f, CubePoints(5.0f),
        FractureMaterialProfile::Glass);
    ASSERT_TRUE(world.GetShardPose("WindowGlass", "WindowGlass~shard0").has_value());

    world.RemoveShard("WindowGlass", "WindowGlass~shard0");
    EXPECT_FALSE(world.GetShardPose("WindowGlass", "WindowGlass~shard0").has_value());
    EXPECT_FALSE(world.GetShardVelocity("WindowGlass", "WindowGlass~shard0").has_value());
}

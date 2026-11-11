#include <cmath>

#include <gtest/gtest.h>

#include "particles/particleHitScan.h"

namespace
{

using namespace VL;

TEST(ParticleHitScan, PlaneHitReturnsClosestPointAndInwardNormal)
{
    const auto hit = IntersectParticlePlane(
        Eigen::Vector3f(0.0f, 100.0f, 0.0f),
        Eigen::Vector3f(0.0f, -100.0f, 0.0f),
        Eigen::Vector3f(0.0f, 1.0f, 0.0f),
        0.0f,
        200.0f);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->distance, 100.0f, 1.0e-5f);
    EXPECT_NEAR(hit->position.y(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(hit->normal.y(), 1.0f, 1.0e-5f);
}

TEST(ParticleHitScan, MaxDistanceAndParallelMisses)
{
    EXPECT_FALSE(IntersectParticlePlane(
        Eigen::Vector3f(0.0f, 100.0f, 0.0f),
        Eigen::Vector3f(0.0f, -100.0f, 0.0f),
        Eigen::Vector3f(0.0f, 1.0f, 0.0f),
        0.0f,
        50.0f).has_value());
    EXPECT_FALSE(IntersectParticlePlane(
        Eigen::Vector3f(0.0f, 100.0f, 0.0f),
        Eigen::Vector3f(100.0f, 100.0f, 0.0f),
        Eigen::Vector3f(0.0f, 1.0f, 0.0f),
        0.0f,
        200.0f).has_value());
}

TEST(ParticleHitScan, PlaneOffsetIsWorldDistanceAlongNormal)
{
    const auto hit = IntersectParticlePlane(
        Eigen::Vector3f(20.0f, 40.0f, -10.0f),
        Eigen::Vector3f(20.0f, -40.0f, -10.0f),
        Eigen::Vector3f(0.0f, 2.0f, 0.0f),
        10.0f,
        200.0f);
    ASSERT_TRUE(hit.has_value());
    // planeNormal 会被归一化，planeOffset 仍按归一化法线的世界距离解释。
    EXPECT_NEAR(hit->position.y(), 10.0f, 1.0e-5f);
    EXPECT_NEAR(hit->position.x(), 20.0f, 1.0e-5f);
    EXPECT_NEAR(hit->position.z(), -10.0f, 1.0e-5f);
}

} // namespace

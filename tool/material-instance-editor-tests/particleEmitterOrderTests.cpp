#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "particles/particleEmitterOrder.h"

namespace
{

using namespace VL;

ParticleEmitterDepthKey MakeKey(
    const std::string& name,
    const Eigen::Vector3f& center,
    float radius = 0.0f)
{
    ParticleEmitterDepthKey key;
    key.key = name;
    key.worldCenter = center;
    key.worldRadius = radius;
    return key;
}

} // namespace

TEST(ParticleEmitterOrder, FartherEmitterIsDrawnFirst)
{
    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("near", Eigen::Vector3f(0.0f, 0.0f, -100.0f)),
        MakeKey("far", Eigen::Vector3f(0.0f, 0.0f, -200.0f))};

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order.at(0), 1u);
    EXPECT_EQ(order.at(1), 0u);
}

TEST(ParticleEmitterOrder, LargerBoundsRadiusExtendsDepthAtSameCenter)
{
    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("small", Eigen::Vector3f(0.0f, 0.0f, -100.0f), 5.0f),
        MakeKey("large", Eigen::Vector3f(0.0f, 0.0f, -100.0f), 50.0f)};

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order.at(0), 1u);
    EXPECT_EQ(order.at(1), 0u);
}

TEST(ParticleEmitterOrder, EqualDepthUsesStableKeyOrder)
{
    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("bravo", Eigen::Vector3f(0.0f, 0.0f, -100.0f), 25.0f),
        MakeKey("alpha", Eigen::Vector3f(0.0f, 0.0f, -100.0f), 25.0f)};

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order.at(0), 1u);
    EXPECT_EQ(order.at(1), 0u);
}

TEST(ParticleEmitterOrder, DepthFollowsCameraForwardDirection)
{
    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("behind", Eigen::Vector3f(0.0f, 0.0f, -100.0f)),
        MakeKey("ahead", Eigen::Vector3f(0.0f, 0.0f, 100.0f))};

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, 1.0f));

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order.at(0), 1u);
    EXPECT_EQ(order.at(1), 0u);
}

TEST(ParticleEmitterOrder, EmptyInputReturnsEmptyOrder)
{
    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            {},
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    EXPECT_TRUE(order.empty());
}

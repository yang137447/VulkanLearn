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
    float radius = 0.0f,
    int sortOrder = 0)
{
    ParticleEmitterDepthKey key;
    key.key = name;
    key.worldCenter = center;
    key.worldRadius = radius;
    key.sortOrder = sortOrder;
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

TEST(ParticleEmitterOrder, SortOrderIsPrimaryOverDepth)
{
    // 近处 emitter 被作者放进背景层（-1）后必须先于远处 emitter 绘制；
    // 若深度仍是主序，sortOrder 就失去表达跨深度固定层的能力。
    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("far", Eigen::Vector3f(0.0f, 0.0f, -200.0f), 0.0f, 0),
        MakeKey("near_background", Eigen::Vector3f(0.0f, 0.0f, -100.0f),
            0.0f, -1)};

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order.at(0), 1u);
    EXPECT_EQ(order.at(1), 0u);
}

TEST(ParticleEmitterOrder, DepthOrdersEmittersInsideSameSortOrder)
{
    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("far", Eigen::Vector3f(0.0f, 0.0f, -300.0f), 0.0f, 5),
        MakeKey("near", Eigen::Vector3f(0.0f, 0.0f, -100.0f), 0.0f, 5)};

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order.at(0), 0u);
    EXPECT_EQ(order.at(1), 1u);
}

TEST(ParticleEmitterOrder, StableKeyBreaksSortOrderAndDepthTies)
{
    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("bravo", Eigen::Vector3f(0.0f, 0.0f, -100.0f), 25.0f, 3),
        MakeKey("alpha", Eigen::Vector3f(0.0f, 0.0f, -100.0f), 25.0f, 3)};

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order.at(0), 1u);
    EXPECT_EQ(order.at(1), 0u);
}

TEST(ParticleEmitterOrder, DefaultSortOrderKeepsLegacyDepthOrder)
{
    // 旧资产没有 sortOrder，默认 0 必须与纯深度排序逐项一致。
    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("near", Eigen::Vector3f(0.0f, 0.0f, -50.0f), 5.0f),
        MakeKey("far", Eigen::Vector3f(0.0f, 0.0f, -400.0f), 30.0f),
        MakeKey("middle", Eigen::Vector3f(0.0f, 0.0f, -150.0f), 10.0f)};
    ASSERT_EQ(emitters.at(0).sortOrder, 0);
    ASSERT_EQ(emitters.at(1).sortOrder, 0);
    ASSERT_EQ(emitters.at(2).sortOrder, 0);

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order.at(0), 1u);
    EXPECT_EQ(order.at(1), 2u);
    EXPECT_EQ(order.at(2), 0u);
}

TEST(ParticleEmitterOrder, GlobalPoolUsesMinimumSortOrderAndBackgroundDepth)
{
    // 池成员跨越两个层与远近：整池插入位置取最小 sortOrder（1）以及
    // 背景边缘深度（300）。同一层里 300 比 200 更远，所以全局 item
    // 先于同层近处 emitter 绘制；sortOrder=0 的 emitter 仍排在池前面。
    const std::vector<ParticleEmitterDepthKey> members{
        MakeKey("smoke_near", Eigen::Vector3f(0.0f, 0.0f, -100.0f),
            0.0f, 5),
        MakeKey("smoke_far", Eigen::Vector3f(0.0f, 0.0f, -300.0f),
            0.0f, 1)};
    ParticleEmitterDepthKey globalKey;
    ASSERT_TRUE(TryMakeParticleGlobalSortDrawKey(
        members,
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f(0.0f, 0.0f, -1.0f),
        globalKey));
    EXPECT_EQ(globalKey.key, kParticleGlobalDrawKey);
    EXPECT_EQ(globalKey.sortOrder, 1);

    const std::vector<ParticleEmitterDepthKey> emitters{
        MakeKey("foreground_layer", Eigen::Vector3f(0.0f, 0.0f, -200.0f),
            0.0f, 0),
        MakeKey("same_layer_near", Eigen::Vector3f(0.0f, 0.0f, -200.0f),
            0.0f, 1),
        MakeKey("later_layer", Eigen::Vector3f(0.0f, 0.0f, -50.0f),
            0.0f, 5),
        globalKey};

    const std::vector<size_t> order =
        ComputeParticleEmitterDrawOrder(
            emitters,
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f(0.0f, 0.0f, -1.0f));

    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order.at(0), 0u);
    EXPECT_EQ(order.at(1), 3u);
    EXPECT_EQ(order.at(2), 1u);
    EXPECT_EQ(order.at(3), 2u);
}

TEST(ParticleEmitterOrder, EmptyGlobalPoolDoesNotProduceDrawItem)
{
    ParticleEmitterDepthKey globalKey;
    EXPECT_FALSE(TryMakeParticleGlobalSortDrawKey(
        {},
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f(0.0f, 0.0f, -1.0f),
        globalKey));
}

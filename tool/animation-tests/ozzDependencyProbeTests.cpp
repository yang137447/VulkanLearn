#include "ozzDependencyProbe.h"

#include <cmath>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/base/maths/quaternion.h>

namespace
{
class AnimationSampling : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ozz::animation::offline::RawSkeleton rawSkeleton;
        rawSkeleton.roots.resize(1);
        rawSkeleton.roots[0].name = "root";
        rawSkeleton.roots[0].transform = ozz::math::Transform::identity();
        rawSkeleton.roots[0].children.resize(1);
        auto& child = rawSkeleton.roots[0].children[0];
        child.name = "child";
        child.transform = ozz::math::Transform::identity();
        child.transform.translation = {0.0f, 3.0f, 0.0f};
        skeleton = ozz::animation::offline::SkeletonBuilder()(rawSkeleton);
        ASSERT_TRUE(skeleton);

        raw.duration = 1.0f;
        raw.tracks.resize(2);
        raw.tracks[0].translations = {{0.0f, {0.0f, 0.0f, 0.0f}},
            {1.0f, {2.0f, 0.0f, 0.0f}}};
        const float halfAngle = 3.14159265358979323846f / 4.0f;
        raw.tracks[0].rotations = {
            {0.0f, ozz::math::Quaternion::identity()},
            {1.0f, {0.0f, 0.0f, std::sin(halfAngle), std::cos(halfAngle)}}};
        raw.tracks[1].translations = {{0.0f, {0.0f, 3.0f, 0.0f}}};
        animation = ozz::animation::offline::AnimationBuilder()(raw);
        ASSERT_TRUE(animation);
    }

    ozz::animation::offline::RawAnimation raw;
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    ozz::unique_ptr<ozz::animation::Animation> animation;
};

TEST_F(AnimationSampling, ComposesParentRotationAndTranslationAtMidpoint)
{
    // 两关节手算样本验证矩阵语义，不用同一库的第二次输出充当正确性基准。
    const auto frames = VL::AnimationPreflight::SampleModelPositions(*skeleton, *animation, {0.5f});
    ASSERT_EQ(frames.size(), 1u);
    ASSERT_EQ(frames[0].size(), 2u);
    EXPECT_NEAR(frames[0][0][0], 1.0f, 0.001f);
    EXPECT_NEAR(frames[0][1][0], 1.0f - 3.0f / std::sqrt(2.0f), 0.003f);
    EXPECT_NEAR(frames[0][1][1], 3.0f / std::sqrt(2.0f), 0.003f);
    EXPECT_NEAR(frames[0][1][2], 0.0f, 0.001f);
}

TEST_F(AnimationSampling, SamplesBothEndpointsWithoutWrapping)
{
    const auto frames = VL::AnimationPreflight::SampleModelPositions(*skeleton, *animation, {0.0f, 1.0f});
    EXPECT_NEAR(frames[0][1][0], 0.0f, 0.001f);
    EXPECT_NEAR(frames[0][1][1], 3.0f, 0.001f);
    EXPECT_NEAR(frames[1][1][0], -1.0f, 0.003f);
    EXPECT_NEAR(frames[1][1][1], 0.0f, 0.003f);
}

TEST_F(AnimationSampling, SeeksBackwardWithoutStaleSamplingCache)
{
    const auto frames = VL::AnimationPreflight::SampleModelPositions(
        *skeleton, *animation, {0.75f, 0.0f, 1.0f, 0.5f, 0.75f});
    for (int joint = 0; joint < 2; ++joint)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            EXPECT_NEAR(frames[0][joint][axis], frames[4][joint][axis], 0.00001f);
        }
    }
}

TEST_F(AnimationSampling, RejectsInvalidRatiosAtTheToolBoundary)
{
    for (float ratio : {-0.01f, 1.01f, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()})
    {
        EXPECT_THROW(VL::AnimationPreflight::SampleModelPositions(
            *skeleton, *animation, {ratio}), std::invalid_argument);
    }
}

TEST_F(AnimationSampling, RejectsTrackCountMismatch)
{
    raw.tracks.resize(1);
    const auto mismatched = ozz::animation::offline::AnimationBuilder()(raw);
    ASSERT_TRUE(mismatched);
    EXPECT_THROW(VL::AnimationPreflight::SampleModelPositions(
        *skeleton, *mismatched, {0.0f}), std::invalid_argument);
}

TEST(AnimationDependency, BuildsAndSamplesRecordedWalkAgainstRawReference)
{
    const auto report = VL::AnimationPreflight::BuildDependencyReport();
    EXPECT_GT(report["jointCount"].get<int>(), 20);
    EXPECT_EQ(report["jointCount"], report["trackCount"]);
    EXPECT_EQ(report["sampleCount"], 121);
    EXPECT_EQ(report["frames"].size(), 121u);
    // 夹具源空间阈值仅用于采样压缩误差；不是 VL 厘米单位或 GASP 脚滑阈值。
    EXPECT_LT(report["maxModelPositionError"].get<float>(), 0.02f);
    EXPECT_GT(report["maxJointTravel"].get<float>(), 0.1f);
    EXPECT_EQ(report["fixture"]["rawClipBlake3"].get<std::string>().size(), 64u);
}
}

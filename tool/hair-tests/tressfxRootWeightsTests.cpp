#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "hair/tressfxRootWeights.h"

namespace
{
using VL::Hair::BuildTressfxRootWeights;
using VL::Hair::SdfProxySkinSource;
using VL::Hair::SdfProxySkinSourceInfluence;
using VL::Hair::TressfxRootWeightInput;

// 手工构造“绑定姿态身体”视图：位置 + CSR 影响。指针只在测试作用域内使用，
// 与正式 loader 传递的视图布局一致。
struct BodyFixture
{
    std::vector<Eigen::Vector3f> positions;
    std::vector<uint32_t> offsets{0};
    std::vector<SdfProxySkinSourceInfluence> influences;

    void AddVertex(const Eigen::Vector3f& position,
        const std::vector<SdfProxySkinSourceInfluence>& vertexInfluences)
    {
        positions.push_back(position);
        for (const SdfProxySkinSourceInfluence& influence : vertexInfluences)
            influences.push_back(influence);
        offsets.push_back(static_cast<uint32_t>(influences.size()));
    }

    SdfProxySkinSource View(uint32_t jointCount) const
    {
        return SdfProxySkinSource{positions.data(), positions.size(), offsets.data(),
            influences.data(), jointCount};
    }
};

BodyFixture TwoBoneBody()
{
    BodyFixture body;
    body.AddVertex(Eigen::Vector3f(0.0f, 0.0f, 0.0f), {{0, 1.0f}});
    body.AddVertex(Eigen::Vector3f(10.0f, 0.0f, 0.0f), {{1, 0.25f}, {2, 0.75f}});
    return body;
}

std::vector<Eigen::Vector3f> Roots(std::initializer_list<Eigen::Vector3f> roots)
{
    return std::vector<Eigen::Vector3f>(roots);
}
} // namespace

TEST(TressfxRootWeights, CopiesNearestVertexInfluencesAndReportsStatistics)
{
    const BodyFixture body = TwoBoneBody();
    const SdfProxySkinSource view = body.View(3);
    const std::vector<Eigen::Vector3f> roots = Roots({
        Eigen::Vector3f(0.1f, 0.0f, 0.0f),
        Eigen::Vector3f(10.1f, 0.0f, 0.0f),
        Eigen::Vector3f(100.0f, 0.0f, 0.0f)});
    const TressfxRootWeightInput input{&roots, &view, 2.0f};

    const auto derived = BuildTressfxRootWeights(input);

    ASSERT_TRUE(derived.IsSuccess());
    const auto& result = derived.Value();
    ASSERT_EQ(result.strandBones.size(), 3u);
    // 第一根：最近顶点 (0,0,0)，单骨满权重。
    ASSERT_EQ(result.strandBones[0].joints.size(), 1u);
    EXPECT_EQ(result.strandBones[0].joints[0], 0u);
    EXPECT_FLOAT_EQ(result.strandBones[0].weights[0], 1.0f);
    // 第二根：复制完整影响列表，不做 4 骨截断（截断在 GPU 打包时做）。
    ASSERT_EQ(result.strandBones[1].joints.size(), 2u);
    EXPECT_EQ(result.strandBones[1].joints[0], 1u);
    EXPECT_EQ(result.strandBones[1].joints[1], 2u);
    EXPECT_FLOAT_EQ(result.strandBones[1].weights[0], 0.25f);
    EXPECT_FLOAT_EQ(result.strandBones[1].weights[1], 0.75f);
    // 第三根：超距，留空由调用方回退 rootBone。
    EXPECT_TRUE(result.strandBones[2].joints.empty());
    EXPECT_TRUE(result.strandBones[2].weights.empty());
    EXPECT_EQ(result.unmatchedCount, 1u);
    EXPECT_EQ(result.matchedCount, 2u);
    EXPECT_EQ(result.jointCount, 3u);
    EXPECT_NEAR(result.maxNearestDistanceCm, 0.1f, 1.0e-4f);
}

TEST(TressfxRootWeights, NormalizesSourceWeightsToConvexCombination)
{
    BodyFixture body;
    body.AddVertex(Eigen::Vector3f(0.0f, 0.0f, 0.0f), {{0, 2.0f}, {1, 6.0f}});
    const SdfProxySkinSource view = body.View(2);
    const std::vector<Eigen::Vector3f> roots = Roots({Eigen::Vector3f(0.05f, 0.0f, 0.0f)});

    const auto derived = BuildTressfxRootWeights({&roots, &view, 2.0f});

    ASSERT_TRUE(derived.IsSuccess());
    const auto& weights = derived.Value().strandBones[0];
    ASSERT_EQ(weights.joints.size(), 2u);
    EXPECT_FLOAT_EQ(weights.weights[0], 0.25f);
    EXPECT_FLOAT_EQ(weights.weights[1], 0.75f);
}

TEST(TressfxRootWeights, EquidistantNearestVertexPicksLowerBodyIndex)
{
    BodyFixture body;
    body.AddVertex(Eigen::Vector3f(-0.5f, 0.0f, 0.0f), {{0, 1.0f}});
    body.AddVertex(Eigen::Vector3f(0.5f, 0.0f, 0.0f), {{1, 1.0f}});
    const SdfProxySkinSource view = body.View(2);
    const std::vector<Eigen::Vector3f> roots = Roots({Eigen::Vector3f(0.0f, 0.0f, 0.0f)});

    const auto derived = BuildTressfxRootWeights({&roots, &view, 2.0f});

    ASSERT_TRUE(derived.IsSuccess());
    ASSERT_EQ(derived.Value().strandBones[0].joints.size(), 1u);
    EXPECT_EQ(derived.Value().strandBones[0].joints[0], 0u);
}

TEST(TressfxRootWeights, ZeroWeightNearestVertexCountsAsUnmatched)
{
    BodyFixture body;
    body.AddVertex(Eigen::Vector3f(0.0f, 0.0f, 0.0f), {{0, 0.0f}});
    body.AddVertex(Eigen::Vector3f(10.0f, 0.0f, 0.0f), {{1, 1.0f}});
    const SdfProxySkinSource view = body.View(2);
    const std::vector<Eigen::Vector3f> roots = Roots({
        Eigen::Vector3f(0.05f, 0.0f, 0.0f),
        Eigen::Vector3f(10.05f, 0.0f, 0.0f)});

    const auto derived = BuildTressfxRootWeights({&roots, &view, 2.0f});

    ASSERT_TRUE(derived.IsSuccess());
    EXPECT_TRUE(derived.Value().strandBones[0].joints.empty());
    EXPECT_EQ(derived.Value().strandBones[1].joints.size(), 1u);
    EXPECT_EQ(derived.Value().unmatchedCount, 1u);
}

TEST(TressfxRootWeights, AllUnmatchedFailsWithNoMatchCode)
{
    const BodyFixture body = TwoBoneBody();
    const SdfProxySkinSource view = body.View(3);
    const std::vector<Eigen::Vector3f> roots = Roots({Eigen::Vector3f(100.0f, 0.0f, 0.0f)});

    const auto derived = BuildTressfxRootWeights({&roots, &view, 2.0f});

    ASSERT_TRUE(derived.IsFailure());
    EXPECT_EQ(derived.Error().code, "TressFXRootWeights.NoMatch");
}

TEST(TressfxRootWeights, EmptyRootsFailsWithInvalidInput)
{
    const BodyFixture body = TwoBoneBody();
    const SdfProxySkinSource view = body.View(3);
    const std::vector<Eigen::Vector3f> roots;

    const auto derived = BuildTressfxRootWeights({&roots, &view, 2.0f});

    ASSERT_TRUE(derived.IsFailure());
    EXPECT_EQ(derived.Error().code, "TressFXRootWeights.InvalidInput");
}

TEST(TressfxRootWeights, MissingBodyFailsWithInvalidInput)
{
    const std::vector<Eigen::Vector3f> roots = Roots({Eigen::Vector3f(0.0f, 0.0f, 0.0f)});

    const auto derived = BuildTressfxRootWeights({&roots, nullptr, 2.0f});

    ASSERT_TRUE(derived.IsFailure());
    EXPECT_EQ(derived.Error().code, "TressFXRootWeights.InvalidInput");
}

TEST(TressfxRootWeights, InvalidDistanceFailsWithInvalidInput)
{
    const BodyFixture body = TwoBoneBody();
    const SdfProxySkinSource view = body.View(3);
    const std::vector<Eigen::Vector3f> roots = Roots({Eigen::Vector3f(0.0f, 0.0f, 0.0f)});

    const auto derived = BuildTressfxRootWeights({&roots, &view, 0.0f});

    ASSERT_TRUE(derived.IsFailure());
    EXPECT_EQ(derived.Error().code, "TressFXRootWeights.InvalidInput");
}

TEST(TressfxRootWeights, JointOutsideSkeletonFailsWithInvalidInput)
{
    BodyFixture body;
    body.AddVertex(Eigen::Vector3f(0.0f, 0.0f, 0.0f), {{5, 1.0f}});
    const SdfProxySkinSource view = body.View(2);
    const std::vector<Eigen::Vector3f> roots = Roots({Eigen::Vector3f(0.0f, 0.0f, 0.0f)});

    const auto derived = BuildTressfxRootWeights({&roots, &view, 2.0f});

    ASSERT_TRUE(derived.IsFailure());
    EXPECT_EQ(derived.Error().code, "TressFXRootWeights.InvalidInput");
}

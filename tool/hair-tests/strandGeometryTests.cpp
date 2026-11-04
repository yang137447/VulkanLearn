#include "hair/strandGeometry.h"
#include "tfxFixture.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
using namespace VL::Hair;
using namespace TfxFixture;

TEST(StrandGeometry, StraightGuideHasUnitTangentsAndTipRestPlaceholder)
{
    const auto input = ParseTfx(Guides());
    ASSERT_TRUE(input.IsSuccess());
    const auto result = PrepareStrandGeometry(input.Value());
    ASSERT_TRUE(result.IsSuccess());
    const auto& geometry = result.Value();
    for (const auto& tangent : geometry.tangents)
        EXPECT_EQ(tangent, (std::array<float, 4>{0, 1, 0, 0}));
    EXPECT_EQ(geometry.restLengths, (std::vector<float>{1, 1, 1, 0}));
    EXPECT_FLOAT_EQ(geometry.maxRestLength, 1);
}

TEST(StrandGeometry, InverseMassDoesNotChangeTangentOrRestLength)
{
    auto bytes = Guides();
    Float(bytes, 172, 500);
    Float(bytes, 188, 0.25f);
    const auto asset = ParseTfx(bytes);
    ASSERT_TRUE(asset.IsSuccess());
    const auto result = PrepareStrandGeometry(asset.Value());
    ASSERT_TRUE(result.IsSuccess());
    EXPECT_EQ(result.Value().restLengths, (std::vector<float>{1, 1, 1, 0}));
    for (const auto& tangent : result.Value().tangents)
        EXPECT_EQ(tangent, (std::array<float, 4>{0, 1, 0, 0}));
}

TEST(StrandGeometry, UnevenSegmentsUseAverageOfUnitDirections)
{
    auto bytes = Guides();
    Point(bytes, 0, {0, 0, 0, 0});
    Point(bytes, 1, {3, 0, 0, 0});
    Point(bytes, 2, {3, 4, 0, 1});
    Point(bytes, 3, {3, 5, 0, 1});
    const auto input = ParseTfx(bytes);
    ASSERT_TRUE(input.IsSuccess());
    const auto result = PrepareStrandGeometry(input.Value());
    ASSERT_TRUE(result.IsSuccess());
    const auto& geometry = result.Value();
    EXPECT_NEAR(geometry.tangents[1][0], std::sqrt(0.5f), 1e-6f);
    EXPECT_NEAR(geometry.tangents[1][1], std::sqrt(0.5f), 1e-6f);
    EXPECT_EQ(geometry.restLengths, (std::vector<float>{3, 4, 1, 0}));
    EXPECT_FLOAT_EQ(geometry.maxRestLength, 4);
}

TEST(StrandGeometry, RibbonTopologyUsesTwoTrianglesPerSegment)
{
    const auto input = ParseTfx(Guides());
    ASSERT_TRUE(input.IsSuccess());
    const auto result = PrepareStrandGeometry(input.Value());
    ASSERT_TRUE(result.IsSuccess());
    EXPECT_EQ(result.Value().ribbonIndices,
        (std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5, 4, 5, 6, 6, 5, 7}));
}

TEST(StrandGeometry, GuidesRemainSeparateInTopologyAndRestLengths)
{
    const auto input = ParseTfx(Guides(2, 4));
    ASSERT_TRUE(input.IsSuccess());
    const auto result = PrepareStrandGeometry(input.Value());
    ASSERT_TRUE(result.IsSuccess());
    const auto& geometry = result.Value();
    ASSERT_EQ(geometry.ribbonIndices.size(), 36u);
    EXPECT_EQ(geometry.ribbonIndices[18], 8u);
    EXPECT_EQ(geometry.restLengths[3], 0);
    EXPECT_EQ(geometry.restLengths[7], 0);
    for (std::size_t triangle = 0; triangle < geometry.ribbonIndices.size(); triangle += 3)
    {
        const auto strand = geometry.ribbonIndices[triangle] / 8;
        EXPECT_EQ(geometry.ribbonIndices[triangle + 1] / 8, strand);
        EXPECT_EQ(geometry.ribbonIndices[triangle + 2] / 8, strand);
    }
}

TEST(StrandGeometry, RejectsCoincidentAdjacentPointsAtPreparationBoundary)
{
    auto bytes = Guides();
    Point(bytes, 1, {0, 0, -2, 1});
    const auto input = ParseTfx(bytes);
    ASSERT_TRUE(input.IsSuccess());
    const auto result = PrepareStrandGeometry(input.Value(), "coincident.tfx");
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Hair.StrandSegment");
    EXPECT_EQ(result.Error().sourcePath, "coincident.tfx");
    EXPECT_EQ(result.Error().sourceNode, "positions[0]");
}

TEST(StrandGeometry, RejectsExactReversalInsteadOfPublishingZeroTangent)
{
    auto bytes = Guides();
    Point(bytes, 2, {0, 0, -2, 1});
    const auto input = ParseTfx(bytes);
    ASSERT_TRUE(input.IsSuccess());
    const auto result = PrepareStrandGeometry(input.Value());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Hair.StrandTangent");
    EXPECT_EQ(result.Error().sourceNode, "positions[1]");
}

TEST(StrandGeometry, RejectsLengthBeyondFloatRangeWithoutOverflow)
{
    auto bytes = Guides();
    Point(bytes, 0, {-std::numeric_limits<float>::max(), 0, 0, 0});
    Point(bytes, 1, {std::numeric_limits<float>::max(), 0, 0, 0});
    const auto input = ParseTfx(bytes);
    ASSERT_TRUE(input.IsSuccess());
    const auto result = PrepareStrandGeometry(input.Value());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Hair.StrandSegment");
}

TEST(StrandGeometry, TinyFiniteSegmentsDoNotUnderflowDuringNormalization)
{
    auto bytes = Guides();
    Point(bytes, 0, {0, 0, 0, 0});
    Point(bytes, 1, {0, 1e-30f, 0, 0});
    Point(bytes, 2, {0, 2e-30f, 0, 1});
    Point(bytes, 3, {0, 3e-30f, 0, 1});
    const auto input = ParseTfx(bytes);
    ASSERT_TRUE(input.IsSuccess());
    const auto result = PrepareStrandGeometry(input.Value());
    ASSERT_TRUE(result.IsSuccess());
    for (const auto& tangent : result.Value().tangents)
        EXPECT_EQ(tangent, (std::array<float, 4>{0, 1, 0, 0}));
}
}

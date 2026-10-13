#include "ui/viewport/editorViewportGeometry.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace VL
{
namespace
{

void ExpectRect(const UiViewportRect& actual, const UiViewportRect& expected)
{
    EXPECT_EQ(actual.x, expected.x);
    EXPECT_EQ(actual.y, expected.y);
    EXPECT_EQ(actual.width, expected.width);
    EXPECT_EQ(actual.height, expected.height);
}

TEST(EditorViewportGeometry, ValidateAcceptsFramebufferAndOffsetRects)
{
    EXPECT_TRUE(ValidateViewportRect({0, 0, 1280, 720}, 1280, 720));
    EXPECT_TRUE(ValidateViewportRect({100, 40, 1180, 680}, 1280, 720));
}

TEST(EditorViewportGeometry, ValidateRejectsEmptyRectOrFramebuffer)
{
    EXPECT_FALSE(ValidateViewportRect({0, 0, 0, 100}, 1280, 720));
    EXPECT_FALSE(ValidateViewportRect({0, 0, 100, 0}, 1280, 720));
    EXPECT_FALSE(ValidateViewportRect({0, 0, 100, 100}, 0, 720));
    EXPECT_FALSE(ValidateViewportRect({0, 0, 100, 100}, 1280, 0));
}

TEST(EditorViewportGeometry, ValidateRejectsPartiallyOutsideRect)
{
    EXPECT_FALSE(ValidateViewportRect({-1, 0, 100, 100}, 1280, 720));
    EXPECT_FALSE(ValidateViewportRect({0, -1, 100, 100}, 1280, 720));
    EXPECT_FALSE(ValidateViewportRect({1200, 0, 100, 100}, 1280, 720));
    EXPECT_FALSE(ValidateViewportRect({0, 700, 100, 100}, 1280, 720));
}

TEST(EditorViewportGeometry, ValidateDoesNotWrapUnsignedEdgeAddition)
{
    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    EXPECT_FALSE(ValidateViewportRect({1, 0, maximum, 1}, maximum, 1));
    EXPECT_FALSE(ValidateViewportRect({0, 1, 1, maximum}, 1, maximum));
    EXPECT_TRUE(ValidateViewportRect({0, 0, maximum, maximum}, maximum, maximum));
}

TEST(EditorViewportGeometry, ClipPreservesContainedRect)
{
    const UiViewportRect rect{100, 40, 800, 600};
    ExpectRect(ClipViewportRect(rect, 1280, 720), rect);
}

TEST(EditorViewportGeometry, ClipReturnsZeroForHiddenRect)
{
    ExpectRect(ClipViewportRect({100, 40, 800, 600}, 1280, 720, false), {});
}

TEST(EditorViewportGeometry, ClipReturnsZeroForEmptyRectOrFramebuffer)
{
    ExpectRect(ClipViewportRect({10, 20, 0, 100}, 1280, 720), {});
    ExpectRect(ClipViewportRect({10, 20, 100, 0}, 1280, 720), {});
    ExpectRect(ClipViewportRect({10, 20, 100, 100}, 0, 720), {});
    ExpectRect(ClipViewportRect({10, 20, 100, 100}, 1280, 0), {});
}

TEST(EditorViewportGeometry, ClipNegativeOffsetsReducesVisibleDimensions)
{
    ExpectRect(ClipViewportRect({-20, -30, 100, 100}, 1280, 720), {0, 0, 80, 70});
}

TEST(EditorViewportGeometry, ClipRightAndBottomToFramebuffer)
{
    ExpectRect(ClipViewportRect({1200, 680, 100, 100}, 1280, 720), {1200, 680, 80, 40});
}

TEST(EditorViewportGeometry, ClipCanCoverFramebufferFromOutsideAllEdges)
{
    ExpectRect(ClipViewportRect({-100, -100, 2000, 1000}, 1280, 720), {0, 0, 1280, 720});
}

TEST(EditorViewportGeometry, ClipReturnsZeroForDisjointOrTouchingRect)
{
    ExpectRect(ClipViewportRect({-101, 0, 100, 100}, 1280, 720), {});
    ExpectRect(ClipViewportRect({-100, 0, 100, 100}, 1280, 720), {});
    ExpectRect(ClipViewportRect({0, -100, 100, 100}, 1280, 720), {});
    ExpectRect(ClipViewportRect({1280, 0, 100, 100}, 1280, 720), {});
    ExpectRect(ClipViewportRect({0, 720, 100, 100}, 1280, 720), {});
}

TEST(EditorViewportGeometry, ClipHandlesMaximumDimensionsWithoutWrapping)
{
    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    ExpectRect(ClipViewportRect({10, 20, maximum, maximum}, 1280, 720), {10, 20, 1270, 700});
}

TEST(EditorViewportGeometry, ClipHandlesMinimumSignedOffsets)
{
    const int32_t minimum = std::numeric_limits<int32_t>::min();
    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    ExpectRect(ClipViewportRect({minimum, minimum, maximum, maximum}, 1280, 720),
        {0, 0, 1280, 720});
    ExpectRect(ClipViewportRect({minimum, minimum, 100, 100}, 1280, 720), {});
}

TEST(EditorViewportGeometry, ClipPreservesRepresentableOriginInLargeFramebuffer)
{
    const int32_t origin = std::numeric_limits<int32_t>::max();
    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    const UiViewportRect clipped = ClipViewportRect({origin, origin, maximum, maximum}, maximum, maximum);
    ExpectRect(clipped, {origin, origin, uint32_t{1} << 31, uint32_t{1} << 31});
    EXPECT_TRUE(ValidateViewportRect(clipped, maximum, maximum));
}

TEST(EditorViewportGeometry, ContainsUsesHalfOpenBorders)
{
    const UiViewportRect rect{100, 40, 800, 600};
    EXPECT_TRUE(ContainsViewportPoint(rect, 100.0f, 40.0f));
    EXPECT_TRUE(ContainsViewportPoint(rect, 899.75f, 639.75f));
    EXPECT_FALSE(ContainsViewportPoint(rect, 99.75f, 40.0f));
    EXPECT_FALSE(ContainsViewportPoint(rect, 100.0f, 39.75f));
    EXPECT_FALSE(ContainsViewportPoint(rect, 900.0f, 40.0f));
    EXPECT_FALSE(ContainsViewportPoint(rect, 100.0f, 640.0f));
}

TEST(EditorViewportGeometry, ContainsRejectsEmptyRect)
{
    EXPECT_FALSE(ContainsViewportPoint({10, 20, 0, 100}, 10.0f, 20.0f));
    EXPECT_FALSE(ContainsViewportPoint({10, 20, 100, 0}, 10.0f, 20.0f));
}

TEST(EditorViewportGeometry, ContainsRejectsNonfiniteCoordinates)
{
    const UiViewportRect rect{0, 0, 100, 100};
    const float nonfinite[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    for (float value : nonfinite)
    {
        EXPECT_FALSE(ContainsViewportPoint(rect, value, 50.0f));
        EXPECT_FALSE(ContainsViewportPoint(rect, 50.0f, value));
    }
}

TEST(EditorViewportGeometry, ContainsSupportsUnclippedNegativeOrigin)
{
    const UiViewportRect rect{-20, -30, 100, 100};
    EXPECT_TRUE(ContainsViewportPoint(rect, -20.0f, -30.0f));
    EXPECT_TRUE(ContainsViewportPoint(rect, 0.0f, 0.0f));
    EXPECT_FALSE(ContainsViewportPoint(rect, 80.0f, 0.0f));
}

TEST(EditorViewportGeometry, ContainsDoesNotRoundLargeRightEdgeIntoAnInsidePoint)
{
    const UiViewportRect rect{1, 0, std::numeric_limits<uint32_t>::max(), 1};
    const float edge = 4294967296.0f;
    EXPECT_FALSE(ContainsViewportPoint(rect, edge, 0.0f));
    EXPECT_TRUE(ContainsViewportPoint(rect, std::nextafter(edge, 0.0f), 0.0f));
}

TEST(EditorViewportGeometry, ContainsKeepsPointBeforeUnrepresentableIntegerEdges)
{
    const UiViewportRect rect{1, 1, 16777216, 16777216};
    EXPECT_TRUE(ContainsViewportPoint(rect, 16777216.0f, 16777216.0f));
    EXPECT_FALSE(ContainsViewportPoint(rect, 16777218.0f, 2.0f));
    EXPECT_FALSE(ContainsViewportPoint(rect, 2.0f, 16777218.0f));
}

TEST(EditorViewportGeometry, MapSubtractsOriginAndPreservesFractionalPixels)
{
    const auto point = MapViewportPoint({100, 40, 800, 600}, 123.75f, 62.5f);
    ASSERT_TRUE(point.has_value());
    EXPECT_FLOAT_EQ(point->x, 23.75f);
    EXPECT_FLOAT_EQ(point->y, 22.5f);
    EXPECT_EQ(point->renderWidth, 800u);
    EXPECT_EQ(point->renderHeight, 600u);
}

TEST(EditorViewportGeometry, MapAcceptsTopLeftAndRejectsRightBottom)
{
    const UiViewportRect rect{100, 40, 800, 600};
    const auto origin = MapViewportPoint(rect, 100.0f, 40.0f);
    ASSERT_TRUE(origin.has_value());
    EXPECT_FLOAT_EQ(origin->x, 0.0f);
    EXPECT_FLOAT_EQ(origin->y, 0.0f);
    EXPECT_FALSE(MapViewportPoint(rect, 900.0f, 40.0f).has_value());
    EXPECT_FALSE(MapViewportPoint(rect, 100.0f, 640.0f).has_value());
}

TEST(EditorViewportGeometry, MapRejectsOutsideOrEmptyRect)
{
    EXPECT_FALSE(MapViewportPoint({100, 40, 800, 600}, 99.0f, 50.0f).has_value());
    EXPECT_FALSE(MapViewportPoint({10, 20, 0, 100}, 10.0f, 20.0f).has_value());
    EXPECT_FALSE(MapViewportPoint({10, 20, 100, 0}, 10.0f, 20.0f).has_value());
}

TEST(EditorViewportGeometry, MapRejectsNonfiniteCoordinates)
{
    const UiViewportRect rect{0, 0, 100, 100};
    const float nonfinite[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    for (float value : nonfinite)
    {
        EXPECT_FALSE(MapViewportPoint(rect, value, 50.0f).has_value());
        EXPECT_FALSE(MapViewportPoint(rect, 50.0f, value).has_value());
    }
}

TEST(EditorViewportGeometry, MapNegativeOriginSubtractsBeforeFloatConversion)
{
    const auto point = MapViewportPoint({-20, -30, 100, 100}, -10.25f, -20.5f);
    ASSERT_TRUE(point.has_value());
    EXPECT_FLOAT_EQ(point->x, 9.75f);
    EXPECT_FLOAT_EQ(point->y, 9.5f);
}

TEST(EditorViewportGeometry, MapUsesClippedOriginAndDimensions)
{
    const auto rect = ClipViewportRect({-20, -30, 100, 100}, 1280, 720);
    const auto point = MapViewportPoint(rect, 0.25f, 0.5f);
    ASSERT_TRUE(point.has_value());
    EXPECT_FLOAT_EQ(point->x, 0.25f);
    EXPECT_FLOAT_EQ(point->y, 0.5f);
    EXPECT_EQ(point->renderWidth, 80u);
    EXPECT_EQ(point->renderHeight, 70u);
}

TEST(EditorViewportGeometry, AspectUsesFloatingPointWidthOverHeight)
{
    const auto landscape = ViewportAspect({100, 40, 1280, 720});
    const auto portrait = ViewportAspect({-10, -20, 720, 1280});
    ASSERT_TRUE(landscape.has_value());
    ASSERT_TRUE(portrait.has_value());
    EXPECT_FLOAT_EQ(*landscape, 1280.0f / 720.0f);
    EXPECT_FLOAT_EQ(*portrait, 720.0f / 1280.0f);
}

TEST(EditorViewportGeometry, AspectRejectsZeroDimensions)
{
    EXPECT_FALSE(ViewportAspect({0, 0, 0, 720}).has_value());
    EXPECT_FALSE(ViewportAspect({0, 0, 1280, 0}).has_value());
    EXPECT_FALSE(ViewportAspect({}).has_value());
}

TEST(EditorViewportGeometry, AspectStaysFiniteAtDimensionExtremes)
{
    const uint32_t maximum = std::numeric_limits<uint32_t>::max();
    const auto wide = ViewportAspect({0, 0, maximum, 1});
    const auto narrow = ViewportAspect({0, 0, 1, maximum});
    ASSERT_TRUE(wide.has_value());
    ASSERT_TRUE(narrow.has_value());
    EXPECT_TRUE(std::isfinite(*wide));
    EXPECT_TRUE(std::isfinite(*narrow));
    EXPECT_GT(*wide, 0.0f);
    EXPECT_GT(*narrow, 0.0f);
}

} // namespace
} // namespace VL

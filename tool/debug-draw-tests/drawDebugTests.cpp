#include "debug/drawDebug.h"
#include "debug/debugProjection.h"
#include "debug/debugDrawControl.h"
#include <gtest/gtest.h>
#include <limits>

namespace
{
using namespace VL::Debug;
Style Persistent(std::string owner = "owner", std::string category = "Physics", std::string key = "")
{
    Style s; s.duration = -1; s.owner = std::move(owner); s.category = std::move(category); s.key = std::move(key); return s;
}
TEST(DrawDebugGeometry, SphereHasThreeClosedGreatCircles)
{
    DrawDebug draw;
    draw.DrawDebugSphere({10, 20, 30}, 5);
    const auto view = draw.Freeze();
    ASSERT_EQ(view->lines.size(), 72u);
    for (const auto& line : view->lines)
    {
        EXPECT_NEAR((line.begin - Eigen::Vector3f(10, 20, 30)).norm(), 5, 1e-4f);
        EXPECT_NEAR((line.end - Eigen::Vector3f(10, 20, 30)).norm(), 5, 1e-4f);
    }
}
TEST(DrawDebugGeometry, CircleOrientationUsesLocalXZPlane)
{
    DrawDebug draw;
    draw.DrawDebugCircle({1, 2, 3}, 5, Eigen::Quaternionf(Eigen::AngleAxisf(1.5707963268f, Eigen::Vector3f::UnitX())));
    const auto view = draw.Freeze();
    for (const auto& line : view->lines) EXPECT_NEAR(line.begin.z(), 3, 1e-4f);
}
TEST(DrawDebugGeometry, RotatedBoxHasTwelveEdgesWithHalfExtents)
{
    DrawDebug draw;
    draw.DrawDebugBox({1, 2, 3}, {2, 4, 8}, Eigen::Quaternionf(Eigen::AngleAxisf(1.5707963268f, Eigen::Vector3f::UnitY())));
    auto view = draw.Freeze();
    ASSERT_EQ(view->lines.size(), 12u);
    for (const auto& line : view->lines)
    {
        EXPECT_NEAR(std::abs(line.begin.x() - 1), 8, 1e-4f);
        EXPECT_NEAR(std::abs(line.begin.y() - 2), 4, 1e-4f);
        EXPECT_NEAR(std::abs(line.begin.z() - 3), 2, 1e-4f);
    }
}
TEST(DrawDebugGeometry, CapsuleIncludesCylinderAndHemispheresWithoutSeamDiagonals)
{
    DrawDebug draw;
    draw.DrawDebugCapsule({0, 0, 0}, 5, 10);
    auto view = draw.Freeze();
    ASSERT_EQ(view->lines.size(), 100u);
    float minY = 100, maxY = -100;
    size_t cylinderSides = 0;
    for (const auto& line : view->lines)
    {
        minY = std::min(minY, std::min(line.begin.y(), line.end.y()));
        maxY = std::max(maxY, std::max(line.begin.y(), line.end.y()));
        if (std::abs(line.begin.y() - line.end.y()) > 19)
        {
            ++cylinderSides;
            EXPECT_FLOAT_EQ(line.begin.x(), line.end.x()); EXPECT_FLOAT_EQ(line.begin.z(), line.end.z());
        }
    }
    EXPECT_NEAR(minY, -15, 1e-4f); EXPECT_NEAR(maxY, 15, 1e-4f); EXPECT_EQ(cylinderSides, 4u);
}
TEST(DrawDebugGeometry, ZeroCylinderCapsuleIsSphere)
{
    DrawDebug draw; draw.DrawDebugCapsule({0, 0, 0}, 5, 0);
    EXPECT_EQ(draw.Freeze()->lines.size(), 72u);
}
TEST(DrawDebugGeometry, ArrowHasFourHeadEdgesAndCoordinateSystemHasAxisColors)
{
    DrawDebug draw;
    draw.DrawDebugArrow({0, 0, 0}, {10, 0, 0}, 2);
    EXPECT_EQ(draw.Freeze()->lines.size(), 5u);
    draw.Clear(); draw.DrawDebugCoordinateSystem({0, 0, 0});
    const auto view = draw.Freeze();
    ASSERT_EQ(view->lines.size(), 15u);
    EXPECT_EQ(view->lines[0].color, (Color{1, .15f, .15f, 1}));
    EXPECT_EQ(view->lines[5].color, (Color{.15f, 1, .15f, 1}));
    EXPECT_EQ(view->lines[10].color, (Color{.15f, .15f, 1, 1}));
}
TEST(DrawDebugGeometry, ClosedPolylineAddsOnlyClosingSegment)
{
    DrawDebug draw; draw.DrawDebugPolyline({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}}, true);
    auto view = draw.Freeze(); ASSERT_EQ(view->lines.size(), 3u);
    EXPECT_TRUE(view->lines.back().end.isZero());
}
TEST(DrawDebugLifetime, SingleFrameClearsOnceAtGTBoundaryAndFrozenValuesSurvive)
{
    DrawDebug draw;
    draw.BeginFrame(); draw.DrawDebugPoint({0, 0, 0});
    const auto frozen = draw.Freeze();
    draw.AdvanceGameTime(1.0f / 60);
    EXPECT_EQ(draw.Count(), 1u);
    draw.EndFrame(); draw.BeginFrame(); EXPECT_EQ(draw.Count(), 0u); EXPECT_EQ(frozen->points.size(), 1u);
}
TEST(DrawDebugLifetime, CommandsBeforeFrameCleanupSurviveAndPausedCompletedFrameIsRetained)
{
    DrawDebug draw;
    draw.DrawDebugPoint({0, 0, 0});
    draw.EndFrame();
    // Pause 命令跳过 BeginFrame，仍完成冻结/显示；Resume 时只删除旧帧值。
    draw.EndFrame();
    EXPECT_EQ(draw.Count(), 1u);
    draw.DrawDebugPoint({1, 0, 0});
    draw.BeginFrame();
    ASSERT_EQ(draw.Count(), 1u);
    EXPECT_FLOAT_EQ(draw.Freeze()->points[0].position.x(), 1);
    draw.EndFrame(); draw.BeginFrame();
    EXPECT_EQ(draw.Count(), 0u);
}
TEST(DrawDebugLifetime, NewWorldAndSameKeyReplacementBeforeCleanupSurviveFirstFrame)
{
    DrawDebug draw;
    auto style = Persistent("owner", "General", "same"); style.duration = 0;
    draw.DrawDebugPoint({0, 0, 0}, 8, style);
    draw.BeginFrame(); EXPECT_EQ(draw.Count(), 1u);
    draw.EndFrame();
    draw.DrawDebugPoint({2, 0, 0}, 8, style);
    draw.BeginFrame();
    ASSERT_EQ(draw.Count(), 1u);
    EXPECT_FLOAT_EQ(draw.Freeze()->points[0].position.x(), 2);
}
TEST(DrawDebugLifetime, TimedAndPersistentIgnoreZeroGameDelta)
{
    DrawDebug draw;
    auto s = Persistent(); s.duration = .5f;
    draw.DrawDebugLine({0, 0, 0}, {1, 0, 0}, s);
    draw.DrawDebugString({0, 0, 0}, "persistent", 14, Persistent());
    draw.BeginFrame(); draw.AdvanceGameTime(0);
    EXPECT_EQ(draw.Count(), 2u);
    draw.AdvanceGameTime(.25f); EXPECT_EQ(draw.Count(), 2u);
    draw.AdvanceGameTime(.25f); EXPECT_EQ(draw.Count(), 1u);
}
TEST(DrawDebugLifetime, KeyReplacementIsScopedAndResetsDeadline)
{
    DrawDebug draw; auto s = Persistent("a", "Physics", "query"); s.duration = 1;
    const auto handle = draw.DrawDebugPoint({0, 0, 0}, 8, s);
    draw.AdvanceGameTime(.5f);
    EXPECT_EQ(draw.DrawDebugPoint({1, 0, 0}, 8, s), handle);
    draw.DrawDebugPoint({2, 0, 0}, 8, Persistent("b", "Physics", "query"));
    draw.AdvanceGameTime(.5f); EXPECT_EQ(draw.Count(), 2u);
    draw.AdvanceGameTime(.5f); EXPECT_EQ(draw.Count(), 1u);
}
TEST(DrawDebugLifetime, HandlesDoNotAliasAfterClear)
{
    DrawDebug draw; auto old = draw.DrawDebugPoint({0, 0, 0});
    draw.Clear(); auto current = draw.DrawDebugPoint({0, 0, 0});
    EXPECT_NE(old, current); EXPECT_FALSE(draw.Remove(old)); EXPECT_TRUE(draw.Remove(current));
}
TEST(DrawDebugFiltering, HiddenCategoriesAndOwnerFilteringDoNotDestroyStoredItems)
{
    DrawDebug draw;
    draw.DrawDebugPoint({0, 0, 0}, 8, Persistent("a"));
    draw.DrawDebugString({0, 0, 0}, "text", 14, Persistent("b", "AI"));
    draw.SetCategoryEnabled("Physics", false);
    auto frozen = draw.Freeze(); EXPECT_EQ(frozen->storedCount, 2u); EXPECT_EQ(frozen->visibleCount, 1u);
    draw.SetOwnerFilter("a"); EXPECT_EQ(draw.Freeze()->visibleCount, 0u);
    draw.SetCategoryEnabled("Physics", true); EXPECT_EQ(draw.Freeze()->visibleCount, 1u);
    draw.SetEnabled(false); EXPECT_TRUE(draw.Freeze()->points.empty());
    EXPECT_EQ(draw.Count(), 2u); EXPECT_EQ(frozen->texts.size(), 1u);
}
TEST(DrawDebugFiltering, PublisherAndCategoryClearDoNotAffectSibling)
{
    DrawDebug draw;
    auto s = Persistent(); s.publisher = "first";
    draw.DrawDebugPoint({0, 0, 0}, 8, s);
    s.publisher = "second"; draw.DrawDebugPoint({1, 0, 0}, 8, s);
    draw.ClearPublisher("owner", "first"); EXPECT_EQ(draw.Count(), 1u);
    draw.ClearCategory("AI"); EXPECT_EQ(draw.Count(), 1u);
    draw.ClearOwner("owner"); EXPECT_EQ(draw.Count(), 0u);
}
TEST(DrawDebugValidation, InvalidReplacementLeavesPublishedItemUnchanged)
{
    DrawDebug draw; auto s = Persistent("owner", "Physics", "same");
    draw.DrawDebugPoint({1, 2, 3}, 8, s);
    s.duration = -2;
    EXPECT_THROW(draw.DrawDebugPoint({0, 0, 0}, 8, s), std::invalid_argument);
    EXPECT_EQ(draw.Count(), 1u); EXPECT_FLOAT_EQ(draw.Freeze()->points[0].position.x(), 1);
}
TEST(DrawDebugValidation, RejectsUnknownDepthAndInvalidGeometry)
{
    DrawDebug draw; auto s = Persistent(); s.depth = static_cast<DepthMode>(2);
    EXPECT_THROW(draw.DrawDebugPoint({0, 0, 0}, 8, s), std::invalid_argument);
    EXPECT_THROW(draw.DrawDebugArrow({0, 0, 0}, {0, 0, 0}), std::invalid_argument);
    EXPECT_THROW(draw.DrawDebugSphere({0, 0, 0}, -1), std::invalid_argument);
    EXPECT_THROW(draw.DrawDebugSphere({0, 0, 0}, 1, {}, 7), std::invalid_argument);
    EXPECT_THROW(draw.DrawDebugBox({0, 0, 0}, {1, -1, 1}), std::invalid_argument);
    EXPECT_THROW(draw.DrawDebugLine({NAN, 0, 0}, {0, 0, 0}), std::invalid_argument);
    EXPECT_THROW(draw.DrawDebugString({0, 0, 0}, ""), std::invalid_argument);
    EXPECT_THROW(draw.DrawDebugString({0, 0, 0}, std::string(513, 'x')), std::invalid_argument);
}
TEST(DrawDebugGeometry, AllShapesPreserveOptionalDepthAndDefaultIsOverlay)
{
    EXPECT_TRUE(DrawDebug::SupportsDepthTest());
    EXPECT_EQ(Style{}.depth, DepthMode::Overlay);
    for (int shape = 0; shape <= int(Shape::Text); ++shape)
    {
        Request r; r.shape = static_cast<Shape>(shape); r.style.depth = DepthMode::DepthTest;
        r.end = {1, 1, 1}; r.points = {{0, 0, 0}, {1, 1, 1}}; r.text = "depth"; r.size = 14;
        DrawDebug draw; draw.Submit(r);
        const auto view = draw.Freeze();
        for (const auto& line : view->lines) EXPECT_EQ(line.depth, DepthMode::DepthTest);
        for (const auto& point : view->points) EXPECT_EQ(point.depth, DepthMode::DepthTest);
        for (const auto& text : view->texts) EXPECT_EQ(text.depth, DepthMode::DepthTest);
    }
}
TEST(DrawDebugValidation, GeometryBudgetRejectsBeforeMutatingStore)
{
    DrawDebug draw;
    auto s = Persistent();
    for (int i = 0; i < 42; ++i) draw.DrawDebugSphere({0, 0, 0}, 1, s, 128);
    EXPECT_EQ(draw.Count(), 42u);
    EXPECT_THROW(draw.DrawDebugSphere({0, 0, 0}, 1, s, 128), std::invalid_argument);
    EXPECT_EQ(draw.Count(), 42u);
}
TEST(DrawDebugProjection, ClipsAcrossEyeNearFarAndViewport)
{
    Camera camera;
    ScreenLine result;
    EXPECT_TRUE(ProjectLine(camera, {0, 0, 1}, {1, 0, -10}, 400, 300, result));
    EXPECT_GE(result.begin.x(), 0); EXPECT_LE(result.end.x(), 400);
    EXPECT_TRUE(std::isfinite(result.begin.x()));
    EXPECT_TRUE(ProjectLine(camera, {-100, 0, -10}, {100, 0, -10}, 400, 300, result));
    EXPECT_NEAR(result.begin.x(), 0, 1e-3f); EXPECT_NEAR(result.end.x(), 400, 1e-3f);
    EXPECT_FALSE(ProjectLine(camera, {0, 0, 1}, {0, 0, 10}, 400, 300, result));
    EXPECT_FALSE(ProjectLine(camera, {0, 0, -1001}, {0, 0, -2000}, 400, 300, result));
}
TEST(DrawDebugProjection, PointsRespectViewportAndWorldY)
{
    Eigen::Vector2f pixel;
    EXPECT_TRUE(ProjectPoint({}, {0, 1, -10}, 400, 300, pixel)); EXPECT_LT(pixel.y(), 150);
    EXPECT_FALSE(ProjectPoint({}, {0, 0, 1}, 400, 300, pixel));
    EXPECT_FALSE(ProjectPoint({}, {100, 0, -1}, 400, 300, pixel));
}
TEST(DrawDebugProjection, DeviceDepthMapsNearFarAndScreenClippingInterpolatesReciprocalZ)
{
    Camera camera; camera.clipNear = 1; camera.clipFar = 101; camera.horizontalFovDegrees = 90;
    Eigen::Vector2f pixel; float depth = -1;
    ASSERT_TRUE(ProjectPoint(camera, {0, 0, -1}, 400, 300, pixel, &depth));
    EXPECT_FLOAT_EQ(depth, 0);
    ASSERT_TRUE(ProjectPoint(camera, {0, 0, -101}, 400, 300, pixel, &depth));
    EXPECT_NEAR(depth, 1, 1e-6f);
    ScreenLine line;
    ASSERT_TRUE(ProjectLine(camera, {-4, 0, -2}, {0, 0, -10}, 400, 300, line));
    EXPECT_NEAR(line.begin.x(), 0, 1e-4f);
    // 屏幕起点 -200 被裁到 0，t=0.5；device Z=(0.505+0.909)/2。
    EXPECT_NEAR(line.beginDepth, .707f, 1e-6f);
    EXPECT_NEAR(line.endDepth, .909f, 1e-6f);
}
TEST(DrawDebugProjection, NearFarClippingRetainsDeviceDepthEndpoints)
{
    Camera camera; camera.clipNear = 1; camera.clipFar = 11;
    ScreenLine line;
    ASSERT_TRUE(ProjectLine(camera, {0, 0, 1}, {1, 0, -21}, 400, 300, line));
    EXPECT_NEAR(line.beginDepth, 0, 1e-6f);
    EXPECT_NEAR(line.endDepth, 1, 1e-6f);
}
TEST(DrawDebugControl, ParsesQuotedCategoriesAndRejectsTrailingOrMalformedArguments)
{
    Control control; std::string error;
    ASSERT_TRUE(ParseControl("category \"AI Paths\" off", control, error));
    EXPECT_EQ(control.action, ControlAction::Category); EXPECT_EQ(control.name, "AI Paths"); EXPECT_FALSE(control.enabled);
    for (const char* input : {"category", "category Foo yes", "on extra", "clear owner", "clear who x", "owner", "status 1",
        "demo unknown", "demo depth extra"})
        EXPECT_FALSE(ParseControl(input, control, error)) << input;
    ASSERT_TRUE(ParseControl("owner *", control, error)); EXPECT_TRUE(control.name.empty());
}
TEST(DrawDebugControl, DemoAcceptsOptionalDepthAndReplacesOverlayWithoutDuplicatingItems)
{
    DrawDebug draw; Control control; std::string error;
    ASSERT_TRUE(ParseControl("demo", control, error)); ApplyControl(draw, control);
    const auto count = draw.Count();
    EXPECT_EQ(draw.Freeze()->lines.front().depth, DepthMode::Overlay);
    ASSERT_TRUE(ParseControl("demo depth", control, error)); ApplyControl(draw, control);
    EXPECT_EQ(draw.Count(), count);
    const auto view = draw.Freeze();
    for (const auto& line : view->lines) EXPECT_EQ(line.depth, DepthMode::DepthTest);
    for (const auto& point : view->points) EXPECT_EQ(point.depth, DepthMode::DepthTest);
    for (const auto& text : view->texts) EXPECT_EQ(text.depth, DepthMode::DepthTest);
    ASSERT_TRUE(ParseControl("demo overlay", control, error)); ApplyControl(draw, control);
    EXPECT_EQ(draw.Freeze()->lines.front().depth, DepthMode::Overlay);
}
TEST(DrawDebugControl, DemoReplacesItsOwnKeysAndClearOwnerRetainsOtherPublishers)
{
    DrawDebug draw; draw.DrawDebugPoint({0, 0, 0}, 8, Persistent());
    DrawDebugDemo(draw); const auto count = draw.Count(); DrawDebugDemo(draw);
    EXPECT_EQ(draw.Count(), count);
    Control control; control.action = ControlAction::ClearOwner; control.name = "Demo";
    ApplyControl(draw, control); EXPECT_EQ(draw.Count(), 1u);
}
TEST(DrawDebugLifetime, HandlesCannotRemoveItemsFromAnotherWorldCollector)
{
    DrawDebug first, second;
    const auto old = first.DrawDebugPoint({0, 0, 0});
    const auto current = second.DrawDebugPoint({0, 0, 0});
    EXPECT_NE(old, current); EXPECT_FALSE(second.Remove(old)); EXPECT_EQ(second.Count(), 1u);
}
}

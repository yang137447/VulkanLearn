#include "ui/debugDrawOverlay.h"
#include "ui/rmlUiInterfaces.h"
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/SystemInterface.h>
#include <gtest/gtest.h>

namespace
{
using namespace VL;

TEST(DebugDrawOverlay, ClipsNearPlaneAndDrawsBelowMenusInsideViewport)
{
    UiRenderSnapshot snapshot;
    snapshot.sceneViewportRect = {100, 50, 400, 300};
    UiDrawCommand image, menu;
    image.textureId = SceneViewportTextureId;
    menu.textureId = 123;
    snapshot.drawCommands = {image, menu};
    Debug::Snapshot view;
    view.enabled = true;
    view.lines.push_back({{0, 0, 1}, {1, 0, -10}, {0, 1, 1, 1}, 2.5f});
    const auto first = snapshot.drawCommands.size();
    AppendDebugDrawOverlay(snapshot, view, {});
    InsertDebugDrawCommands(snapshot, first);
    ASSERT_EQ(snapshot.drawCommands.size(), 3u);
    EXPECT_EQ(snapshot.drawCommands[1].textureId, 0u);
    EXPECT_EQ(snapshot.drawCommands[2].textureId, 123u);
    EXPECT_EQ(snapshot.drawCommands[1].clipRect.x, 100);
    EXPECT_EQ(snapshot.drawCommands[1].clipRect.width, 400u);
    EXPECT_EQ(snapshot.indices.size(), 6u);
    for (const auto& vertex : snapshot.vertices)
    {
        EXPECT_TRUE(std::isfinite(vertex.position[0]));
        EXPECT_TRUE(std::isfinite(vertex.position[1]));
        EXPECT_GE(vertex.position[0], 98.75f);
        EXPECT_LE(vertex.position[0], 501.25f);
    }
}

TEST(DebugDrawOverlay, DisabledOrBehindCameraProducesNoGeometry)
{
    UiRenderSnapshot snapshot;
    snapshot.sceneViewportRect = {0, 0, 400, 300};
    Debug::Snapshot view;
    view.enabled = false;
    view.lines.push_back({{0, 0, -10}, {1, 0, -10}, {0, 1, 1, 1}, 2});
    AppendDebugDrawOverlay(snapshot, view, {});
    EXPECT_TRUE(snapshot.vertices.empty());
    view.enabled = true;
    view.lines[0].begin.z() = view.lines[0].end.z() = 10;
    AppendDebugDrawOverlay(snapshot, view, {});
    EXPECT_TRUE(snapshot.vertices.empty());
    EXPECT_TRUE(snapshot.drawCommands.empty());
}

TEST(DebugDrawOverlay, PointSizeAndThicknessArePixelsAndImmersiveDrawsBelowHud)
{
    UiRenderSnapshot snapshot; snapshot.sceneViewportRect = {0, 0, 400, 300};
    UiDrawCommand hud; hud.textureId = 42; snapshot.drawCommands.push_back(hud);
    Debug::DrawDebug draw;
    draw.DrawDebugPoint({0, 0, -10}, 12);
    draw.DrawDebugLine({-1, 0, -10}, {1, 0, -10});
    auto view = draw.Freeze();
    AppendDebugDrawOverlay(snapshot, *view, {});
    InsertDebugDrawCommands(snapshot, 1);
    ASSERT_EQ(snapshot.drawCommands.size(), 2u);
    EXPECT_EQ(snapshot.drawCommands.back().textureId, 42u);
    ASSERT_EQ(snapshot.vertices.size(), 8u);
    EXPECT_FLOAT_EQ(snapshot.vertices[2].position[1] - snapshot.vertices[0].position[1], 2);
    EXPECT_FLOAT_EQ(snapshot.vertices[6].position[0] - snapshot.vertices[4].position[0], 12);
}
TEST(DebugDrawOverlay, MixedDepthLinesAndPointsKeepEndpointDepthWithoutAffectingOverlay)
{
    UiRenderSnapshot snapshot; snapshot.sceneViewportRect = {100, 50, 400, 300};
    Debug::DrawDebug draw;
    draw.DrawDebugLine({-1, 0, -2}, {1, 0, -10});
    Debug::Style style; style.depth = Debug::DepthMode::DepthTest;
    draw.DrawDebugLine({-1, 0, -2}, {1, 0, -10}, style);
    draw.DrawDebugPoint({0, 0, -5}, 8, style);
    AppendDebugDrawOverlay(snapshot, *draw.Freeze(), {});
    ASSERT_EQ(snapshot.vertices.size(), 12u);
    EXPECT_TRUE(snapshot.debugDepthTest);
    for (int i = 0; i < 4; ++i) EXPECT_FLOAT_EQ(snapshot.vertices[i].debugDepth, -1);
    EXPECT_LT(snapshot.vertices[4].debugDepth, snapshot.vertices[5].debugDepth);
    EXPECT_FLOAT_EQ(snapshot.vertices[4].debugDepth, snapshot.vertices[7].debugDepth);
    EXPECT_FLOAT_EQ(snapshot.vertices[5].debugDepth, snapshot.vertices[6].debugDepth);
    for (int i = 8; i < 12; ++i) EXPECT_FLOAT_EQ(snapshot.vertices[i].debugDepth, snapshot.vertices[8].debugDepth);
}

class DebugLabelSystem final : public Rml::SystemInterface
{
public: double GetElapsedTime() override { return 0; }
};
class DebugDrawLabelTest : public testing::Test
{
protected:
    void SetUp() override
    {
        Rml::SetSystemInterface(&system); Rml::SetRenderInterface(&renderer);
        ASSERT_TRUE(Rml::Initialise());
        ASSERT_TRUE(Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf"));
        ASSERT_TRUE(labels.Initialize("debug-draw-label-test", renderer));
    }
    void TearDown() override
    {
        labels.Reset(); Rml::Shutdown(); Rml::SetRenderInterface(nullptr); Rml::SetSystemInterface(nullptr);
    }
    DebugLabelSystem system;
    RmlUiRenderInterface renderer;
    DebugDrawLabels labels;
};
TEST_F(DebugDrawLabelTest, TextIsEscapedProjectedClippedAndReusesSlots)
{
    Debug::DrawDebug draw;
    draw.DrawDebugString({0, 0, -10}, "<b>Health</b> & 100");
    draw.DrawDebugString({0, 0, 10}, "behind camera");
    UiRenderSnapshot snapshot; snapshot.viewportWidth = 960; snapshot.viewportHeight = 540;
    renderer.BeginFrame(snapshot);
    labels.Render(*draw.Freeze(), {}, {100, 50, 400, 300}, 960, 540);
    renderer.EndFrame();
    EXPECT_FALSE(snapshot.vertices.empty());
    auto* root = labels.Document()->GetElementById("labels");
    ASSERT_EQ(root->GetNumChildren(), 1);
    auto* text = root->GetChild(0);
    EXPECT_EQ(text->QuerySelector("b"), nullptr);
    EXPECT_NE(text->GetInnerRML().find("&lt;b&gt;Health&lt;/b&gt;"), std::string::npos);
    draw.Clear(); draw.DrawDebugString({0, 0, -10}, "updated");
    renderer.BeginFrame(snapshot);
    labels.Render(*draw.Freeze(), {}, {100, 50, 400, 300}, 960, 540);
    renderer.EndFrame();
    EXPECT_EQ(root->GetChild(0), text);
    EXPECT_EQ(text->GetInnerRML(), "updated");
    draw.Clear(); draw.DrawDebugString({0, 0, 10}, "behind");
    renderer.BeginFrame(snapshot);
    labels.Render(*draw.Freeze(), {}, {100, 50, 400, 300}, 960, 540);
    renderer.EndFrame();
    EXPECT_FALSE(text->IsVisible());
}
TEST_F(DebugDrawLabelTest, EachLabelPublishesItsOwnAnchorDepthAndRestoresOrdinaryUi)
{
    Debug::DrawDebug draw;
    Debug::Style style; style.depth = Debug::DepthMode::DepthTest;
    draw.DrawDebugString({-.5f, .25f, -2}, "near", 14, style);
    draw.DrawDebugString({-1, 0, -10}, "far", 14, style);
    draw.DrawDebugString({-1, -1, -10}, "overlay");
    UiRenderSnapshot snapshot; snapshot.viewportWidth = 960; snapshot.viewportHeight = 540;
    renderer.BeginFrame(snapshot);
    labels.Render(*draw.Freeze(), {}, {100, 50, 400, 300}, 960, 540);
    ASSERT_EQ(snapshot.drawCommands.size(), 3u);
    std::vector<float> depths;
    for (const auto& command : snapshot.drawCommands)
    {
        const float depth = snapshot.vertices[snapshot.indices[command.firstIndex]].debugDepth;
        depths.push_back(depth);
        for (uint32_t index = command.firstIndex; index < command.firstIndex + command.indexCount; ++index)
            EXPECT_FLOAT_EQ(snapshot.vertices[snapshot.indices[index]].debugDepth, depth);
    }
    EXPECT_GT(depths[0], 0); EXPECT_LT(depths[0], depths[1]); EXPECT_FLOAT_EQ(depths[2], -1);
    EXPECT_TRUE(snapshot.debugDepthTest);
    // label context 返回后普通 RmlUi 绘制不得继承最后一个深度标签。
    draw.Clear(); draw.DrawDebugString({0, 0, -5}, "last depth", 14, style);
    labels.Render(*draw.Freeze(), {}, {100, 50, 400, 300}, 960, 540);
    Rml::Vertex vertices[3]{};
    int indices[]{0, 1, 2};
    const auto geometry = renderer.CompileGeometry({vertices, 3}, {indices, 3});
    renderer.RenderGeometry(geometry, {}, 0);
    EXPECT_FLOAT_EQ(snapshot.vertices.back().debugDepth, -1);
    renderer.ReleaseGeometry(geometry);
    renderer.EndFrame();
}
}

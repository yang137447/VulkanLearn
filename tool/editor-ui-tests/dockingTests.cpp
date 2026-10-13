#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "ui/widgets/workspace/dockWorkspace.h"

namespace
{
using namespace VL;
using namespace VL::EditorUi;
using namespace VL::UiWidgets;

TEST(DockWorkspaceTest, DefaultLayoutHasUeRegionsAndOneContentRendererStack)
{
    DockWorkspace workspace;
    std::string error;
    ASSERT_TRUE(DockWorkspace::Validate(workspace.State(), error)) << error;
    const auto geometry = workspace.Layout({0, 0, 1280, 654});
    ASSERT_EQ(geometry.stacks.size(), 4u);
    EXPECT_EQ(geometry.splitters.size(), 3u);
    EXPECT_EQ(workspace.FindStack("content"), workspace.FindStack("renderer"));
    EXPECT_NE(workspace.FindStack("outliner"), workspace.FindStack("details"));
    EXPECT_EQ(workspace.State().nodes.at(workspace.FindStack("content")).active, "content");
}

TEST(DockWorkspaceTest, CenterDockMergesTabsAndCollapsesSourceSplit)
{
    DockWorkspace workspace;
    const auto target = workspace.FindStack("viewport");
    ASSERT_TRUE(workspace.Dock("outliner", target, DockEdge::Center));
    EXPECT_EQ(workspace.FindStack("outliner"), target);
    EXPECT_EQ(workspace.State().nodes.at(target).active, "outliner");
    EXPECT_EQ(workspace.State().nodes.size(), 5u);
    std::string error;
    EXPECT_TRUE(DockWorkspace::Validate(workspace.State(), error)) << error;
}

TEST(DockWorkspaceTest, ResourceDocumentsUseTheWorkspaceTabStack)
{
    DockWorkspace workspace;
    ASSERT_TRUE(workspace.ReplaceResourcePanels({"resource:0", "resource:1"}));
    const auto stack = workspace.FindStack("resource:0");
    ASSERT_NE(stack, 0u);
    EXPECT_EQ(stack, workspace.FindStack("viewport"));
    EXPECT_EQ(workspace.State().nodes.size(), 7u);
    EXPECT_EQ(workspace.FindStack("resource:1"), stack);
    const auto& tabs = workspace.State().nodes.at(stack).tabs;
    EXPECT_EQ(std::find(tabs.begin(), tabs.end(), "resource"), tabs.end());
    EXPECT_NE(std::find(tabs.begin(), tabs.end(), "resource:0"), tabs.end());
    EXPECT_NE(std::find(tabs.begin(), tabs.end(), "resource:1"), tabs.end());
    std::string error;
    EXPECT_TRUE(DockWorkspace::Validate(workspace.State(), error)) << error;
    ASSERT_TRUE(workspace.ReplaceResourcePanels({"resource:1"}));
    EXPECT_EQ(workspace.FindStack("resource:0"), 0u);
    EXPECT_NE(workspace.FindStack("resource:1"), 0u);
    EXPECT_TRUE(DockWorkspace::Validate(workspace.State(), error)) << error;
    ASSERT_TRUE(workspace.ReplaceResourcePanels({"resource:1", "resource:2"}));
    EXPECT_EQ(workspace.FindStack("resource:2"), workspace.FindStack("viewport"));
    EXPECT_EQ(workspace.State().nodes.size(), 7u);
    ASSERT_TRUE(workspace.ReplaceResourcePanels({}));
    EXPECT_EQ(workspace.State().nodes.at(workspace.FindStack("viewport")).tabs.size(), 1u);
}

TEST(DockWorkspaceTest, EveryEdgeCreatesAnIndependentSplit)
{
    for (const DockEdge edge : {DockEdge::Left, DockEdge::Right, DockEdge::Top, DockEdge::Bottom})
    {
        DockWorkspace workspace;
        const auto target = workspace.FindStack("viewport");
        ASSERT_TRUE(workspace.Dock("renderer", target, edge));
        EXPECT_NE(workspace.FindStack("renderer"), target);
        std::string error;
        EXPECT_TRUE(DockWorkspace::Validate(workspace.State(), error)) << error;
        const auto geometry = workspace.Layout({0, 0, 1280, 654});
        EXPECT_EQ(geometry.stacks.size(), 5u);
        EXPECT_EQ(geometry.splitters.size(), 4u);
    }
}

TEST(DockWorkspaceTest, FloatingBoundsStayInsideWorkspaceAndCanReattach)
{
    DockWorkspace workspace;
    ASSERT_TRUE(workspace.Float("details", {-1.0f, 2.0f, 0.4f, 0.6f}));
    ASSERT_EQ(workspace.State().floating.size(), 1u);
    const auto& floating = workspace.State().floating.front();
    EXPECT_FLOAT_EQ(floating.rect.x, 0.0f);
    EXPECT_FLOAT_EQ(floating.rect.y, 0.4f);
    const auto geometry = workspace.Layout({0, 0, 960, 474});
    const auto& rect = geometry.stacks.back().rect;
    EXPECT_GE(rect.x, 0);
    EXPECT_GE(rect.y, 0);
    EXPECT_LE(rect.x + rect.width, 960);
    EXPECT_LE(rect.y + rect.height, 474);
    EXPECT_TRUE(workspace.Dock("details", workspace.FindStack("outliner"), DockEdge::Center));
    EXPECT_TRUE(workspace.State().floating.empty());
    std::string error;
    EXPECT_TRUE(DockWorkspace::Validate(workspace.State(), error)) << error;
}

TEST(DockWorkspaceTest, SplitterRespectsBothSubtreeMinimumWidths)
{
    DockWorkspace workspace;
    const auto root = workspace.State().root;
    ASSERT_TRUE(workspace.ResizeSplit(root, -100, {0, 0, 1280, 654}));
    auto geometry = workspace.Layout({0, 0, 1280, 654});
    EXPECT_GE(geometry.stacks.front().rect.width, 260);
    ASSERT_TRUE(workspace.ResizeSplit(root, 2000, {0, 0, 1280, 654}));
    geometry = workspace.Layout({0, 0, 1280, 654});
    EXPECT_GE(geometry.stacks.back().rect.width, 220);
}

TEST(DockWorkspaceTest, SmallWindowProducesOnlyNonnegativeContainedRects)
{
    DockWorkspace workspace;
    ASSERT_TRUE(workspace.Dock("renderer", workspace.FindStack("viewport"), DockEdge::Left));
    const auto geometry = workspace.Layout({0, 0, 320, 200});
    for (const auto& stack : geometry.stacks)
    {
        EXPECT_GE(stack.rect.width, 0);
        EXPECT_GE(stack.rect.height, 0);
        EXPECT_LE(stack.rect.x + stack.rect.width, 320.01f);
        EXPECT_LE(stack.rect.y + stack.rect.height, 200.01f);
    }
}

TEST(DockWorkspaceTest, RestoreRejectsCyclesWithoutChangingCurrentState)
{
    DockWorkspace workspace;
    auto candidate = workspace.State();
    candidate.nodes.at(candidate.root).first = candidate.root;
    std::string error;
    EXPECT_FALSE(workspace.Restore(candidate, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(workspace.FindStack("viewport"), 1u);
    EXPECT_EQ(workspace.State().nodes.size(), 7u);
}

TEST(DockWorkspaceTest, RestoreRejectsMissingDuplicateUnknownPanelsAndNonfiniteRatio)
{
    DockWorkspace workspace;
    std::string error;
    auto candidate = workspace.State();
    candidate.nodes.at(workspace.FindStack("content")).tabs.push_back("viewport");
    EXPECT_FALSE(workspace.Restore(candidate, error));
    candidate = workspace.State();
    candidate.nodes.at(workspace.FindStack("content")).tabs.push_back("unknown");
    EXPECT_FALSE(workspace.Restore(candidate, error));
    candidate = workspace.State();
    candidate.nodes.at(candidate.root).ratio = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(workspace.Restore(candidate, error));
}

TEST(DockWorkspaceTest, EdgeDockIntoFloatingStackReturnsItToTheRootTree)
{
    DockWorkspace workspace;
    ASSERT_TRUE(workspace.Float("details", {0.2f, 0.2f, 0.4f, 0.5f}));
    ASSERT_TRUE(workspace.Dock("renderer", workspace.FindStack("details"), DockEdge::Bottom));
    EXPECT_TRUE(workspace.State().floating.empty());
    std::string error;
    EXPECT_TRUE(DockWorkspace::Validate(workspace.State(), error)) << error;
}

TEST(DockWorkspaceTest, LastRootTabCannotFloatOrDisappearIntoFloatingStack)
{
    DockWorkspace workspace;
    // resource 已不在常驻面板集合里（资产编辑器按需创建），因此这里只遍历常驻的那四个。
    for (const char* panel : {"outliner", "details", "content", "renderer"})
        ASSERT_TRUE(workspace.Float(panel, {0.2f, 0.2f, 0.4f, 0.5f}));
    EXPECT_FALSE(workspace.Float("viewport", {0, 0, 0.5f, 0.5f}));
    EXPECT_FALSE(workspace.Dock("viewport", workspace.FindStack("details"), DockEdge::Center));
    std::string error;
    EXPECT_TRUE(DockWorkspace::Validate(workspace.State(), error)) << error;
}

class DockPersistenceTest : public testing::Test
{
protected:
    void SetUp() override
    {
        path = std::filesystem::temp_directory_path() /
            ("vl-workspace-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    }
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(std::filesystem::path(path.wstring() + L".tmp"), error);
        std::filesystem::remove(std::filesystem::path(path.wstring() + L".bak"), error);
    }
    std::filesystem::path path;
};

TEST_F(DockPersistenceTest, JsonRoundTripRetainsSplitRatioActiveTabAndFloatingPosition)
{
    DockWorkspace workspace;
    ASSERT_TRUE(workspace.Activate("renderer"));
    ASSERT_TRUE(workspace.ResizeSplit(workspace.State().root, 800, {0, 0, 1280, 654}));
    ASSERT_TRUE(workspace.Float("details", {0.2f, 0.1f, 0.4f, 0.6f}));
    std::string error;
    ASSERT_TRUE(WriteWorkspaceLayout(path, workspace.State(), error)) << error;
    DockLayoutState restored;
    ASSERT_TRUE(ReadWorkspaceLayout(path, restored, error)) << error;
    DockWorkspace actual;
    ASSERT_TRUE(actual.Restore(std::move(restored), error)) << error;
    EXPECT_EQ(actual.State().nodes.at(actual.FindStack("renderer")).active, "renderer");
    EXPECT_FLOAT_EQ(actual.State().nodes.at(actual.State().root).ratio,
        workspace.State().nodes.at(workspace.State().root).ratio);
    ASSERT_EQ(actual.State().floating.size(), 1u);
    EXPECT_FLOAT_EQ(actual.State().floating[0].rect.y, 0.1f);
    ASSERT_TRUE(WriteWorkspaceLayout(path, actual.State(), error)) << error;
}

TEST_F(DockPersistenceTest, InvalidJsonNeverReplacesOutputState)
{
    std::ofstream stream(path);
    stream << "{\"version\":1,\"root\":{\"tabs\":[\"viewport\"],\"active\":\"missing\"},\"floating\":[]}";
    stream.close();
    DockWorkspace workspace;
    DockLayoutState output = workspace.State();
    std::string error;
    EXPECT_FALSE(ReadWorkspaceLayout(path, output, error));
    EXPECT_EQ(output.nodes.size(), 7u);
    EXPECT_FALSE(error.empty());
}

// 可见性由 EditorDockingController 拥有，但规则本身属于停靠布局，因此与布局树共用一个套件名。
// 用例的公共起点：空持久化路径（不写用户目录），并只留下 Viewport 可见。
void HideAllButViewport(EditorDockingController& docking)
{
    docking.SetPersistencePath({});
    // resource 已不在常驻面板集合里（资产编辑器按需创建），因此这里只遍历常驻的那四个。
    for (const char* panel : {"outliner", "details", "content", "renderer"})
        EXPECT_TRUE(docking.SetPanelVisible(panel, false)) << panel;
}

TEST(DockWorkspaceTest, HidingKeepsTheTreePositionAndHandsActivationToAVisibleTab)
{
    EditorDockingController docking;
    HideAllButViewport(docking);
    EXPECT_TRUE(docking.IsPanelVisible("viewport"));
    EXPECT_FALSE(docking.IsPanelVisible("content"));
    // 隐藏只抑制渲染：标签组位置不变，重新显示时回到同一个栈里。
    EXPECT_EQ(docking.Workspace().FindStack("content"), docking.Workspace().FindStack("renderer"));
    EXPECT_TRUE(docking.IsPanelActive("viewport"));
    EXPECT_FALSE(docking.IsPanelActive("renderer"));
}

TEST(DockWorkspaceTest, ShowingAPanelActivatesItAndKeepsTheStackTrace)
{
    EditorDockingController docking;
    HideAllButViewport(docking);
    ASSERT_TRUE(docking.SetPanelVisible("renderer", true));
    EXPECT_TRUE(docking.IsPanelVisible("renderer"));
    // 显示即激活：Window 菜单勾选与可见标签必须指向同一个面板。
    EXPECT_TRUE(docking.IsPanelActive("renderer"));
    EXPECT_FALSE(docking.IsPanelActive("content"));
    std::string error;
    EXPECT_TRUE(DockWorkspace::Validate(docking.Workspace().State(), error)) << error;
}

TEST(DockWorkspaceTest, RefusesToHideTheLastVisiblePanelAndExplainsWhy)
{
    EditorDockingController docking;
    HideAllButViewport(docking);
    // 只剩 Viewport 可见，再隐藏它必须被拒绝，并给出人类可读的 notice。
    EXPECT_FALSE(docking.SetPanelVisible("viewport", false));
    EXPECT_TRUE(docking.IsPanelVisible("viewport"));
    EXPECT_FALSE(docking.Notice().empty());
    // 常驻面板现在是五个减一个：resource 已移出常驻集合（资产编辑器按需创建）。
    EXPECT_EQ(docking.HiddenPanels().size(), 4u);
}

TEST(DockWorkspaceTest, UnknownPanelsAreRejectedInsteadOfSilentlyIgnored)
{
    EditorDockingController docking;
    HideAllButViewport(docking);
    EXPECT_FALSE(docking.IsPanelVisible("outliner-typo"));
    EXPECT_FALSE(docking.SetPanelVisible("outliner-typo", false));
    EXPECT_FALSE(docking.SetPanelVisible("outliner-typo", true));
}

TEST(DockWorkspaceTest, ResetLayoutShowsEveryPanelAgain)
{
    EditorDockingController docking;
    HideAllButViewport(docking);
    docking.Reset();
    EXPECT_TRUE(docking.HiddenPanels().empty());
    for (const auto& panel : DockWorkspace::Panels())
        EXPECT_TRUE(docking.IsPanelVisible(panel)) << panel;
}

TEST(DockWorkspaceTest, ActivePanelHandsOffInsideItsOwnStack)
{
    EditorDockingController docking;
    docking.SetPersistencePath({});
    // Content/Renderer 同组：隐藏激活标签后激活权必须交给同组仍可见的标签。
    docking.Activate("renderer");
    ASSERT_TRUE(docking.IsPanelActive("renderer"));
    ASSERT_TRUE(docking.SetPanelVisible("renderer", false));
    EXPECT_TRUE(docking.IsPanelActive("content"));
    EXPECT_FALSE(docking.IsPanelActive("renderer"));
}

TEST_F(DockPersistenceTest, VisibilityRoundTripsAndLegacyFilesWithoutHiddenKeyStayReadable)
{
    DockWorkspace workspace;
    const std::vector<std::string> hidden{"outliner", "renderer"};
    std::string error;
    ASSERT_TRUE(WriteWorkspaceLayout(path, workspace.State(), error, &hidden)) << error;
    DockLayoutState restored;
    std::vector<std::string> restoredHidden;
    ASSERT_TRUE(ReadWorkspaceLayout(path, restored, error, &restoredHidden)) << error;
    EXPECT_EQ(restoredHidden, hidden);

    // 没有 "hidden" 键的旧文件（写侧不传隐藏集合时就是这个形状）必须继续可读。
    ASSERT_TRUE(WriteWorkspaceLayout(path, workspace.State(), error)) << error;
    DockLayoutState legacy;
    std::vector<std::string> legacyHidden;
    ASSERT_TRUE(ReadWorkspaceLayout(path, legacy, error, &legacyHidden)) << error;
    EXPECT_TRUE(legacyHidden.empty());
    EXPECT_EQ(legacy.nodes.size(), workspace.State().nodes.size());

    // 未知或重复的面板名整体拒绝，不能静默接受。
    for (const char* list : {"[\"outliner-typo\"]", "[\"outliner\",\"outliner\"]", "[7]"})
    {
        std::ofstream stream(path, std::ios::trunc);
        stream << "{\"version\":1,\"root\":{\"tabs\":[\"viewport\",\"resource\"],\"active\":\"viewport\"},"
                  "\"floating\":[],\"hidden\":" << list << "}";
        stream.close();
        DockLayoutState rejected;
        EXPECT_FALSE(ReadWorkspaceLayout(path, rejected, error)) << list;
        EXPECT_FALSE(error.empty());
    }
}

TEST_F(DockPersistenceTest, RejectsFilesThatHideEveryPanel)
{
    // 运行时拒绝隐藏最后一个可见面板，文件层同样不接受全隐藏的布局。
    std::ofstream stream(path, std::ios::trunc);
    stream << "{\"version\":1,\"root\":{\"tabs\":[\"viewport\"],\"active\":\"viewport\"},"
              "\"floating\":[],\"hidden\":[\"viewport\"]}";
    stream.close();
    DockLayoutState output;
    std::string error;
    EXPECT_FALSE(ReadWorkspaceLayout(path, output, error));
    EXPECT_FALSE(error.empty());
}

class DockTestSystem final : public Rml::SystemInterface
{
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        if (type <= Rml::Log::LT_WARNING) errors.push_back(message);
        return true;
    }
    std::vector<std::string> errors;
};

class DockTestRenderer final : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return ++next; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override
    {
        if (source == SceneViewportTextureSource) { dimensions = {1, 1}; return 1; }
        if (std::filesystem::path(source).extension() == ".png" && std::filesystem::exists(source))
        {
            dimensions = {32, 32};
            return 1;
        }
        return 0;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
    size_t next = 0;
};

class DockRmlTest : public testing::Test
{
protected:
    void SetUp() override
    {
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(&renderer);
        ASSERT_TRUE(Rml::Initialise());
        const std::filesystem::path font("C:/Windows/Fonts/segoeui.ttf");
        if (!std::filesystem::exists(font)) GTEST_SKIP();
        ASSERT_TRUE(Rml::LoadFontFace(font.string()));
        context = Rml::CreateContext("dock-test", {1280, 720});
        ASSERT_NE(context, nullptr);
        document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
        ASSERT_NE(document, nullptr);
        panel = std::make_unique<MaterialInstanceAssetEditorPanel>();
        panel->SetWorkspaceLayoutPath({});
        std::string error;
        ASSERT_TRUE(panel->Attach(*document, error)) << error;
        document->Show();
        Refresh();
        ASSERT_TRUE(system.errors.empty()) << Errors();
    }
    void TearDown() override
    {
        panel.reset();
        if (context) Rml::RemoveContext(context->GetName());
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }
    void Refresh()
    {
        panel->Build(snapshot);
        context->Update();
    }
    std::string Errors() const
    {
        std::string output;
        for (const auto& error : system.errors) output += error + "\n";
        return output;
    }
    Rml::Vector2f Point(const std::string& id) const
    {
        auto* element = document->GetElementById(id);
        const auto origin = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        return origin + Rml::Vector2f(12, 12);
    }
    void Begin(const std::string& id)
    {
        const auto point = Point(id);
        context->ProcessMouseMove(static_cast<int>(point.x), static_cast<int>(point.y), 0);
        context->ProcessMouseButtonDown(0, 0);
    }
    void Move(float x, float y)
    {
        context->ProcessMouseMove(static_cast<int>(x), static_cast<int>(y), 0);
        context->Update();
    }
    void End(float x, float y)
    {
        Move(x, y);
        context->ProcessMouseButtonUp(0, 0);
        Refresh();
    }
    Rml::Element* Stack(const std::string& tab) const
    {
        return document->GetElementById(tab)->GetParentNode()->GetParentNode();
    }
    DockTestSystem system;
    DockTestRenderer renderer;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    std::unique_ptr<MaterialInstanceAssetEditorPanel> panel;
    MaterialInstanceEditorSnapshot snapshot;
};

TEST_F(DockRmlTest, ActualPointerCenterDropStacksTabsAndHidesViewport)
{
    const auto target = Point("workspace-viewport-tab");
    Begin("workspace-outliner-tab");
    Move(target.x, target.y);
    ASSERT_TRUE(panel->IsWorkspaceDragging());
    End(target.x, target.y);
    EXPECT_FALSE(panel->IsWorkspaceDragging());
    EXPECT_EQ(Stack("workspace-outliner-tab"), Stack("workspace-viewport-tab"));
    EXPECT_EQ(panel->GetSceneViewportRect().width, 0u);
    document->GetElementById("workspace-viewport-tab")->DispatchEvent("click", {});
    Refresh();
    EXPECT_GT(panel->GetSceneViewportRect().width, 0u);
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(DockRmlTest, IdleFramesDoNotRewriteWorkspaceLayoutProperties)
{
    const auto writes = panel->WorkspacePropertyWriteCount();
    for (int frame = 0; frame < 100; ++frame) Refresh();
    EXPECT_EQ(panel->WorkspacePropertyWriteCount(), writes);
    context->SetDimensions({960, 540});
    Refresh();
    EXPECT_GT(panel->WorkspacePropertyWriteCount(), writes);
}

TEST_F(DockRmlTest, ActualPointerEdgeDropCreatesPaneWithoutRecreatingBusinessControls)
{
    auto* original = document->GetElementById("asset-search");
    auto* image = document->GetElementById("editor-viewport-image");
    const auto origin = image->GetAbsoluteOffset(Rml::BoxArea::Border);
    Begin("workspace-content-tab");
    End(origin.x + 8, origin.y + 100);
    EXPECT_NE(Stack("workspace-content-tab"), Stack("workspace-viewport-tab"));
    EXPECT_EQ(document->GetElementById("asset-search"), original);
    EXPECT_LT(Stack("workspace-content-tab")->GetAbsoluteOffset(Rml::BoxArea::Border).x,
        Stack("workspace-viewport-tab")->GetAbsoluteOffset(Rml::BoxArea::Border).x);
}

TEST_F(DockRmlTest, ActualPointerFloatAndReattachStayInsideNativeWindow)
{
    Begin("workspace-details-tab");
    End(200, 15);
    auto* floating = Stack("workspace-details-tab");
    ASSERT_TRUE(floating->IsClassSet("floating"));
    const auto origin = floating->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto size = floating->GetBox().GetSize(Rml::BoxArea::Border);
    EXPECT_GE(origin.x, 0);
    EXPECT_GE(origin.y, 41);
    EXPECT_LE(origin.x + size.x, 1280.1f);
    EXPECT_LE(origin.y + size.y, 695.1f);
    const auto target = Point("workspace-outliner-tab");
    Begin("workspace-details-tab");
    End(target.x, target.y);
    EXPECT_EQ(Stack("workspace-details-tab"), Stack("workspace-outliner-tab"));
    EXPECT_FALSE(Stack("workspace-details-tab")->IsClassSet("floating"));
}

TEST_F(DockRmlTest, ActualPointerSplitterChangesViewportAndHonorsMinimum)
{
    auto* splitter = document->GetElementById("dock-split-7");
    ASSERT_NE(splitter, nullptr);
    const auto origin = splitter->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto before = panel->GetSceneViewportRect();
    context->ProcessMouseMove(static_cast<int>(origin.x + 2), static_cast<int>(origin.y + 100), 0);
    context->ProcessMouseButtonDown(0, 0);
    ASSERT_TRUE(panel->IsWorkspaceDragging());
    End(1270, origin.y + 100);
    EXPECT_GT(panel->GetSceneViewportRect().width, before.width);
    EXPECT_GE(Stack("workspace-details-tab")->GetBox().GetSize(Rml::BoxArea::Border).x, 220.0f);
}

TEST_F(DockRmlTest, CancelDragRemovesWindowCaptureWithoutMutatingLayout)
{
    Begin("workspace-outliner-tab");
    Move(100, 120);
    ASSERT_TRUE(panel->IsWorkspaceDragging());
    panel->CancelWorkspaceDrag();
    Refresh();
    EXPECT_FALSE(panel->IsWorkspaceDragging());
    EXPECT_FALSE(document->GetElementById("dock-drag-shield")->IsVisible());
    EXPECT_NE(Stack("workspace-outliner-tab"), Stack("workspace-viewport-tab"));
    context->ProcessMouseButtonUp(0, 0);
}

TEST_F(DockRmlTest, ResetCommandPreservesBusinessDom)
{
    auto* original = document->GetElementById("asset-search");
    Begin("workspace-details-tab");
    End(200, 15);
    ASSERT_TRUE(Stack("workspace-details-tab")->IsClassSet("floating"));
    document->GetElementById("workspace-reset")->DispatchEvent("click", {});
    Refresh();
    EXPECT_FALSE(Stack("workspace-details-tab")->IsClassSet("floating"));
    EXPECT_EQ(document->GetElementById("asset-search"), original);
    EXPECT_NE(Stack("workspace-details-tab"), Stack("workspace-outliner-tab"));
}

TEST_F(DockRmlTest, ResetReparentsFocusedUnfinishedInputWithoutLosingText)
{
    snapshot.activeDocument.emplace();
    auto& asset = *snapshot.activeDocument;
    asset.assetPath = "MI_test.json";
    asset.schemaDigest = "dock-test";
    EditorParameterSnapshot parameter;
    parameter.name = "u_scalar";
    parameter.type = EditorParameterType::Float;
    parameter.effectiveValue = {EditorParameterType::Float, {0.5f, 0, 0, 0}};
    parameter.defaultValue = parameter.effectiveValue;
    asset.parameters.push_back(parameter);
    Refresh();
    auto* control = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("parameter-0-0"));
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(control->Focus());
    control->SetValue("-");
    document->GetElementById("workspace-reset")->DispatchEvent("click", {});
    Refresh();
    EXPECT_EQ(document->GetElementById("parameter-0-0"), control);
    EXPECT_EQ(control->GetValue(), "-");
    EXPECT_EQ(context->GetFocusElement(), control);
}

TEST_F(DockRmlTest, DirtyCloseModalBlocksWorkspaceCommandsAndCapture)
{
    snapshot.documentTabs.push_back({"MI_dirty.json", true});
    Refresh();
    auto* close = document->QuerySelector("[data-action=close]");
    ASSERT_NE(close, nullptr);
    close->DispatchEvent("click", {});
    Refresh();
    ASSERT_TRUE(panel->HasModal());
    auto* original = Stack("workspace-details-tab");
    document->GetElementById("workspace-reset")->DispatchEvent("click", {});
    Begin("workspace-details-tab");
    Move(100, 120);
    EXPECT_FALSE(panel->IsWorkspaceDragging());
    EXPECT_EQ(Stack("workspace-details-tab"), original);
    context->ProcessMouseButtonUp(0, 0);
}

TEST_F(DockRmlTest, ViewportRectUsesImageInteriorAndRejectsHiddenDocument)
{
    auto* image = document->GetElementById("editor-viewport-image");
    const auto origin = image->GetAbsoluteOffset(Rml::BoxArea::Content);
    const auto size = image->GetBox().GetSize(Rml::BoxArea::Content);
    const auto rect = panel->GetSceneViewportRect();
    EXPECT_NEAR(rect.x, origin.x, 1);
    EXPECT_NEAR(rect.y, origin.y, 1);
    EXPECT_NEAR(rect.width, size.x, 2);
    EXPECT_NEAR(rect.height, size.y, 2);
    EXPECT_TRUE(panel->IsSceneViewportElement(image));
    EXPECT_FALSE(panel->IsSceneViewportElement(document->GetElementById("workspace-viewport-tab")));
    document->Hide();
    context->Update();
    EXPECT_EQ(panel->GetSceneViewportRect().width, 0u);
}

TEST_F(DockRmlTest, ValidJsonRestoresFloatingPanelsAndActiveTabsOnAttach)
{
    const auto path = std::filesystem::temp_directory_path() /
        ("vl-rml-layout-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    DockWorkspace layout;
    ASSERT_TRUE(layout.Float("details", {0.2f, 0.2f, 0.4f, 0.5f}));
    ASSERT_TRUE(layout.Activate("renderer"));
    std::string error;
    ASSERT_TRUE(WriteWorkspaceLayout(path, layout.State(), error)) << error;
    panel->SetWorkspaceLayoutPath(path);
    ASSERT_TRUE(panel->Attach(*document, error)) << error;
    Refresh();
    EXPECT_TRUE(Stack("workspace-details-tab")->IsClassSet("floating"));
    EXPECT_TRUE(document->GetElementById("workspace-renderer-tab")->IsClassSet("selected"));
    EXPECT_TRUE(document->GetElementById("renderer-page")->IsVisible());
    EXPECT_FALSE(document->GetElementById("content-browser-page")->IsVisible());
    panel->SetWorkspaceLayoutPath({});
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
}

TEST_F(DockRmlTest, HidingAPanelSuppressesItsTabAndBodyWithoutMovingTheStack)
{
    auto* outlinerStack = Stack("workspace-outliner-tab");
    ASSERT_TRUE(panel->SetPanelVisible("outliner", false));
    Refresh();
    EXPECT_FALSE(document->GetElementById("workspace-outliner-tab")->IsVisible());
    EXPECT_FALSE(document->GetElementById("editor-navigation")->IsVisible());
    EXPECT_EQ(Stack("workspace-outliner-tab"), outlinerStack);
    EXPECT_TRUE(document->GetElementById("workspace-details-tab")->IsVisible());

    ASSERT_TRUE(panel->SetPanelVisible("outliner", true));
    Refresh();
    EXPECT_TRUE(document->GetElementById("workspace-outliner-tab")->IsVisible());
    EXPECT_TRUE(document->GetElementById("editor-navigation")->IsVisible());
    EXPECT_TRUE(document->GetElementById("workspace-outliner-tab")->IsClassSet("selected"));
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(DockRmlTest, FullyHiddenStackCollapsesOutOfGeometryAndComesBack)
{
    auto* outlinerStack = Stack("workspace-outliner-tab");
    const float outlinerHeight = outlinerStack->GetBox().GetSize(Rml::BoxArea::Border).y;
    const float columnTop = outlinerStack->GetAbsoluteOffset(Rml::BoxArea::Border).y;
    const float detailsHeight = Stack("workspace-details-tab")->GetBox().GetSize(Rml::BoxArea::Border).y;
    ASSERT_GT(outlinerHeight, 80.0f);
    // Outliner 是右列上半区；整组隐藏后 Details 必须上移接管整列（含原分隔条），
    // 而不是在原地留下一个空灰框。
    ASSERT_TRUE(panel->SetPanelVisible("outliner", false));
    Refresh();
    EXPECT_FALSE(Stack("workspace-outliner-tab")->IsVisible());
    EXPECT_NEAR(Stack("workspace-details-tab")->GetAbsoluteOffset(Rml::BoxArea::Border).y,
        columnTop, 1.0f);
    EXPECT_NEAR(Stack("workspace-details-tab")->GetBox().GetSize(Rml::BoxArea::Border).y,
        detailsHeight + outlinerHeight + 6.0f, 2.0f);

    ASSERT_TRUE(panel->SetPanelVisible("outliner", true));
    Refresh();
    EXPECT_TRUE(Stack("workspace-outliner-tab")->IsVisible());
    EXPECT_NEAR(Stack("workspace-details-tab")->GetAbsoluteOffset(Rml::BoxArea::Border).y,
        columnTop + outlinerHeight + 6.0f, 1.0f);
    EXPECT_NEAR(Stack("workspace-details-tab")->GetBox().GetSize(Rml::BoxArea::Border).y, detailsHeight, 1.0f);
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(DockRmlTest, WindowMenuResetAndRestoreFollowTheWorkspaceFile)
{
    const auto path = std::filesystem::temp_directory_path() /
        ("vl-rml-visibility-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    panel->SetWorkspaceLayoutPath(path);

    // Reset Layout 回到默认布局：全部面板重新可见，并把该布局写回持久化文件。
    ASSERT_TRUE(panel->SetPanelVisible("outliner", false));
    Refresh();
    document->GetElementById("menu-window-reset")->DispatchEvent("click", {});
    Refresh();
    EXPECT_TRUE(panel->IsPanelVisible("outliner"));
    EXPECT_FALSE(Stack("workspace-details-tab")->IsClassSet("floating"));

    // 磁盘上换成另一份布局：浮动 Details 且 Renderer 隐藏。
    DockWorkspace layout;
    ASSERT_TRUE(layout.Float("details", {0.2f, 0.2f, 0.4f, 0.5f}));
    const std::vector<std::string> hidden{"renderer"};
    std::string error;
    ASSERT_TRUE(WriteWorkspaceLayout(path, layout.State(), error, &hidden)) << error;

    // Restore Saved Layout 重新读取文件：浮动位置和隐藏集合都回来。
    document->GetElementById("menu-window-restore")->DispatchEvent("click", {});
    Refresh();
    EXPECT_TRUE(Stack("workspace-details-tab")->IsClassSet("floating"));
    EXPECT_FALSE(panel->IsPanelVisible("renderer"));
    EXPECT_FALSE(document->GetElementById("workspace-renderer-tab")->IsVisible());

    panel->SetWorkspaceLayoutPath({});
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
}

} // namespace

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "editor/ui/editorContentBrowserController.h"
#include "editor/ui/materialInstanceEditorView.h"

namespace
{
using namespace VL::EditorUi;

constexpr std::array<EditorAssetKind, 5> Kinds{
    EditorAssetKind::MaterialInstance, EditorAssetKind::Mesh, EditorAssetKind::Scene,
    EditorAssetKind::Texture, EditorAssetKind::Terrain
};
constexpr std::array<const char*, 5> Filters{"material", "mesh", "scene", "texture", "terrain"};

// 独立固定布局只测试 controller 的工作量与窗口契约，不受 docking 或真实资产 IO 影响。
const char* const BrowserDocument = R"(
<rml><head><style>
body { width: 100%; height: 100%; font-family: Segoe UI; font-size: 12px; }
scrollbarvertical { width: 10px; margin-left: 2px; }
scrollbarhorizontal { height: 10px; margin-top: 2px; }
scrollbarvertical sliderbar { width: 8px; min-height: 24px; }
scrollbarhorizontal sliderbar { height: 8px; min-width: 24px; }
sliderarrowdec, sliderarrowinc { display: none; }
#asset-list { position: absolute; left: 0px; top: 0px; width: 600px; height: 360px;
    overflow: auto; padding: 6px; box-sizing: border-box; }
#asset-list.browser-grid-view { display: flex; flex-direction: row; flex-wrap: wrap;
    align-content: flex-start; align-items: flex-start; }
#asset-list.browser-list-view { display: block; }
.asset-tile { display: flex; flex-direction: column; flex-shrink: 0; box-sizing: border-box;
    width: 92px; height: 116px; margin: 0px 5px 6px 0px; overflow: hidden; }
.browser-size-medium .asset-tile { width: 116px; height: 142px; }
.browser-size-large .asset-tile { width: 140px; height: 166px; }
.asset-row { display: flex; box-sizing: border-box; width: 100%; height: 29px;
    margin-bottom: 1px; overflow: hidden; }
.asset-preview { width: 16px; height: 16px; flex-shrink: 0; }
.asset-name, .asset-kind { display: block; overflow: hidden; }
.asset-dirty { width: 8px; height: 12px; }
.browser-spacer { display: block; flex-basis: 100%; width: 100%; flex-shrink: 0; pointer-events: none; }
#browser-sources-panel { position: absolute; left: 620px; top: 0px; width: 120px; height: 360px; }
#test-controls { position: absolute; left: 0px; top: 400px; }
</style></head><body>
<div id="asset-list"></div><div id="asset-empty"></div>
<div id="browser-sources-panel"><div id="asset-sources"></div></div>
<div id="test-controls">
<div id="asset-breadcrumbs"></div><div id="asset-count"></div><div id="browser-selection"></div>
<input id="asset-search" type="text" data-action="asset-search"/>
<input id="browser-recursive" type="checkbox" data-action="browser-recursive" checked="checked"/>
<button id="browser-back" data-action="browser-back">Back</button>
<button id="browser-forward" data-action="browser-forward">Forward</button>
<button id="browser-up" data-action="browser-up">Up</button>
<button id="browser-sources-toggle" data-action="browser-sources-toggle">Sources</button>
<button id="browser-grid" data-action="browser-grid">Grid</button>
<button id="browser-list" data-action="browser-list">List</button>
<button id="browser-refresh" data-action="browser-refresh">Refresh</button>
<select id="browser-sort" data-action="browser-sort">
<option value="name-asc">Ascending</option><option value="name-desc">Descending</option></select>
<select id="browser-size" data-action="browser-size">
<option value="small">Small</option><option value="medium">Medium</option>
<option value="large">Large</option></select>
<select id="browser-type" data-action="browser-type">
<option value="all">All</option><option value="material">Material</option>
<option value="mesh">Mesh</option><option value="scene">Scene</option>
<option value="texture">Texture</option><option value="terrain">Terrain</option></select>
</div></body></rml>)";

std::string AssetName(size_t index)
{
    return "Item_" + std::to_string(100000 + index).substr(1);
}

std::string AssetPath(size_t index)
{
    const char separator = index % 13 == 0 ? '\\' : '/';
    return std::string("Library") + separator + "Folder_" + std::to_string(index % 25) +
        separator + AssetName(index) + ".json";
}

std::shared_ptr<const EditorJsonAssetCatalogSnapshot> MakeCatalog(size_t count, uint64_t revision)
{
    auto result = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    result->revision = revision;
    result->assets.reserve(count);
    for (size_t index = 0; index < count; ++index)
        result->assets.push_back({AssetPath(index), Kinds[index % Kinds.size()], AssetName(index)});
    return result;
}

class PerformanceSystem final : public Rml::SystemInterface
{
public:
    double GetElapsedTime() override { return time; }
    bool LogMessage(Rml::Log::Type type, const Rml::String& text) override
    {
        if (type <= Rml::Log::LT_WARNING) errors += text + "\n";
        return true;
    }
    double time = 1.0;
    std::string errors;
};

class PerformanceRenderer final : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String&) override
    {
        dimensions = {64, 64};
        return 1;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

class ContentBrowserPerformanceTest : public testing::TestWithParam<size_t>, public Rml::EventListener
{
protected:
    void SetUp() override
    {
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(&renderer);
        ASSERT_TRUE(Rml::Initialise());
        if (!std::filesystem::exists("C:/Windows/Fonts/segoeui.ttf"))
            GTEST_SKIP() << "Segoe UI is unavailable.";
        ASSERT_TRUE(Rml::LoadFontFace("C:/Windows/Fonts/segoeui.ttf"));
        context = Rml::CreateContext("browser-performance-test", {800, 600});
        ASSERT_NE(context, nullptr);
        document = context->LoadDocumentFromMemory(BrowserDocument);
        ASSERT_NE(document, nullptr);
        std::string error;
        ASSERT_TRUE(browser.Attach(*document, error)) << error;
        for (const char* type : {"click", "dblclick", "change"}) document->AddEventListener(type, this);
        snapshot.assetCatalog = MakeCatalog(GetParam(), 1);
        document->Show();
    }

    void TearDown() override
    {
        if (document != nullptr)
            for (const char* type : {"click", "dblclick", "change"}) document->RemoveEventListener(type, this);
        browser.Detach();
        if (context != nullptr) Rml::RemoveContext(context->GetName());
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }

    void ProcessEvent(Rml::Event& event) override
    {
        std::optional<std::string> opened;
        if (browser.ProcessEvent(event, opened)) event.StopPropagation();
        if (opened) openedPaths.push_back(*opened);
    }

    void Frame()
    {
        browser.Build(snapshot);
        context->Update();
        ASSERT_TRUE(system.errors.empty()) << system.errors;
    }

    void Settle()
    {
        // 首帧尺寸与 scrollbar 要经过 RmlUi layout；随后窗口应保持稳定。
        for (int frame = 0; frame < 4; ++frame) Frame();
    }

    Rml::Element* List() const { return document->GetElementById("asset-list"); }

    Rml::ElementList Items() const
    {
        Rml::ElementList result;
        List()->QuerySelectorAll(result, "[data-action=browser-item]");
        return result;
    }

    void Click(const char* id)
    {
        document->GetElementById(id)->DispatchEvent("click", {});
        Settle();
    }

    void SetValue(const char* id, const std::string& value)
    {
        auto* control = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById(id));
        ASSERT_NE(control, nullptr);
        control->SetValue(value);
        control->DispatchEvent("change", {{"value", Rml::Variant(value)}});
        Settle();
    }

    void ExpectStableWork(const EditorContentBrowserController::Stats& before) const
    {
        const auto& after = browser.GetStats();
        EXPECT_EQ(after.catalogRebuilds, before.catalogRebuilds);
        EXPECT_EQ(after.entryRebuilds, before.entryRebuilds);
        EXPECT_EQ(after.entriesMarkupBuilds, before.entriesMarkupBuilds);
        EXPECT_EQ(after.listDomRebuilds, before.listDomRebuilds);
        EXPECT_EQ(after.sourcesMarkupBuilds, before.sourcesMarkupBuilds);
        EXPECT_EQ(after.breadcrumbMarkupBuilds, before.breadcrumbMarkupBuilds);
    }

    void ExpectBoundedWindow() const
    {
        const auto items = Items();
        EXPECT_FALSE(items.empty());
        EXPECT_LE(items.size(), 192u);
        // spacer 是最多两个直接子节点，不是资产，也不参与事件地址映射。
        EXPECT_LE(List()->GetNumChildren(), 194);
        EXPECT_EQ(browser.GetStats().visibleItems, items.size());
        EXPECT_LT(items.size(), GetParam());
    }

    Rml::Element* FirstOnscreenItem() const
    {
        const float top = List()->GetAbsoluteOffset(Rml::BoxArea::Content).y;
        const float bottom = top + List()->GetBox().GetSize(Rml::BoxArea::Content).y;
        for (auto* item : Items())
        {
            const float y = item->GetAbsoluteOffset(Rml::BoxArea::Border).y;
            const float height = item->GetBox().GetSize(Rml::BoxArea::Border).y;
            if (y >= top && y + height <= bottom) return item;
        }
        return nullptr;
    }

    void PointerClick(Rml::Element* item)
    {
        ASSERT_NE(item, nullptr);
        const auto origin = item->GetAbsoluteOffset(Rml::BoxArea::Border);
        const auto size = item->GetBox().GetSize(Rml::BoxArea::Border);
        context->ProcessMouseMove(static_cast<int>(origin.x + size.x / 2),
            static_cast<int>(origin.y + size.y / 2), 0);
        context->ProcessMouseButtonDown(0, 0);
        context->ProcessMouseButtonUp(0, 0);
    }

    PerformanceSystem system;
    PerformanceRenderer renderer;
    EditorContentBrowserController browser;
    MaterialInstanceEditorSnapshot snapshot;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    std::vector<std::string> openedPaths;
};

TEST_P(ContentBrowserPerformanceTest, GridAndListDomAreBoundedAtEveryScrollPosition)
{
    Settle();
    EXPECT_EQ(browser.GetStats().matchedItems, GetParam());
    for (const char* mode : {"browser-grid", "browser-list"})
    {
        Click(mode);
        const auto entryRebuilds = browser.GetStats().entryRebuilds;
        for (const float fraction : {0.0f, 0.5f, 1.0f})
        {
            List()->SetScrollTop(List()->GetScrollHeight() * fraction);
            Settle();
            ExpectBoundedWindow();
            EXPECT_EQ(browser.GetStats().entryRebuilds, entryRebuilds);
            EXPECT_EQ(browser.GetStats().matchedItems, GetParam());
        }
        EXPECT_EQ(Items().back()->GetAttribute<std::string>("data-path", ""), AssetPath(GetParam() - 1));
    }
}

TEST_P(ContentBrowserPerformanceTest, StableFramesDoNotRebuildCatalogEntriesOrMarkup)
{
    Settle();
    const auto before = browser.GetStats();
    const auto nodes = Items();
    for (int frame = 0; frame < 120; ++frame) Frame();
    ExpectStableWork(before);
    EXPECT_EQ(Items(), nodes);
}

TEST_P(ContentBrowserPerformanceTest, GridSpacersPreserveWholeRowPositionsAndScrollExtent)
{
    Settle();
    const auto marginSize = Items().front()->GetBox().GetSize(Rml::BoxArea::Margin);
    const auto& box = List()->GetBox();
    const float paddingX = box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Left) +
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Right);
    const float paddingTop = box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top);
    const float paddingY = paddingTop + box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom);
    const auto columns = static_cast<size_t>((List()->GetClientWidth() - paddingX) / marginSize.x);
    ASSERT_GT(columns, 0u);
    const size_t rows = (GetParam() + columns - 1) / columns;
    const float scrollHeight = List()->GetScrollHeight();
    // RmlUi overflow 对末行 margin 的取舍可差一个行距，不能随虚拟窗口缩短总高度。
    EXPECT_NEAR(scrollHeight, static_cast<float>(rows) * marginSize.y + paddingY, marginSize.y);
    List()->SetScrollTop(1000.0f * marginSize.y + paddingTop);
    Settle();
    const auto firstRow = static_cast<size_t>((List()->GetScrollTop() - paddingTop) / marginSize.y) - 2;
    auto* first = Items().front();
    EXPECT_EQ(first->GetAttribute<std::string>("data-path", ""), AssetPath(firstRow * columns));
    Rml::ElementList spacers;
    List()->QuerySelectorAll(spacers, ".browser-spacer");
    ASSERT_EQ(spacers.size(), 2u);
    EXPECT_NEAR(spacers.front()->GetBox().GetSize(Rml::BoxArea::Content).y,
        static_cast<float>(firstRow) * marginSize.y, 1.0f);
    EXPECT_NEAR(first->GetAbsoluteOffset(Rml::BoxArea::Border).y + List()->GetScrollTop() -
        List()->GetAbsoluteOffset(Rml::BoxArea::Content).y, static_cast<float>(firstRow) * marginSize.y, 1.0f);
    EXPECT_NEAR(List()->GetScrollHeight(), scrollHeight, 1.0f);
}

TEST_P(ContentBrowserPerformanceTest, ScrollUsesFullSortedIdentityAndRejectsRewrittenAddress)
{
    Settle();
    Click("browser-list");
    const float rowHeight = Items().front()->GetBox().GetSize(Rml::BoxArea::Margin).y;
    ASSERT_GT(rowHeight, 0.0f);
    const float paddingTop = List()->GetBox().GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top);
    List()->SetScrollTop(1000.0f * rowHeight + paddingTop);
    Settle();
    ASSERT_FALSE(Items().empty());
    const auto expectedRow = static_cast<size_t>((List()->GetScrollTop() - paddingTop) / rowHeight) - 2;
    auto* item = Items().front();
    const std::string path = item->GetAttribute<std::string>("data-path", "");
    EXPECT_EQ(path, AssetPath(expectedRow));
    item->QuerySelector(".asset-name")->DispatchEvent("dblclick", {});
    ASSERT_EQ(openedPaths.size(), 1u);
    EXPECT_EQ(openedPaths.back(), path);
    item->SetAttribute("data-path", AssetPath(GetParam() - 1));
    item->DispatchEvent("dblclick", {});
    EXPECT_EQ(openedPaths.size(), 1u);
}

TEST_P(ContentBrowserPerformanceTest, TypeFiltersCountAndReachTheEntireCatalog)
{
    Settle();
    Click("browser-list");
    for (size_t kind = 0; kind < Kinds.size(); ++kind)
    {
        SetValue("browser-type", Filters[kind]);
        const size_t expectedCount = GetParam() / Kinds.size();
        EXPECT_EQ(browser.GetStats().matchedItems, expectedCount);
        EXPECT_EQ(document->GetElementById("asset-count")->GetInnerRML(),
            std::to_string(expectedCount) + " assets");
        List()->SetScrollTop(List()->GetScrollHeight());
        Settle();
        ExpectBoundedWindow();
        const auto items = Items();
        for (auto* item : items)
        {
            EXPECT_TRUE(item->IsClassSet("asset-kind-" + std::string(Filters[kind])));
            EXPECT_EQ(item->QuerySelector(".asset-kind")->GetInnerRML(), EditorAssetKindName(Kinds[kind]));
            EXPECT_EQ(item->QuerySelector(".asset-preview")->GetAttribute<std::string>("src", ""),
                EditorAssetKindIcon(Kinds[kind]));
        }
        EXPECT_EQ(items.back()->GetAttribute<std::string>("data-path", ""),
            AssetPath(GetParam() - Kinds.size() + kind));
    }
    SetValue("browser-type", "all");
    EXPECT_EQ(browser.GetStats().matchedItems, GetParam());
    EXPECT_FLOAT_EQ(List()->GetScrollTop(), 0.0f);
}

TEST_P(ContentBrowserPerformanceTest, DirtyAndSelectionRefreshKeepSamePageNodesAndDoubleClick)
{
    Settle();
    auto* item = Items().front();
    const std::string path = item->GetAttribute<std::string>("data-path", "");
    const auto nodes = Items();
    const auto before = browser.GetStats();
    item->QuerySelector(".asset-name")->DispatchEvent("click", {});
    snapshot.assets = {{path, true}, {"Outside/MI_not_in_catalog.json", true}};
    Frame();
    ExpectStableWork(before);
    EXPECT_EQ(Items(), nodes);
    EXPECT_TRUE(item->IsClassSet("selected"));
    auto* marker = item->QuerySelector(".asset-dirty");
    ASSERT_NE(marker, nullptr);
    EXPECT_TRUE(marker->IsVisible());
    item->QuerySelector(".asset-preview")->DispatchEvent("dblclick", {});
    ASSERT_EQ(openedPaths.size(), 1u);
    EXPECT_EQ(openedPaths.back(), path);
    snapshot.assets.clear();
    Frame();
    ExpectStableWork(before);
    EXPECT_EQ(item->QuerySelector(".asset-dirty"), marker);
    EXPECT_FALSE(marker->IsVisible());
    EXPECT_EQ(browser.GetStats().matchedItems, GetParam());
}

TEST_P(ContentBrowserPerformanceTest, ScrollBetweenPointerClicksCannotOpenAnotherAsset)
{
    Settle();
    Click("browser-list");
    auto* item = FirstOnscreenItem();
    ASSERT_NE(item, nullptr);
    const std::string oldPath = item->GetAttribute<std::string>("data-path", "");
    PointerClick(item);
    EXPECT_TRUE(openedPaths.empty());
    Frame();
    List()->SetScrollTop(30006.0f);
    Settle();
    auto* next = FirstOnscreenItem();
    ASSERT_NE(next, nullptr);
    EXPECT_NE(next->GetAttribute<std::string>("data-path", ""), oldPath);
    system.time += 0.1;
    PointerClick(next);
    Frame();
    EXPECT_TRUE(openedPaths.empty());
    EXPECT_EQ(document->GetElementById("browser-selection")->GetInnerRML(),
        next->GetAttribute<std::string>("data-path", ""));
}

TEST_P(ContentBrowserPerformanceTest, ScrollInvalidatesArmedDoubleClickUntilTheNextSingleClick)
{
    Settle();
    Click("browser-list");
    Items().front()->DispatchEvent("click", {});
    Frame();
    List()->SetScrollTop(30006.0f);
    Settle();
    auto* next = FirstOnscreenItem();
    ASSERT_NE(next, nullptr);
    const std::string path = next->GetAttribute<std::string>("data-path", "");
    next->DispatchEvent("dblclick", {});
    EXPECT_TRUE(openedPaths.empty());
    next->DispatchEvent("click", {});
    Frame();
    next->DispatchEvent("dblclick", {});
    ASSERT_EQ(openedPaths.size(), 1u);
    EXPECT_EQ(openedPaths.back(), path);
}

TEST_P(ContentBrowserPerformanceTest, HiddenBrowserDefersCatalogAndMarkupWorkUntilVisible)
{
    Settle();
    List()->SetProperty("display", "none");
    context->Update();
    const auto before = browser.GetStats();
    snapshot.assetCatalog = MakeCatalog(GetParam(), 2);
    for (int frame = 0; frame < 60; ++frame) Frame();
    ExpectStableWork(before);
    List()->SetProperty("display", "flex");
    context->Update();
    Settle();
    EXPECT_EQ(browser.GetStats().catalogRebuilds, before.catalogRebuilds + 1);
    EXPECT_EQ(browser.GetStats().entryRebuilds, before.entryRebuilds + 1);
    ExpectBoundedWindow();
}

TEST_P(ContentBrowserPerformanceTest, CatalogIdentityChangeRebuildsEvenWithTheSameRevision)
{
    Settle();
    snapshot.assets = {{Items().front()->GetAttribute<std::string>("data-path", ""), true}};
    Frame();
    const auto before = browser.GetStats();
    const auto nodes = Items();
    snapshot.assetCatalog = MakeCatalog(GetParam(), 1);
    Settle();
    EXPECT_EQ(browser.GetStats().catalogRebuilds, before.catalogRebuilds + 1);
    EXPECT_EQ(browser.GetStats().entryRebuilds, before.entryRebuilds + 1);
    EXPECT_EQ(browser.GetStats().listDomRebuilds, before.listDomRebuilds);
    EXPECT_EQ(Items(), nodes);
    EXPECT_TRUE(Items().front()->QuerySelector(".asset-dirty")->IsVisible());
    snapshot.assets.clear();
    Frame();
    EXPECT_FALSE(Items().front()->QuerySelector(".asset-dirty")->IsVisible());
    const auto stable = browser.GetStats();
    for (int frame = 0; frame < 20; ++frame) Frame();
    ExpectStableWork(stable);
}

TEST_P(ContentBrowserPerformanceTest, FirstFrameAndResizeRecalculateWindowsWithoutResorting)
{
    browser.Build(snapshot);
    ExpectBoundedWindow();
    context->Update();
    Settle();
    const auto before = browser.GetStats();
    const size_t initialCount = Items().size();
    List()->SetProperty("width", "320px");
    List()->SetProperty("height", "240px");
    context->Update();
    Settle();
    ExpectBoundedWindow();
    EXPECT_NE(Items().size(), initialCount);
    EXPECT_EQ(browser.GetStats().entryRebuilds, before.entryRebuilds);
    EXPECT_GT(browser.GetStats().listDomRebuilds, before.listDomRebuilds);
    SetValue("browser-size", "large");
    const auto size = Items().front()->GetBox().GetSize(Rml::BoxArea::Border);
    EXPECT_FLOAT_EQ(size.x, 140.0f);
    EXPECT_FLOAT_EQ(size.y, 166.0f);
    EXPECT_EQ(browser.GetStats().entryRebuilds, before.entryRebuilds);
    const auto stable = browser.GetStats();
    for (int frame = 0; frame < 20; ++frame) Frame();
    ExpectStableWork(stable);
}

INSTANTIATE_TEST_SUITE_P(LargeCatalog, ContentBrowserPerformanceTest, testing::Values(size_t{10000}, size_t{50000}));
} // namespace

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
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
#include "editor/ui/editorDockingController.h"
#include "editor/ui/materialInstanceEditorView.h"

namespace
{
using namespace VL::EditorUi;

class BrowserTestSystem final : public Rml::SystemInterface
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

class BrowserTestRenderer final : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override { ++draws; }
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override
    {
        // 真实 RmlUi 负责布局与派发；栅格图标不需要物理窗口或 GPU。
        if (source == "vl://scene-viewport" || std::filesystem::path(source).extension() == ".png")
        {
            dimensions = {64, 64};
            loadedSources.push_back(source);
            return 1;
        }
        return 0;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
    size_t draws = 0;
    std::vector<std::string> loadedSources;
};

class ContentBrowserTest : public testing::Test, public Rml::EventListener
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
        context = Rml::CreateContext("content-browser-test", {1280, 720});
        ASSERT_NE(context, nullptr);
        document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
        ASSERT_NE(document, nullptr);
        docking.SetPersistencePath({});
        std::string error;
        ASSERT_TRUE(docking.Attach(*document, error)) << error;
        ASSERT_TRUE(browser.Attach(*document, error)) << error;
        Listen();
        snapshot.assets = {
            {"Maps/Alpha/Materials/MI_Zebra.json", true},
            {"Common/Materials/MI_shared.json", false},
            {"Maps/Beta/Materials/MI_shared.json", true},
            {"Maps/Alpha/Materials/Sub/MI_deep.json", false},
            {"MI_root.json", false},
            {"Maps/Alphabet/MI_other.json", false}
        };
        document->Show();
        Refresh();
    }

    void TearDown() override
    {
        Unlisten();
        browser.Detach();
        docking.Detach();
        if (context != nullptr) Rml::RemoveContext(context->GetName());
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }

    void ProcessEvent(Rml::Event& event) override
    {
        std::optional<std::string> opened = "stale output";
        lastHandled = browser.ProcessEvent(event, opened);
        if (opened) openedPaths.push_back(*opened);
        if (lastHandled) event.StopPropagation();
    }

    void Listen()
    {
        for (const char* type : {"click", "change", "dblclick"})
            document->AddEventListener(type, this);
    }

    void Unlisten()
    {
        if (document == nullptr) return;
        for (const char* type : {"click", "change", "dblclick"})
            document->RemoveEventListener(type, this);
    }

    void Refresh()
    {
        browser.Build(snapshot);
        docking.Sync();
        context->Update();
        ASSERT_TRUE(system.errors.empty()) << system.errors;
    }

    Rml::ElementList Items(const char* container = "asset-list") const
    {
        Rml::ElementList result;
        document->GetElementById(container)->QuerySelectorAll(result, "[data-action=browser-item]");
        return result;
    }

    std::vector<std::string> Paths(const char* container = "asset-list",
        const char* action = "browser-item") const
    {
        Rml::ElementList elements;
        document->GetElementById(container)->QuerySelectorAll(elements, "[data-action=" + std::string(action) + "]");
        std::vector<std::string> paths;
        for (const auto* element : elements)
            paths.push_back(element->GetAttribute<std::string>("data-path", ""));
        return paths;
    }

    Rml::Element* Find(const char* action, const std::string& path) const
    {
        Rml::ElementList elements;
        document->QuerySelectorAll(elements, "[data-action=" + std::string(action) + "]");
        for (auto* element : elements)
            if (element->GetAttribute<std::string>("data-path", "") == path) return element;
        return nullptr;
    }

    void Send(Rml::Element* element, const char* type = "click")
    {
        ASSERT_NE(element, nullptr);
        lastHandled = false;
        element->DispatchEvent(type, {});
        EXPECT_TRUE(lastHandled);
    }

    void Click(const char* id)
    {
        Send(document->GetElementById(id));
        Refresh();
    }

    void SetValue(const char* id, const std::string& value)
    {
        auto* control = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById(id));
        ASSERT_NE(control, nullptr);
        control->SetValue(value);
        control->DispatchEvent("change", {{"value", Rml::Variant(value)}});
        Refresh();
    }

    void Navigate(const std::string& path)
    {
        size_t offset = 0;
        while (!path.empty())
        {
            const auto separator = path.find('/', offset);
            Send(Find("browser-folder", path.substr(0, separator)));
            Refresh();
            if (separator == std::string::npos) break;
            offset = separator + 1;
        }
    }

    std::string Markup(const char* id) const
    {
        return document->GetElementById(id)->GetInnerRML();
    }

    BrowserTestSystem system;
    BrowserTestRenderer renderer;
    EditorContentBrowserController browser;
    EditorDockingController docking;
    MaterialInstanceEditorSnapshot snapshot;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    bool lastHandled = false;
    std::vector<std::string> openedPaths;
};

TEST_F(ContentBrowserTest, RootDefaultsToAllCatalogAssetsAndNoFolderTiles)
{
    EXPECT_EQ(Items().size(), snapshot.assets.size());
    EXPECT_EQ(Markup("asset-count"), "6 assets");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb"), std::vector<std::string>{""});
    EXPECT_NE(Markup("asset-breadcrumbs").find("Content"), std::string::npos);
    EXPECT_TRUE(document->GetElementById("browser-recursive")->HasAttribute("checked"));
    EXPECT_TRUE(document->GetElementById("asset-list")->IsClassSet("browser-grid-view"));
    EXPECT_TRUE(document->GetElementById("asset-list")->IsClassSet("browser-size-small"));
    EXPECT_TRUE(document->GetElementById("browser-back")->HasAttribute("disabled"));
    EXPECT_TRUE(document->GetElementById("browser-forward")->HasAttribute("disabled"));
    EXPECT_TRUE(document->GetElementById("browser-up")->HasAttribute("disabled"));
    for (const auto* item : Items())
        EXPECT_NE(item->GetInnerRML().find("Material Instance"), std::string::npos);
}

TEST_F(ContentBrowserTest, SourcesDeriveSortedAncestorsAndExpandWithoutNavigation)
{
    EXPECT_EQ(Paths("asset-sources", "browser-folder"), (std::vector<std::string>{"", "Common", "Maps"}));
    Send(Find("browser-folder-expand", "Maps"));
    Refresh();
    EXPECT_EQ(Paths("asset-sources", "browser-folder"),
        (std::vector<std::string>{"", "Common", "Maps", "Maps/Alpha", "Maps/Alphabet", "Maps/Beta"}));
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb"), std::vector<std::string>{""});
    EXPECT_EQ(Find("browser-folder-expand", "Maps")->GetAttribute<std::string>("aria-expanded", ""), "true");
    Send(Find("browser-folder-expand", "Maps"));
    Refresh();
    EXPECT_EQ(Find("browser-folder", "Maps/Alpha"), nullptr);
    EXPECT_EQ(Items().size(), snapshot.assets.size());
}

TEST_F(ContentBrowserTest, NonrecursiveViewPlacesImmediateFoldersBeforeFiles)
{
    Click("browser-recursive");
    EXPECT_FALSE(document->GetElementById("browser-recursive")->HasAttribute("checked"));
    EXPECT_EQ(Paths(), (std::vector<std::string>{"Common", "Maps", "MI_root.json"}));
    Navigate("Maps/Alpha/Materials");
    EXPECT_EQ(Paths(), (std::vector<std::string>{
        "Maps/Alpha/Materials/Sub", "Maps/Alpha/Materials/MI_Zebra.json"}));
    EXPECT_EQ(Markup("asset-count"), "1 folder, 1 asset");
    EXPECT_EQ(Items().front()->QuerySelector(".asset-preview")->GetAttribute<std::string>("src", ""), "icons/folder-tile.png");
}

TEST_F(ContentBrowserTest, RecursiveFolderScopeDoesNotIncludeSiblingPrefix)
{
    Navigate("Maps/Alpha");
    EXPECT_EQ(Paths(), (std::vector<std::string>{
        "Maps/Alpha/Materials/Sub/MI_deep.json", "Maps/Alpha/Materials/MI_Zebra.json"}));
    EXPECT_EQ(Find("browser-item", "Maps/Alphabet/MI_other.json"), nullptr);
}

TEST_F(ContentBrowserTest, SearchIsLocalCaseInsensitiveAndPreservesSources)
{
    const auto* maps = Find("browser-folder", "Maps");
    const auto sources = Markup("asset-sources");
    SetValue("asset-search", "mI_ShArEd");
    EXPECT_EQ(Paths(), (std::vector<std::string>{
        "Common/Materials/MI_shared.json", "Maps/Beta/Materials/MI_shared.json"}));
    EXPECT_EQ(Markup("asset-sources"), sources);
    EXPECT_EQ(Find("browser-folder", "Maps"), maps);
    EXPECT_TRUE(openedPaths.empty());
    SetValue("asset-search", "ALPHA/MATERIALS");
    EXPECT_EQ(Items().size(), 2u);
    EXPECT_EQ(snapshot.assets.size(), 6u);
}

TEST_F(ContentBrowserTest, SearchAndRecursiveCheckboxRespectCurrentFolder)
{
    Navigate("Maps/Alpha/Materials");
    SetValue("asset-search", "DEEP");
    ASSERT_EQ(Items().size(), 1u);
    Click("browser-recursive");
    EXPECT_TRUE(Items().empty());
    EXPECT_EQ(Markup("asset-empty"), "No matching assets.");
    Click("browser-recursive");
    ASSERT_EQ(Items().size(), 1u);
    EXPECT_EQ(Paths().front(), "Maps/Alpha/Materials/Sub/MI_deep.json");
}

TEST_F(ContentBrowserTest, SingleClickSelectsAndNestedDoubleClickOpensExactIdentity)
{
    const std::string path = "Maps/Beta/Materials/MI_shared.json";
    auto* item = Find("browser-item", path);
    ASSERT_NE(item, nullptr);
    auto* image = item->QuerySelector(".asset-preview");
    Send(image);
    EXPECT_TRUE(openedPaths.empty());
    EXPECT_EQ(Find("browser-item", path), item);
    EXPECT_FALSE(item->IsClassSet("selected"));
    Refresh();
    EXPECT_EQ(Find("browser-item", path), item);
    item = Find("browser-item", path);
    EXPECT_TRUE(item->IsClassSet("selected"));
    EXPECT_EQ(Markup("browser-selection"), path);
    Send(item->QuerySelector(".asset-name"), "dblclick");
    ASSERT_EQ(openedPaths.size(), 1u);
    EXPECT_EQ(openedPaths.front(), path);
    EXPECT_NE(openedPaths.front(), "Common/Materials/MI_shared.json");
}

TEST_F(ContentBrowserTest, PointerDoubleClickSurvivesSelectionAndDirtyRefreshBetweenClicks)
{
    ASSERT_FALSE(Items().empty());
    auto* item = Items().front();
    const std::string path = item->GetAttribute<std::string>("data-path", "");
    const auto origin = item->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto size = item->GetBox().GetSize(Rml::BoxArea::Border);
    context->ProcessMouseMove(static_cast<int>(origin.x + size.x / 2),
        static_cast<int>(origin.y + size.y / 2), 0);
    system.time = 1.0;
    context->ProcessMouseButtonDown(0, 0);
    context->ProcessMouseButtonUp(0, 0);
    EXPECT_TRUE(openedPaths.empty());
    const auto originalItems = Items();
    for (auto& asset : snapshot.assets) asset.dirty = !asset.dirty;
    Refresh();
    EXPECT_EQ(Items(), originalItems);
    EXPECT_EQ(Items().front(), item);
    EXPECT_TRUE(item->IsClassSet("selected"));
    system.time += 0.1;
    context->ProcessMouseButtonDown(0, 0);
    context->ProcessMouseButtonUp(0, 0);
    ASSERT_EQ(openedPaths.size(), 1u);
    EXPECT_EQ(openedPaths.front(), path);
}

TEST_F(ContentBrowserTest, FolderTileSelectsThenDoubleClickNavigatesWithoutOpenRequest)
{
    Click("browser-recursive");
    auto* item = Find("browser-item", "Maps");
    ASSERT_NE(item, nullptr);
    Send(item);
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb"), std::vector<std::string>{""});
    Refresh();
    EXPECT_TRUE(Find("browser-item", "Maps")->IsClassSet("selected"));
    Send(Find("browser-item", "Maps"), "dblclick");
    Refresh();
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb"), (std::vector<std::string>{"", "Maps"}));
    EXPECT_EQ(Paths(), (std::vector<std::string>{"Maps/Alpha", "Maps/Alphabet", "Maps/Beta"}));
    EXPECT_TRUE(openedPaths.empty());
}

TEST_F(ContentBrowserTest, TreeNavigationDefersDomReplacementUntilBuild)
{
    auto* maps = Find("browser-folder", "Maps");
    auto* rootItem = Find("browser-item", "MI_root.json");
    Send(maps->QuerySelector("img"));
    EXPECT_EQ(Find("browser-folder", "Maps"), maps);
    EXPECT_EQ(Find("browser-item", "MI_root.json"), rootItem);
    EXPECT_TRUE(openedPaths.empty());
    Refresh();
    EXPECT_EQ(Find("browser-item", "MI_root.json"), nullptr);
    EXPECT_TRUE(Find("browser-folder", "Maps")->IsClassSet("selected"));
}

TEST_F(ContentBrowserTest, BackForwardUpAndBreadcrumbsRetainNavigationHistory)
{
    Navigate("Maps/Alpha/Materials");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb"),
        (std::vector<std::string>{"", "Maps", "Maps/Alpha", "Maps/Alpha/Materials"}));
    Click("browser-back");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps/Alpha");
    Click("browser-forward");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps/Alpha/Materials");
    Click("browser-up");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps/Alpha");
    Send(Find("browser-breadcrumb", ""));
    Refresh();
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb"), std::vector<std::string>{""});
    Click("browser-back");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps/Alpha");
    EXPECT_TRUE(openedPaths.empty());
}

TEST_F(ContentBrowserTest, NewNavigationAfterBackDiscardsForwardBranch)
{
    Navigate("Maps/Alpha");
    Click("browser-back");
    Send(Find("browser-folder", "Maps/Beta"));
    Refresh();
    EXPECT_TRUE(document->GetElementById("browser-forward")->HasAttribute("disabled"));
    Click("browser-back");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps");
    Click("browser-forward");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps/Beta");
}

TEST_F(ContentBrowserTest, SortUsesStemNamesAndKeepsFoldersFirstInBothDirections)
{
    SetValue("browser-sort", "name-desc");
    auto paths = Paths();
    EXPECT_EQ(paths.front(), "Maps/Alpha/Materials/MI_Zebra.json");
    EXPECT_EQ(paths.back(), "Maps/Alpha/Materials/Sub/MI_deep.json");
    Click("browser-recursive");
    EXPECT_EQ(Paths(), (std::vector<std::string>{"Maps", "Common", "MI_root.json"}));
    SetValue("browser-sort", "name-asc");
    EXPECT_EQ(Paths(), (std::vector<std::string>{"Common", "Maps", "MI_root.json"}));
}

TEST_F(ContentBrowserTest, ViewSizeAndSourcesToggleArePresentationOnly)
{
    const auto paths = Paths();
    Click("browser-list");
    auto* list = document->GetElementById("asset-list");
    EXPECT_TRUE(list->IsClassSet("browser-list-view"));
    EXPECT_FALSE(list->IsClassSet("browser-grid-view"));
    ASSERT_FALSE(Items().empty());
    EXPECT_TRUE(Items().front()->IsClassSet("asset-row"));
    EXPECT_EQ(Items().front()->QuerySelector(".asset-preview")->GetAttribute<std::string>("src", ""),
        EditorAssetKindIcon(EditorAssetKind::MaterialInstance));
    SetValue("browser-size", "large");
    EXPECT_TRUE(list->IsClassSet("browser-size-large"));
    EXPECT_FALSE(list->IsClassSet("browser-size-small"));
    EXPECT_FALSE(list->IsClassSet("browser-size-medium"));
    Click("browser-sources-toggle");
    EXPECT_FALSE(document->GetElementById("browser-sources-panel")->IsVisible());
    Click("browser-sources-toggle");
    EXPECT_TRUE(document->GetElementById("browser-sources-panel")->IsVisible());
    Click("browser-grid");
    EXPECT_TRUE(Items().front()->IsClassSet("asset-tile"));
    EXPECT_EQ(Paths(), paths);
    EXPECT_TRUE(openedPaths.empty());
}

TEST_F(ContentBrowserTest, NameDirtyMarkerAndAllPathAttributesAreEscaped)
{
    const std::string path = "Maps/A&\"<folder>/MI_<injected>&\"long.json";
    snapshot.assets = {{path, true}};
    Refresh();
    auto* item = Find("browser-item", path);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->GetAttribute<std::string>("title", ""), path);
    EXPECT_NE(item->QuerySelector(".asset-dirty"), nullptr);
    EXPECT_NE(item->QuerySelector(".asset-name")->GetInnerRML().find("&lt;injected&gt;"), std::string::npos);
    EXPECT_EQ(item->QuerySelector(".asset-name")->GetInnerRML().find(".json"), std::string::npos);
    EXPECT_EQ(document->QuerySelector("injected"), nullptr);
    Navigate("Maps");
    Send(Find("browser-folder", "Maps/A&\"<folder>"));
    Refresh();
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps/A&\"<folder>");
    Send(Find("browser-item", path), "dblclick");
    ASSERT_EQ(openedPaths.size(), 1u);
    EXPECT_EQ(openedPaths.front(), path);
}

TEST_F(ContentBrowserTest, UnchangedBuildPreservesEntryAndTreeDomIdentity)
{
    const auto* item = Find("browser-item", "Maps/Alpha/Materials/MI_Zebra.json");
    const auto* maps = Find("browser-folder", "Maps");
    Refresh();
    const auto before = browser.GetStats();
    for (int frame = 0; frame < 20; ++frame) Refresh();
    EXPECT_EQ(Find("browser-item", "Maps/Alpha/Materials/MI_Zebra.json"), item);
    EXPECT_EQ(Find("browser-folder", "Maps"), maps);
    EXPECT_EQ(browser.GetStats().catalogRebuilds, before.catalogRebuilds);
    EXPECT_EQ(browser.GetStats().entryRebuilds, before.entryRebuilds);
    EXPECT_EQ(browser.GetStats().entriesMarkupBuilds, before.entriesMarkupBuilds);
    EXPECT_EQ(browser.GetStats().sourcesMarkupBuilds, before.sourcesMarkupBuilds);
}

TEST_F(ContentBrowserTest, DirtyFlagRefreshPreservesItemAndMarkerNodes)
{
    const std::string path = "Common/Materials/MI_shared.json";
    auto* item = Find("browser-item", path);
    ASSERT_NE(item, nullptr);
    auto* marker = item->QuerySelector(".asset-dirty");
    ASSERT_NE(marker, nullptr);
    EXPECT_FALSE(marker->IsVisible());
    const auto originalItems = Items();
    snapshot.assets[1].dirty = true;
    Refresh();
    EXPECT_EQ(Items(), originalItems);
    EXPECT_EQ(Find("browser-item", path), item);
    EXPECT_EQ(item->QuerySelector(".asset-dirty"), marker);
    EXPECT_TRUE(marker->IsVisible());
    EXPECT_EQ(marker->GetInnerRML(), "*");
    snapshot.assets[1].dirty = false;
    Refresh();
    EXPECT_EQ(Items(), originalItems);
    EXPECT_EQ(item->QuerySelector(".asset-dirty"), marker);
    EXPECT_FALSE(marker->IsVisible());
}

TEST_F(ContentBrowserTest, AuthoritativeCatalogExcludesOutsideMiOverlayAndShowsAllJsonKinds)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->revision = 7;
    catalog->assets = {
        {"Models/SM_cube.json", EditorAssetKind::Mesh, "Cube"},
        {"Maps/SC_level.json", EditorAssetKind::Scene, "Level"},
        {"Textures/TX_albedo.json", EditorAssetKind::Texture, "Albedo"},
        {"Terrains/TR_hill.json", EditorAssetKind::Terrain, "Hill"},
        {"Materials/MI_paint.json", EditorAssetKind::MaterialInstance, "Paint"}
    };
    snapshot.assetCatalog = catalog;
    snapshot.assets = {{"Materials/MI_paint.json", true}, {"Outside/MI_old.json", true},
        {"Models/SM_cube.json", true}};
    Refresh();
    Refresh();
    EXPECT_EQ(Items().size(), catalog->assets.size());
    EXPECT_EQ(Find("browser-item", "Outside/MI_old.json"), nullptr);
    EXPECT_EQ(Find("browser-folder", "Outside"), nullptr);
    const char* classes[] = {"asset-kind-mesh", "asset-kind-scene", "asset-kind-texture",
        "asset-kind-terrain", "asset-kind-material"};
    for (size_t index = 0; index < catalog->assets.size(); ++index)
    {
        const auto& asset = catalog->assets[index];
        auto* item = Find("browser-item", asset.assetPath);
        ASSERT_NE(item, nullptr);
        EXPECT_TRUE(item->IsClassSet(classes[index]));
        EXPECT_EQ(item->QuerySelector(".asset-kind")->GetInnerRML(), EditorAssetKindName(asset.kind));
        EXPECT_EQ(item->QuerySelector(".asset-preview")->GetAttribute<std::string>("src", ""),
            EditorAssetKindIcon(asset.kind));
        EXPECT_EQ(item->QuerySelector(".asset-name")->GetInnerRML(), asset.name);
        EXPECT_EQ(item->QuerySelector(".asset-dirty")->IsVisible(),
            asset.kind == EditorAssetKind::MaterialInstance);
        Send(item->QuerySelector(".asset-name"), "dblclick");
        ASSERT_FALSE(openedPaths.empty());
        EXPECT_EQ(openedPaths.back(), asset.assetPath);
    }
}

TEST_F(ContentBrowserTest, TypeSelectorUsesCatalogAndKeepsOriginalWindowsPath)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->assets = {
        {"Models\\SM_windows.json", EditorAssetKind::Mesh, ""},
        {"Textures/TX_image.json", EditorAssetKind::Texture, ""},
        {"Materials/MI_paint.json", EditorAssetKind::MaterialInstance, ""}
    };
    snapshot.assetCatalog = catalog;
    Refresh();
    SetValue("browser-type", "mesh");
    EXPECT_EQ(Paths(), std::vector<std::string>{"Models\\SM_windows.json"});
    ASSERT_EQ(Items().size(), 1u);
    EXPECT_TRUE(Items().front()->IsClassSet("asset-kind-mesh"));
    Send(Items().front());
    Refresh();
    EXPECT_EQ(Markup("browser-selection"), "Models\\SM_windows.json");
    Send(Items().front(), "dblclick");
    EXPECT_EQ(openedPaths.back(), "Models\\SM_windows.json");
    SetValue("browser-type", "texture");
    EXPECT_EQ(Paths(), std::vector<std::string>{"Textures/TX_image.json"});
    EXPECT_TRUE(Items().front()->IsClassSet("asset-kind-texture"));
    SetValue("browser-type", "all");
    EXPECT_EQ(Items().size(), 3u);
    EXPECT_EQ(document->GetElementById("asset-list")->GetNumChildren(), 3);
}

TEST_F(ContentBrowserTest, EmptyAuthoritativeCatalogDoesNotFallBackToMiFixture)
{
    snapshot.assetCatalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    Refresh();
    EXPECT_TRUE(Items().empty());
    EXPECT_EQ(Markup("asset-count"), "0 assets");
    EXPECT_EQ(Markup("asset-empty"), "No assets found.");
    EXPECT_EQ(Paths("asset-sources", "browser-folder"), std::vector<std::string>{""});
}

TEST_F(ContentBrowserTest, RefreshClickIsDelegatedWithoutOpenOrLocalRebuild)
{
    const auto before = browser.GetStats();
    lastHandled = true;
    document->GetElementById("browser-refresh")->DispatchEvent("click", {});
    EXPECT_FALSE(lastHandled);
    EXPECT_TRUE(openedPaths.empty());
    Refresh();
    EXPECT_EQ(browser.GetStats().catalogRebuilds, before.catalogRebuilds);
    EXPECT_EQ(browser.GetStats().entryRebuilds, before.entryRebuilds);
}

TEST_F(ContentBrowserTest, UnchangedSnapshotAndSelectionRefreshPreserveListScroll)
{
    snapshot.assets.clear();
    for (int index = 0; index < 40; ++index)
        snapshot.assets.push_back({"Common/MI_scroll_" + std::to_string(index) + ".json", false});
    Refresh();
    Click("browser-list");
    auto* list = document->GetElementById("asset-list");
    list->SetScrollTop(90.0f);
    context->Update();
    // 滚动可能换虚拟页；稳定后的同页刷新才保证节点身份不变。
    Refresh();
    Refresh();
    auto* item = Items().front();
    const float scroll = list->GetScrollTop();
    ASSERT_GT(scroll, 0.0f);
    Refresh();
    EXPECT_EQ(Items().front(), item);
    EXPECT_FLOAT_EQ(list->GetScrollTop(), scroll);
    Send(item);
    Refresh();
    EXPECT_EQ(Items().front(), item);
    EXPECT_TRUE(item->IsClassSet("selected"));
    EXPECT_FLOAT_EQ(list->GetScrollTop(), scroll);
}

TEST_F(ContentBrowserTest, RefreshGuardDoesNotTurnProgrammaticCheckboxSyncIntoAnEdit)
{
    auto* checkbox = document->GetElementById("browser-recursive");
    document->RemoveEventListener("change", this);
    checkbox->RemoveAttribute("checked");
    document->AddEventListener("change", this);
    Refresh();
    EXPECT_TRUE(checkbox->HasAttribute("checked"));
    EXPECT_EQ(Items().size(), snapshot.assets.size());
    Refresh();
    EXPECT_TRUE(checkbox->HasAttribute("checked"));
    EXPECT_TRUE(openedPaths.empty());
}

TEST_F(ContentBrowserTest, AttachDetachRetainsFolderSearchSelectionAndViewState)
{
    Navigate("Maps/Alpha/Materials");
    SetValue("asset-search", "ZEBRA");
    Send(Find("browser-item", "Maps/Alpha/Materials/MI_Zebra.json"));
    Refresh();
    Click("browser-recursive");
    Click("browser-list");
    SetValue("browser-sort", "name-desc");
    SetValue("browser-size", "small");
    Click("browser-sources-toggle");
    Unlisten();
    browser.Detach();
    docking.Detach();
    document->Close();
    document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
    ASSERT_NE(document, nullptr);
    std::string error = "previous error";
    ASSERT_TRUE(docking.Attach(*document, error)) << error;
    ASSERT_TRUE(browser.Attach(*document, error)) << error;
    EXPECT_TRUE(error.empty());
    Listen();
    document->Show();
    Refresh();
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps/Alpha/Materials");
    EXPECT_EQ(Markup("browser-selection"), "Maps/Alpha/Materials/MI_Zebra.json");
    EXPECT_TRUE(Items().front()->IsClassSet("selected"));
    EXPECT_TRUE(Items().front()->IsClassSet("asset-row"));
    EXPECT_FALSE(document->GetElementById("browser-recursive")->HasAttribute("checked"));
    EXPECT_FALSE(document->GetElementById("browser-sources-panel")->IsVisible());
    EXPECT_TRUE(document->GetElementById("asset-list")->IsClassSet("browser-size-small"));
    EXPECT_EQ(dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("asset-search"))->GetValue(), "ZEBRA");
    EXPECT_EQ(dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("browser-sort"))->GetValue(), "name-desc");
    Click("browser-back");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps/Alpha");
}

TEST_F(ContentBrowserTest, RemovedFolderFallsBackToSurvivingAncestorAndClearsSelection)
{
    Navigate("Maps/Alpha/Materials");
    Send(Find("browser-item", "Maps/Alpha/Materials/MI_Zebra.json"));
    snapshot.assets = {{"Maps/Beta/Materials/MI_shared.json", false}};
    Refresh();
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps");
    EXPECT_EQ(Markup("browser-selection"), "No selection");
    Click("browser-back");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb"), std::vector<std::string>{""});
    Click("browser-forward");
    EXPECT_EQ(Paths("asset-breadcrumbs", "browser-breadcrumb").back(), "Maps");
}

TEST_F(ContentBrowserTest, StaleOrForeignItemAddressCannotOpenAnAsset)
{
    auto* item = Find("browser-item", "MI_root.json");
    ASSERT_NE(item, nullptr);
    item->SetAttribute("data-path", "Maps/Missing/MI_removed.json");
    Send(item, "dblclick");
    EXPECT_TRUE(openedPaths.empty());
    auto* outside = document->GetElementById("browser-selection");
    outside->SetAttribute("data-action", "browser-item");
    outside->SetAttribute("data-path", "MI_root.json");
    Send(outside, "dblclick");
    EXPECT_TRUE(openedPaths.empty());
    Refresh();
    SetValue("asset-search", "ZEBRA");
    item = Find("browser-item", "Maps/Alpha/Materials/MI_Zebra.json");
    ASSERT_NE(item, nullptr);
    item->SetAttribute("data-path", "MI_root.json");
    Send(item, "dblclick");
    EXPECT_TRUE(openedPaths.empty());
}

TEST_F(ContentBrowserTest, DetachAndUnknownEventsAreNotHandledAndClearOpenOutput)
{
    auto* item = Find("browser-item", "MI_root.json");
    ASSERT_NE(item, nullptr);
    document->AddEventListener("mousemove", this);
    lastHandled = true;
    item->DispatchEvent("mousemove", {});
    EXPECT_FALSE(lastHandled);
    EXPECT_TRUE(openedPaths.empty());
    document->RemoveEventListener("mousemove", this);
    browser.Detach();
    lastHandled = true;
    item->DispatchEvent("click", {});
    EXPECT_FALSE(lastHandled);
    EXPECT_TRUE(openedPaths.empty());
}

TEST_F(ContentBrowserTest, AttachReportsTheMissingRequiredElement)
{
    auto* missing = document->GetElementById("asset-breadcrumbs");
    auto removed = missing->GetParentNode()->RemoveChild(missing);
    EditorContentBrowserController candidate;
    std::string error;
    EXPECT_FALSE(candidate.Attach(*document, error));
    EXPECT_NE(error.find("asset-breadcrumbs"), std::string::npos);
}

TEST_F(ContentBrowserTest, EmptyAndLoadingCatalogShowExplicitEmptyState)
{
    snapshot.assets.clear();
    snapshot.browserLoading = true;
    Refresh();
    EXPECT_EQ(Markup("asset-empty"), "Loading...");
    EXPECT_EQ(Markup("asset-count"), "0 assets");
    EXPECT_TRUE(document->GetElementById("asset-empty")->IsVisible());
    snapshot.browserLoading = false;
    Refresh();
    EXPECT_EQ(Markup("asset-empty"), "No material instances found.");
    EXPECT_EQ(Paths("asset-sources", "browser-folder"), std::vector<std::string>{""});
}

TEST_F(ContentBrowserTest, WindowsSeparatorsDoNotLoseOriginalAssetIdentity)
{
    const std::string path = "Maps\\Alpha\\Materials\\MI_windows.json";
    snapshot.assets = {{path, false}};
    Refresh();
    Navigate("Maps/Alpha/Materials");
    EXPECT_EQ(Paths(), std::vector<std::string>{path});
    Send(Find("browser-item", path), "dblclick");
    ASSERT_EQ(openedPaths.size(), 1u);
    EXPECT_EQ(openedPaths.front(), path);
}

TEST_F(ContentBrowserTest, GridAndListRenderRasterIconsAtSupportedWindowSizes)
{
    for (const auto dimensions : {Rml::Vector2i(960, 540), Rml::Vector2i(1280, 720)})
    {
        context->SetDimensions(dimensions);
        Refresh();
        for (const char* mode : {"browser-grid", "browser-list"})
        {
            Click(mode);
            auto* list = document->GetElementById("asset-list");
            const auto origin = list->GetAbsoluteOffset(Rml::BoxArea::Content);
            const auto size = list->GetBox().GetSize(Rml::BoxArea::Content);
            EXPECT_GT(size.x, 0.0f);
            EXPECT_GT(size.y, 0.0f);
            EXPECT_GE(origin.x, 0.0f);
            EXPECT_LE(origin.x + size.x, dimensions.x + 1.0f);
            ASSERT_FALSE(Items().empty());
            const auto tileSize = Items().front()->GetBox().GetSize(Rml::BoxArea::Border);
            EXPECT_GT(tileSize.x, 0.0f);
            EXPECT_GT(tileSize.y, 0.0f);
            context->Render();
            EXPECT_GT(renderer.draws, 0u);
            EXPECT_TRUE(system.errors.empty()) << system.errors;
        }
    }
    EXPECT_FALSE(renderer.loadedSources.empty());
}
} // namespace

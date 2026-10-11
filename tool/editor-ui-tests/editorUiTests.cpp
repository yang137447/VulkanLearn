#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "editor/ui/materialInstanceAssetEditorPanel.h"

namespace
{
using namespace VL;
using namespace VL::EditorUi;

class TestSystem final : public Rml::SystemInterface
{
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        if (type <= Rml::Log::LT_WARNING) errors.push_back(message);
        return true;
    }
    std::vector<std::string> errors;
};

class TestRenderer final : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(
        Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return ++next; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override { ++draws; }
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
    size_t draws = 0;
};

class CommandSink final : public IEditorCommandSink
{
public:
    void Submit(EditorCommandEnvelope command) override { commands.push_back(std::move(command)); }
    std::vector<EditorCommandEnvelope> commands;
};

class RuntimeSink final : public IEditorRuntimeActionSink
{
public:
    void RequestRuntimeAction(UiAction action) override { actions.push_back(std::move(action)); }
    std::vector<UiAction> actions;
};

class JsonAssetSink final : public IEditorJsonAssetRequestSink
{
public:
    void RequestOpenJsonAsset(const std::string& path) override { opened.push_back(path); }
    void RequestRefreshAssetCatalog() override { ++refreshes; }
    std::vector<std::string> opened;
    size_t refreshes = 0;
};

class SelectionSink final : public IMaterialInstanceSceneSelectionSink
{
public:
    bool RequestModelSelection(const Editor::Selection::MaterialInstanceModelContext& model) override
    {
        selectedModel = model;
        return true;
    }
    bool RequestMaterialSelection(const Editor::Selection::MaterialInstanceSelection& material) override
    {
        selectedMaterial = material;
        return true;
    }
    std::optional<Editor::Selection::MaterialInstanceModelContext> selectedModel;
    std::optional<Editor::Selection::MaterialInstanceSelection> selectedMaterial;
};

class EditorUiTest : public testing::Test
{
protected:
    void SetUp() override
    {
        Rml::SetSystemInterface(&system);
        Rml::SetRenderInterface(&renderer);
        ASSERT_TRUE(Rml::Initialise());
        const std::filesystem::path font("C:/Windows/Fonts/segoeui.ttf");
        if (!std::filesystem::exists(font)) GTEST_SKIP() << "Segoe UI is unavailable.";
        ASSERT_TRUE(Rml::LoadFontFace(font.string()));
        context = Rml::CreateContext("editor-test", {1280, 720});
        ASSERT_NE(context, nullptr);
        document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
        ASSERT_NE(document, nullptr);
        panel = std::make_unique<MaterialInstanceAssetEditorPanel>(&sink);
        panel->SetWorkspaceLayoutPath({});
        panel->SetRuntimeActionSink(&runtimeSink);
        panel->SetSceneSelectionSink(&selectionSink);
        panel->SetJsonAssetRequestSink(&jsonAssetSink);
        std::string error;
        ASSERT_TRUE(panel->Attach(*document, error)) << error;
        snapshot.activeDocument.emplace();
        auto& asset = *snapshot.activeDocument;
        asset.assetPath = "Maps/SC_test/Materials/MI_test.json";
        asset.baseMaterialPath = "shader/glsl/M_test.json";
        asset.schemaDigest = "schema-one";
        asset.revision = 7;
        asset.status = EditorDocumentStatus::Dirty;
        EditorParameterSnapshot parameter;
        parameter.name = "u_color";
        parameter.type = EditorParameterType::Vec3;
        parameter.effectiveValue = {EditorParameterType::Vec3, {0.1f, 0.2f, 0.3f, 0.0f}};
        parameter.defaultValue = parameter.effectiveValue;
        asset.parameters.push_back(parameter);
        snapshot.selectedDocumentPath = asset.assetPath;
        snapshot.documentTabs.push_back({asset.assetPath, true});
        snapshot.assets.push_back({asset.assetPath, true});
        panel->Build(snapshot);
        document->Show();
        context->Update();
        panel->Build(snapshot);
        context->Update();
        ASSERT_TRUE(system.errors.empty()) << Errors();
    }
    void TearDown() override
    {
        panel.reset();
        if (context != nullptr) Rml::RemoveContext(context->GetName());
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }
    std::string Errors() const
    {
        std::string result;
        for (const auto& error : system.errors) result += error + "\n";
        return result;
    }
    void Refresh()
    {
        panel->Build(snapshot);
        context->Update();
    }
    void SetValue(const std::string& id, const std::string& value)
    {
        auto* control = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById(id));
        ASSERT_NE(control, nullptr);
        control->SetValue(value);
        control->DispatchEvent("change", {});
    }
    void Click(const std::string& selector)
    {
        auto* element = document->QuerySelector(selector);
        ASSERT_NE(element, nullptr);
        element->DispatchEvent("click", {});
    }
    Rml::ElementFormControl* Control(const std::string& id)
    {
        return dynamic_cast<Rml::ElementFormControl*>(document->GetElementById(id));
    }
    void ExpectSliderRange(const std::string& componentId, float minValue, float maxValue)
    {
        auto* slider = Control(componentId + "-slider");
        ASSERT_NE(slider, nullptr) << componentId;
        EXPECT_EQ(slider->GetAttribute<std::string>("type", ""), "range");
        EXPECT_FLOAT_EQ(slider->GetAttribute<float>("min", 0.0f), minValue) << componentId;
        EXPECT_FLOAT_EQ(slider->GetAttribute<float>("max", 0.0f), maxValue) << componentId;
    }
    // 滑条拖动最终也走 WidgetSlider::SetValueInternal → change；直接写控件值命中同一条路径，
    // 不依赖窗口鼠标坐标（指针拖动的覆盖在 dockingTests 里）。
    void DragSlider(const std::string& id, const std::string& value)
    {
        auto* control = Control(id);
        ASSERT_NE(control, nullptr) << id;
        control->SetValue(value);
    }
    std::string Notice() const
    {
        auto* notice = document->GetElementById("editor-notice");
        return notice == nullptr ? std::string() : notice->GetInnerRML();
    }
    TestSystem system;
    TestRenderer renderer;
    CommandSink sink;
    RuntimeSink runtimeSink;
    SelectionSink selectionSink;
    JsonAssetSink jsonAssetSink;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    std::unique_ptr<MaterialInstanceAssetEditorPanel> panel;
    MaterialInstanceEditorSnapshot snapshot;
};

TEST_F(EditorUiTest, WorkspaceParsesAndRendersWithoutWarnings)
{
    context->Render();
    EXPECT_GT(renderer.draws, 0u);
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(EditorUiTest, WorkspaceTabsKeepVisibleTitlesAfterDockShellRebuild)
{
    const std::vector<std::pair<std::string, std::string>> tabs = {
        {"workspace-viewport-tab", "Viewport"},
        {"workspace-outliner-tab", "Scene Outliner"},
        {"workspace-details-tab", "Details"},
        {"workspace-content-tab", "Content Browser"},
        {"workspace-renderer-tab", "Renderer"}};
    for (const auto& [id, title] : tabs)
    {
        auto* tab = document->GetElementById(id);
        ASSERT_NE(tab, nullptr) << id;
        EXPECT_NE(tab->GetInnerRML().find(title), std::string::npos) << id;
        EXPECT_GT(tab->GetBox().GetSize(Rml::BoxArea::Border).x, 0.0f) << id;
    }
}

TEST_F(EditorUiTest, OutlinerAndDetailsShareTheRightColumnWithoutOverlap)
{
    for (const Rml::Vector2i dimensions : {
        Rml::Vector2i(960, 540), Rml::Vector2i(1280, 720), Rml::Vector2i(1920, 1080)})
    {
        context->SetDimensions(dimensions);
        panel->Build(snapshot);
        context->Update();
        auto* outliner = document->GetElementById("workspace-outliner-tab")->GetParentNode()->GetParentNode();
        auto* details = document->GetElementById("workspace-details-tab")->GetParentNode()->GetParentNode();
        const auto outlinerOrigin = outliner->GetAbsoluteOffset(Rml::BoxArea::Border);
        const auto detailsOrigin = details->GetAbsoluteOffset(Rml::BoxArea::Border);
        const auto outlinerSize = outliner->GetBox().GetSize(Rml::BoxArea::Border);
        const auto detailsSize = details->GetBox().GetSize(Rml::BoxArea::Border);
        EXPECT_NEAR(outlinerOrigin.x, (dimensions.x - 6.0f) * 0.69f + 6.0f, 2.0f);
        EXPECT_NEAR(detailsOrigin.x, outlinerOrigin.x, 1.0f);
        EXPECT_NEAR(detailsOrigin.y, outlinerOrigin.y + outlinerSize.y + 6.0f, 1.0f);
        EXPECT_GE(outlinerSize.y, 100.0f);
        EXPECT_GT(detailsSize.y, 170.0f);
        EXPECT_NEAR(detailsOrigin.y + detailsSize.y, dimensions.y - 25.0f, 1.0f);
    }
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(EditorUiTest, ContentDockLeavesAnIndependentSceneViewport)
{
    for (const Rml::Vector2i dimensions : {Rml::Vector2i(960, 540), Rml::Vector2i(1280, 720)})
    {
        context->SetDimensions(dimensions);
        panel->Build(snapshot);
        context->Update();
        auto* content = document->GetElementById("workspace-content-tab")->GetParentNode()->GetParentNode();
        auto* sidebar = document->GetElementById("workspace-outliner-tab")->GetParentNode()->GetParentNode();
        const auto origin = content->GetAbsoluteOffset(Rml::BoxArea::Border);
        const auto size = content->GetBox().GetSize(Rml::BoxArea::Border);
        EXPECT_NEAR(origin.x, 0.0f, 1.0f);
        EXPECT_NEAR(origin.x + size.x + 6.0f, sidebar->GetAbsoluteOffset(Rml::BoxArea::Border).x, 1.0f);
        EXPECT_NEAR(origin.y + size.y, dimensions.y - 25.0f, 1.0f);
        EXPECT_GT(origin.y, 250.0f);
        // 视口内部但避开左上角的工具条覆盖层：工具条属于编辑器，非场景命中。
        const auto* sceneHit = context->GetElementAtPoint({50.0f, 200.0f});
        EXPECT_TRUE(panel->IsSceneViewportElement(sceneHit));
        const auto viewport = panel->GetSceneViewportRect();
        EXPECT_GT(viewport.width, 0u);
        EXPECT_GT(viewport.height, 0u);
        EXPECT_GE(viewport.y, 73);
    }
}

TEST_F(EditorUiTest, WorkspaceTabsSwitchPagesWithoutProducingBusinessCommands)
{
    Click("#workspace-renderer-tab");
    context->Update();
    EXPECT_TRUE(document->GetElementById("workspace-renderer-tab")->IsClassSet("selected"));
    EXPECT_FALSE(document->GetElementById("content-browser-page")->IsVisible());
    EXPECT_TRUE(document->GetElementById("renderer-page")->IsVisible());
    Click("#workspace-content-tab");
    context->Update();
    EXPECT_TRUE(document->GetElementById("workspace-content-tab")->IsClassSet("selected"));
    EXPECT_TRUE(document->GetElementById("content-browser-page")->IsVisible());
    EXPECT_FALSE(document->GetElementById("renderer-page")->IsVisible());
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());
}

TEST_F(EditorUiTest, WorkspaceRefreshAndReattachKeepTheSelectedPage)
{
    Click("#workspace-renderer-tab");
    panel->Build(snapshot);
    std::string error;
    ASSERT_TRUE(panel->Attach(*document, error)) << error;
    context->Update();
    EXPECT_TRUE(document->GetElementById("workspace-renderer-tab")->IsClassSet("selected"));
    EXPECT_TRUE(document->GetElementById("renderer-page")->IsVisible());
    EXPECT_FALSE(document->GetElementById("content-browser-page")->IsVisible());
}

TEST_F(EditorUiTest, BrowserTilesHaveStableDimensions)
{
    auto* row = document->QuerySelector("#asset-list button");
    ASSERT_NE(row, nullptr);
    EXPECT_NEAR(row->GetBox().GetSize(Rml::BoxArea::Border).x, 92.0f, 1.0f);
    EXPECT_NEAR(row->GetBox().GetSize(Rml::BoxArea::Border).y, 116.0f, 1.0f);
}

TEST_F(EditorUiTest, BrowserRecursiveControlKeepsItsCheckboxAndLabelVisible)
{
    for (const Rml::Vector2i dimensions : {Rml::Vector2i(960, 540), Rml::Vector2i(1280, 720)})
    {
        context->SetDimensions(dimensions);
        panel->Build(snapshot);
        context->Update();
        auto* checkbox = document->GetElementById("browser-recursive");
        auto* label = checkbox->GetParentNode()->QuerySelector("span");
        ASSERT_NE(label, nullptr);
        EXPECT_NEAR(checkbox->GetBox().GetSize(Rml::BoxArea::Border).x, 13.0f, 1.0f);
        EXPECT_NEAR(checkbox->GetBox().GetSize(Rml::BoxArea::Border).y, 13.0f, 1.0f);
        EXPECT_GT(label->GetBox().GetSize(Rml::BoxArea::Border).x, 45.0f);
        EXPECT_GT(label->GetAbsoluteOffset().x, checkbox->GetAbsoluteOffset().x);
    }
    context->Render();
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(EditorUiTest, DefaultBrowserDockShowsTheCompleteFirstTileAtSmallWindowSizes)
{
    for (const Rml::Vector2i dimensions : {Rml::Vector2i(960, 540), Rml::Vector2i(1280, 720)})
    {
        context->SetDimensions(dimensions);
        panel->Build(snapshot);
        context->Update();
        auto* list = document->GetElementById("asset-list");
        auto* tile = list->GetChild(0);
        ASSERT_NE(tile, nullptr);
        const float tileBottom = tile->GetAbsoluteOffset(Rml::BoxArea::Border).y +
            tile->GetBox().GetSize(Rml::BoxArea::Border).y;
        const float listBottom = list->GetAbsoluteOffset(Rml::BoxArea::Content).y +
            list->GetBox().GetSize(Rml::BoxArea::Content).y;
        EXPECT_LE(tileBottom, listBottom);
    }
}

TEST_F(EditorUiTest, BrowserTileSelectsOnClickAndOpensOnDoubleClick)
{
    auto* row = document->QuerySelector("#asset-list button");
    ASSERT_NE(row, nullptr);
    const auto origin = row->GetAbsoluteOffset(Rml::BoxArea::Content);
    context->ProcessMouseMove(static_cast<int>(origin.x + 3), static_cast<int>(origin.y + 3), 0);
    context->ProcessMouseButtonDown(0, 0);
    context->ProcessMouseButtonUp(0, 0);
    EXPECT_TRUE(sink.commands.empty());
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(document->QuerySelector("#asset-list button"), row);
    context->ProcessMouseButtonDown(0, 0);
    context->ProcessMouseButtonUp(0, 0);
    ASSERT_EQ(sink.commands.size(), 1u);
    EXPECT_EQ(sink.commands[0].type, EditorCommandType::OpenMaterialInstanceAsset);
}

TEST_F(EditorUiTest, BrowserGridPlacesMultipleTilesOnTheSameRow)
{
    snapshot.assets.push_back({"Maps/SC_test/Materials/MI_other_one.json", false});
    snapshot.assets.push_back({"Maps/SC_test/Materials/MI_other_two.json", false});
    for (const Rml::Vector2i dimensions : {Rml::Vector2i(960, 540), Rml::Vector2i(1280, 720)})
    {
        context->SetDimensions(dimensions);
        panel->Build(snapshot);
        context->Update();
        auto* list = document->GetElementById("asset-list");
        ASSERT_EQ(list->GetNumChildren(), 3);
        SCOPED_TRACE(testing::Message() << "viewport=" << dimensions.x
            << " list=" << list->GetBox().GetSize(Rml::BoxArea::Content).x
            << " parent=" << list->GetParentNode()->GetBox().GetSize(Rml::BoxArea::Content).x
            << " body=" << list->GetParentNode()->GetParentNode()->GetBox().GetSize(Rml::BoxArea::Content).x);
        const auto first = list->GetChild(0)->GetAbsoluteOffset(Rml::BoxArea::Border);
        const auto second = list->GetChild(1)->GetAbsoluteOffset(Rml::BoxArea::Border);
        EXPECT_GT(second.x, first.x + 90.0f);
        EXPECT_NEAR(second.y, first.y, 1.0f);
    }
}

TEST_F(EditorUiTest, MeshLevelAndTextureOpenReadOnlyJsonAndSceneRequestsWorldLoad)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->revision = 1;
    catalog->assets = {
        {"Meshes/SM_test.json", EditorAssetKind::Mesh, "Mesh"},
        {"Maps/SC_test.json", EditorAssetKind::Scene, "Level"},
        {"Textures/T_test.json", EditorAssetKind::Texture, "Texture"}};
    snapshot.assetCatalog = catalog;
    panel->Build(snapshot);
    context->Update();
    for (const auto& asset : catalog->assets)
    {
        Rml::Element* tile = nullptr;
        auto* list = document->GetElementById("asset-list");
        for (int index = 0; index < list->GetNumChildren(); ++index)
            if (list->GetChild(index)->GetAttribute<std::string>("data-path", "") == asset.assetPath)
                tile = list->GetChild(index);
        ASSERT_NE(tile, nullptr);
        tile->DispatchEvent("dblclick", {});
        ASSERT_FALSE(jsonAssetSink.opened.empty());
        EXPECT_EQ(jsonAssetSink.opened.back(), asset.assetPath);
        auto json = std::make_shared<EditorJsonDocumentSnapshot>();
        json->revision = 1;
        json->assetPath = asset.assetPath;
        json->kind = asset.kind;
        json->name = asset.name;
        json->fields.push_back({-1, 0, "", "$", "{1}", EditorJsonValueKind::Object});
        json->fields.push_back({0, 1, "/name", "name", asset.name, EditorJsonValueKind::String});
        snapshot.jsonDocument = json;
        panel->Build(snapshot);
        context->Update();
        EXPECT_TRUE(document->GetElementById("save-asset")->HasAttribute("disabled"));
        if (asset.kind == EditorAssetKind::Mesh || asset.kind == EditorAssetKind::Texture)
        {
            EXPECT_TRUE(document->GetElementById("resource-editor")->IsVisible());
            EXPECT_TRUE(document->GetElementById("resource-json-inspector")->IsVisible());
            EXPECT_NE(document->GetElementById("resource-json-fields")->GetInnerRML().find(asset.name),
                std::string::npos);
        }
        else
        {
            EXPECT_TRUE(document->GetElementById("json-inspector")->IsVisible());
            EXPECT_FALSE(document->GetElementById("material-instance-details")->IsVisible());
            EXPECT_NE(document->GetElementById("json-fields")->GetInnerRML().find(asset.name),
                std::string::npos);
        }
        panel->RequestSave();
    }
    EXPECT_TRUE(sink.commands.empty());
    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.back().type, UiActionType::LoadWorld);
    EXPECT_EQ(runtimeSink.actions.back().stringValue, "Maps/SC_test.json");
}

TEST_F(EditorUiTest, ResourceEditorDeduplicatesTabsAndReturnsToWorld)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->revision = 2;
    catalog->assets.push_back({"Textures/T_test.json", EditorAssetKind::Texture, "Texture"});
    snapshot.assetCatalog = catalog;
    panel->Build(snapshot);
    context->Update();

    auto* item = document->QuerySelector("#asset-list button");
    ASSERT_NE(item, nullptr);
    item->DispatchEvent("dblclick", {});
    auto json = std::make_shared<EditorJsonDocumentSnapshot>();
    json->assetPath = "Textures/T_test.json";
    json->kind = EditorAssetKind::Texture;
    json->name = "Texture";
    snapshot.jsonDocument = json;
    panel->Build(snapshot);
    context->Update();
    ASSERT_TRUE(document->GetElementById("resource-editor")->IsVisible());
    auto* resourceTab = document->QuerySelector(".workspace-resource-tab");
    ASSERT_NE(resourceTab, nullptr);
    EXPECT_TRUE(resourceTab->IsClassSet("selected"));
    EXPECT_NE(resourceTab->GetInnerRML().find("T_test.json"), std::string::npos);
    EXPECT_NE(resourceTab->GetInnerRML().find("icons/texture.png"), std::string::npos);
    const size_t requests = jsonAssetSink.opened.size();

    item->DispatchEvent("dblclick", {});
    EXPECT_EQ(jsonAssetSink.opened.size(), requests);
    EXPECT_NE(document->QuerySelector(".workspace-resource-tab"), nullptr);

    Click("#workspace-viewport-tab");
    context->Update();
    EXPECT_FALSE(document->GetElementById("resource-editor")->IsVisible());
    EXPECT_TRUE(document->GetElementById("dock-shell")->IsVisible());
}

TEST_F(EditorUiTest, RefreshAssetsRequestsOneWorkerRefreshWithoutMiCommands)
{
    Click("#browser-refresh");
    EXPECT_EQ(jsonAssetSink.refreshes, 1u);
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, JsonInspectorEscapesFieldsAndBoundsExpandedArrayPages)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->revision = 1;
    catalog->assets.push_back({"Maps/SC_test.json", EditorAssetKind::Scene});
    snapshot.assetCatalog = catalog;
    panel->Build(snapshot);
    context->Update();
    document->QuerySelector("#asset-list button")->DispatchEvent("dblclick", {});
    auto json = std::make_shared<EditorJsonDocumentSnapshot>();
    json->revision = 1;
    json->assetPath = "Maps/SC_test.json";
    json->fields.push_back({-1, 0, "", "$", "{1}", EditorJsonValueKind::Object});
    json->fields.push_back({0, 1, "/objects", "objects", "[300]", EditorJsonValueKind::Array});
    for (int index = 0; index < 300; ++index)
        json->fields.push_back({1, 2, "/objects/" + std::to_string(index), std::to_string(index),
            "<injected/>", EditorJsonValueKind::String});
    snapshot.jsonDocument = json;
    panel->Build(snapshot);
    Click("[data-action=json-expand][data-index=1]");
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(document->QuerySelector("injected"), nullptr);
    EXPECT_EQ(document->GetElementById("json-fields")->GetNumChildren(), 64);
    Click("#json-next");
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(document->GetElementById("json-fields")->GetNumChildren(), 64);
    EXPECT_NE(document->GetElementById("json-page")->GetInnerRML().find("2 /"), std::string::npos);
}

TEST_F(EditorUiTest, TenThousandAssetsKeepWholeEditorGeometryAndWarmWorkBounded)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->revision = 1;
    for (size_t index = 0; index < 10000; ++index)
        catalog->assets.push_back({"Meshes/SM_grid_" + std::to_string(index) + ".json", EditorAssetKind::Mesh});
    snapshot.assetCatalog = catalog;
    for (int frame = 0; frame < 4; ++frame)
    {
        panel->Build(snapshot);
        context->Update();
        context->Render();
    }
    auto* list = document->GetElementById("asset-list");
    EXPECT_LE(list->GetNumChildren(), 202);
    const auto propertyWrites = panel->WorkspacePropertyWriteCount();
    renderer.draws = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 100; ++frame)
    {
        panel->Build(snapshot);
        context->Update();
        context->Render();
    }
    const double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "Editor warm-frame CPU (fake GPU), 10000 assets: "
        << elapsed / 100.0 << " ms/frame, " << renderer.draws / 100 << " draws/frame\n";
    EXPECT_EQ(panel->WorkspacePropertyWriteCount(), propertyWrites);
    EXPECT_LT(renderer.draws / 100, 1000u);
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(EditorUiTest, OverflowingBrowserGridKeepsColumnsForEveryTileSize)
{
    for (int index = 0; index < 60; ++index)
        snapshot.assets.push_back({"Maps/SC_test/Materials/MI_grid_" + std::to_string(index) + ".json", false});
    for (const Rml::Vector2i dimensions : {Rml::Vector2i(960, 540), Rml::Vector2i(1280, 720)})
    {
        context->SetDimensions(dimensions);
        for (const char* size : {"small", "medium", "large"})
        {
            SetValue("browser-size", size);
            panel->Build(snapshot);
            context->Update();
            SCOPED_TRACE(testing::Message() << "viewport=" << dimensions.x << " size=" << size);
            auto* list = document->GetElementById("asset-list");
            Rml::ElementList tiles;
            list->QuerySelectorAll(tiles, ".asset-tile");
            ASSERT_GT(tiles.size(), 2u);
            EXPECT_LE(tiles.size(), 61u);
            EXPECT_NE(document->GetElementById("asset-count")->GetInnerRML().find("61 assets"), std::string::npos);
            const auto first = tiles[0]->GetAbsoluteOffset(Rml::BoxArea::Border);
            const auto second = tiles[1]->GetAbsoluteOffset(Rml::BoxArea::Border);
            EXPECT_GT(second.x, first.x + 90.0f);
            EXPECT_NEAR(second.y, first.y, 1.0f);
            EXPECT_GT(tiles.back()->GetAbsoluteOffset(Rml::BoxArea::Border).y, first.y);
            const auto listOrigin = list->GetAbsoluteOffset(Rml::BoxArea::Content);
            const float right = listOrigin.x + list->GetBox().GetSize(Rml::BoxArea::Content).x;
            for (auto* tile : tiles)
            {
                EXPECT_LE(tile->GetAbsoluteOffset(Rml::BoxArea::Border).x +
                    tile->GetBox().GetSize(Rml::BoxArea::Border).x, right + 1.0f);
            }
        }
    }
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(EditorUiTest, ExpandedModelTreeRoutesChildSelectionThroughTheExistingSink)
{
    MaterialInstanceModelSnapshot model;
    model.worldGeneration = 1;
    model.objectId = 11;
    model.objectIdentity = "object-one";
    model.scenePath = "Maps/SC_test/SC_test.json";
    model.displayName = "Model";
    model.materials.push_back({11, 3, "Surface", snapshot.activeDocument->assetPath});
    snapshot.sceneModels.push_back(model);
    panel->Build(snapshot);
    Click("[data-action=model-expand]");
    panel->Build(snapshot);
    ASSERT_NE(document->QuerySelector("[data-action=model-slot]"), nullptr);
    Click("[data-action=model-slot]");
    ASSERT_TRUE(selectionSink.selectedMaterial);
    EXPECT_EQ(selectionSink.selectedMaterial->worldGeneration, 1u);
    EXPECT_EQ(selectionSink.selectedMaterial->objectId, 11u);
    EXPECT_EQ(selectionSink.selectedMaterial->materialSlotIndex, 3u);
    EXPECT_EQ(selectionSink.selectedMaterial->materialInstancePath, snapshot.activeDocument->assetPath);
}

TEST_F(EditorUiTest, WorldReplacementClearsTreeExpansionEvenWhenObjectNamesMatch)
{
    MaterialInstanceModelSnapshot model;
    model.worldGeneration = 1;
    model.objectIdentity = "same-name";
    model.materials.push_back({11, 0, "Surface", snapshot.activeDocument->assetPath});
    snapshot.sceneModels.push_back(model);
    panel->Build(snapshot);
    Click("[data-action=model-expand]");
    panel->Build(snapshot);
    ASSERT_NE(document->QuerySelector("[data-action=model-slot]"), nullptr);
    snapshot.sceneModels[0].worldGeneration = 2;
    panel->Build(snapshot);
    EXPECT_EQ(document->QuerySelector("[data-action=model-slot]"), nullptr);
}

TEST_F(EditorUiTest, SnapshotRefreshDoesNotSubmitEdits)
{
    snapshot.activeDocument->parameters[0].effectiveValue.values[0] = 0.8f;
    panel->Build(snapshot);
    panel->Build(snapshot);
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, ComponentEditsCoalesceIntoOneCompleteVector)
{
    SetValue("parameter-0-0", "0.7");
    SetValue("parameter-0-1", "0.8");
    SetValue("parameter-0-0", "0.9");
    panel->Build(snapshot);
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& command = sink.commands.front();
    EXPECT_EQ(command.type, EditorCommandType::ExecuteEditorCommandBatch);
    EXPECT_EQ(command.source, EditorCommandSource::RmlUi);
    EXPECT_EQ(command.expectedDocumentRevision, 7u);
    const auto& batch = std::get<ExecuteEditorCommandBatchPayload>(command.payload);
    ASSERT_EQ(batch.commands.size(), 1u);
    const auto& parameter = std::get<SetMaterialParameterOverridePayload>(batch.commands[0].payload);
    EXPECT_EQ(parameter.parameterType, EditorMaterialParameterType::Vec3);
    EXPECT_EQ(std::get<EditorVec3>(parameter.value), (EditorVec3{0.9f, 0.8f, 0.3f}));
}

TEST_F(EditorUiTest, InvalidNumberDoesNotQueueAnAssetEdit)
{
    SetValue("parameter-0-0", "nan");
    panel->Build(snapshot);
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, PartialNumberDoesNotQueueAnAssetEdit)
{
    SetValue("parameter-0-0", "0.8garbage");
    panel->Build(snapshot);
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, SliderRangePrefersChannelThenParameterThenTheDefaultValue)
{
    // 默认 fixture 的三个分量都没有声明范围：兜底按 default value(0.1/0.2/0.3) 得到 ±1。
    ExpectSliderRange("parameter-0-0", -1.0f, 1.0f);
    ExpectSliderRange("parameter-0-2", -1.0f, 1.0f);

    // 参数级范围在没有通道范围时生效。
    // details markup 按 identity（assetPath + schemaDigest）重建：范围来自 M_ schema，
    // schema 变化必然改变摘要，所以范围变化一定会重建分组。
    snapshot.activeDocument->parameters[0].hasRange = true;
    snapshot.activeDocument->parameters[0].minValue = -2.0f;
    snapshot.activeDocument->parameters[0].maxValue = 3.0f;
    snapshot.activeDocument->schemaDigest = "schema-parameter-range";
    panel->Build(snapshot);
    context->Update();
    ExpectSliderRange("parameter-0-0", -2.0f, 3.0f);
    ExpectSliderRange("parameter-0-1", -2.0f, 3.0f);
    ExpectSliderRange("parameter-0-2", -2.0f, 3.0f);

    // 通道范围优先于参数范围，而且只影响它自己那一维。
    snapshot.activeDocument->parameters[0].channels[1].hasRange = true;
    snapshot.activeDocument->parameters[0].channels[1].minValue = 0.0f;
    snapshot.activeDocument->parameters[0].channels[1].maxValue = 2.0f;
    snapshot.activeDocument->schemaDigest = "schema-channel-range";
    panel->Build(snapshot);
    context->Update();
    ExpectSliderRange("parameter-0-1", 0.0f, 2.0f);
    ExpectSliderRange("parameter-0-0", -2.0f, 3.0f);
    ExpectSliderRange("parameter-0-2", -2.0f, 3.0f);
    // 滑条与文本输入共用同一业务地址，滑条不引入第二条命令路径。
    auto* slider = Control("parameter-0-1-slider");
    ASSERT_NE(slider, nullptr);
    for (const char* attribute : {"data-action", "data-index", "data-component"})
    {
        EXPECT_EQ(slider->GetAttribute<std::string>(attribute, ""),
            Control("parameter-0-1")->GetAttribute<std::string>(attribute, "")) << attribute;
    }
}

TEST_F(EditorUiTest, LegacyRangeFallbackFollowsTheDefaultValueNotTheEditedValue)
{
    auto& parameter = snapshot.activeDocument->parameters[0];
    parameter.defaultValue.values[0] = 2.0f;
    parameter.effectiveValue.values[0] = 50.0f;
    snapshot.activeDocument->schemaDigest = "schema-legacy-range";
    panel->Build(snapshot);
    context->Update();
    // 旧 M_ 文件没有范围：extent = max(1, |default| * 2) = 4。
    // 基准是默认值而不是正在编辑的值，否则错误输入会把可调区间越推越宽
    // （按 effectiveValue=50 推导会得到 ±100）。
    ExpectSliderRange("parameter-0-0", -4.0f, 4.0f);
    ExpectSliderRange("parameter-0-1", -1.0f, 1.0f);
}

TEST_F(EditorUiTest, NonFiniteOrInvertedRangesStillYieldAUsableSlider)
{
    auto& parameter = snapshot.activeDocument->parameters[0];
    parameter.hasRange = true;
    parameter.minValue = 5.0f;
    parameter.maxValue = -5.0f;  // 反向区间：不能直接交给范围控件
    parameter.channels[0].hasRange = true;
    parameter.channels[0].minValue = std::numeric_limits<float>::quiet_NaN();
    parameter.channels[0].maxValue = 1.0f;
    parameter.channels[1].hasRange = true;
    parameter.channels[1].minValue = 0.0f;
    parameter.channels[1].maxValue = std::numeric_limits<float>::infinity();
    panel->Build(snapshot);
    context->Update();
    // 通道范围非有限、参数范围反向：三层全部落空时仍得到按默认值推导的有限兜底。
    for (const char* id : {"parameter-0-0", "parameter-0-1", "parameter-0-2"})
    {
        SCOPED_TRACE(id);
        ExpectSliderRange(id, -1.0f, 1.0f);
        auto* slider = Control(std::string(id) + "-slider");
        ASSERT_NE(slider, nullptr);
        const float minimum = slider->GetAttribute<float>("min", 0.0f);
        const float maximum = slider->GetAttribute<float>("max", 0.0f);
        EXPECT_TRUE(std::isfinite(minimum));
        EXPECT_TRUE(std::isfinite(maximum));
        EXPECT_LE(minimum, maximum);
        EXPECT_GT(slider->GetAttribute<float>("step", 0.0f), 0.0f);
    }
    // 兜底区间上的拖动仍然提交正常编辑，不触发有限数校验。
    DragSlider("parameter-0-0-slider", "-0.5");
    panel->Build(snapshot);
    EXPECT_EQ(Notice().find("finite"), std::string::npos);
    ASSERT_FALSE(sink.commands.empty());
}

TEST_F(EditorUiTest, SliderDragProducesTheSameBatchItemAsTyping)
{
    // 拖动：滑条自身派发 change，走的是和文本输入相同的 HandleChange。
    DragSlider("parameter-0-0-slider", "0.75");
    panel->Build(snapshot);
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& draggedBatch = std::get<ExecuteEditorCommandBatchPayload>(sink.commands[0].payload);
    ASSERT_EQ(draggedBatch.commands.size(), 1u);
    // payload 按值取出：下面清空 sink 后不能再持有容器内元素的引用。
    const EditorCommandType draggedType = draggedBatch.commands[0].type;
    const SetMaterialParameterOverridePayload dragged =
        std::get<SetMaterialParameterOverridePayload>(draggedBatch.commands[0].payload);
    sink.commands.clear();

    // 键入同一个数：必须是同一种 batch item、同一个字段键。
    SetValue("parameter-0-0", "0.75");
    panel->Build(snapshot);
    ASSERT_EQ(sink.commands.size(), 1u);
    EXPECT_EQ(sink.commands[0].type, EditorCommandType::ExecuteEditorCommandBatch);
    const auto& typedBatch = std::get<ExecuteEditorCommandBatchPayload>(sink.commands[0].payload);
    ASSERT_EQ(typedBatch.commands.size(), 1u);
    EXPECT_EQ(typedBatch.commands[0].type, draggedType);
    const auto& typed = std::get<SetMaterialParameterOverridePayload>(typedBatch.commands[0].payload);
    EXPECT_EQ(dragged.assetPath, typed.assetPath);
    EXPECT_EQ(dragged.parameter, typed.parameter);
    EXPECT_EQ(dragged.parameterType, typed.parameterType);
    // 数值只差滑条自身的量化步长；未触碰的分量保持快照值。
    const auto draggedValues = std::get<EditorVec3>(dragged.value);
    EXPECT_NEAR(draggedValues[0], 0.75f, 0.01f);
    EXPECT_EQ(draggedValues[1], 0.2f);
    EXPECT_EQ(draggedValues[2], 0.3f);
    EXPECT_EQ(Notice().find("finite"), std::string::npos);
}

TEST_F(EditorUiTest, OnlyColorParametersGetAnSrgbSwatchAndHexReadout)
{
    // 默认 fixture 的 u_color 是 Vec3：分量有滑条，但没有色块与十六进制读数。
    EXPECT_EQ(document->GetElementById("color-swatch-0"), nullptr);
    EXPECT_EQ(document->GetElementById("color-hex-0"), nullptr);
    EXPECT_NE(Control("parameter-0-0-slider"), nullptr);

    EditorParameterSnapshot color;
    color.name = "u_tint";
    color.type = EditorParameterType::Color;
    color.effectiveValue = {EditorParameterType::Color, {0.21404114f, 0.0f, 1.0f, 0.5f}};
    color.defaultValue = color.effectiveValue;
    snapshot.activeDocument->parameters.push_back(color);
    // 参数集合来自 M_ schema：新参数意味着 schema 变化，分组在这个 identity 上重建。
    snapshot.activeDocument->schemaDigest = "schema-color";
    panel->Build(snapshot);
    context->Update();
    ASSERT_NE(document->GetElementById("color-swatch-1"), nullptr);
    // 线性 0.214041 → sRGB 0x80；alpha 不做 gamma 变换。
    EXPECT_EQ(document->GetElementById("color-hex-1")->GetInnerRML(), "#8000FF80");
    // HEAD 对 Color 用 ColorPicker4，没有滑条；这里也保持文本输入 + 显示侧读数。
    EXPECT_EQ(document->GetElementById("parameter-1-0-slider"), nullptr);
    EXPECT_NE(Control("parameter-1-0"), nullptr);

    // 有效值变化不重建 markup：读数与色块由 RefreshControls 逐帧跟随，
    // 且不提交任何资产命令（色块是只读展示）。
    snapshot.activeDocument->parameters[1].effectiveValue.values[0] = 1.0f;
    snapshot.activeDocument->parameters[1].effectiveValue.values[2] = 0.0f;
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(document->GetElementById("color-hex-1")->GetInnerRML(), "#FF000080");
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, DetailsSectionsCollapseWithoutRebuildingTheGroupDom)
{
    for (const char* key : {"general", "render-states", "parameters", "textures"})
        EXPECT_TRUE(panel->IsSectionExpanded(key)) << key;

    auto* header = document->GetElementById("details-section-parameters-title");
    auto* body = document->QuerySelector("#details-section-parameters .details-section-body");
    auto* component = document->GetElementById("parameter-0-0");
    ASSERT_NE(header, nullptr);
    ASSERT_NE(body, nullptr);
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(header->GetAttribute<std::string>("data-action", ""), "section");
    EXPECT_EQ(header->GetAttribute<std::string>("data-section", ""), "parameters");
    EXPECT_TRUE(body->IsVisible(true));
    ASSERT_TRUE(header->Focus());
    EXPECT_EQ(context->GetFocusElement(), header);

    header->DispatchEvent("click", {});
    context->Update();
    EXPECT_FALSE(panel->IsSectionExpanded("parameters"));
    EXPECT_TRUE(document->GetElementById("details-section-parameters")->IsClassSet("collapsed"));
    EXPECT_FALSE(body->IsVisible(true));
    // 切换只改既有分组元素的 class：参数控件还是同一批 DOM 节点，焦点也不丢。
    EXPECT_EQ(document->QuerySelector("#details-section-parameters .details-section-body"), body);
    EXPECT_EQ(document->GetElementById("parameter-0-0"), component);
    EXPECT_EQ(context->GetFocusElement(), header);

    header->DispatchEvent("click", {});
    context->Update();
    EXPECT_TRUE(panel->IsSectionExpanded("parameters"));
    EXPECT_FALSE(document->GetElementById("details-section-parameters")->IsClassSet("collapsed"));
    EXPECT_TRUE(body->IsVisible(true));
    // 折叠是纯粹的会话 UI 状态：不提交资产命令，也不请求运行时动作。
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());
}

TEST_F(EditorUiTest, SectionStateSurvivesBuildRefreshAndDocumentSwitch)
{
    auto* header = document->GetElementById("details-section-general-title");
    ASSERT_NE(header, nullptr);
    header->DispatchEvent("click", {});
    panel->Build(snapshot);
    context->Update();
    EXPECT_TRUE(document->GetElementById("details-section-general")->IsClassSet("collapsed"));

    // 切换文档会重建 details DOM（identity 变化），展开状态仍属于面板。
    snapshot.activeDocument->assetPath = "Maps/SC_test/Materials/MI_other.json";
    snapshot.activeDocument->schemaDigest = "schema-two";
    panel->Build(snapshot);
    context->Update();
    auto* section = document->GetElementById("details-section-general");
    ASSERT_NE(section, nullptr);
    EXPECT_TRUE(section->IsClassSet("collapsed"));
    EXPECT_FALSE(document->QuerySelector("#details-section-general .details-section-body")->IsVisible(true));
    EXPECT_FALSE(panel->IsSectionExpanded("general"));
    // 没有折叠过的分组仍保持默认展开（HEAD 四个分组都是 DefaultOpen）。
    EXPECT_FALSE(document->GetElementById("details-section-render-states")->IsClassSet("collapsed"));
    EXPECT_FALSE(document->GetElementById("details-section-parameters")->IsClassSet("collapsed"));
    EXPECT_FALSE(document->GetElementById("details-section-textures")->IsClassSet("collapsed"));
}

TEST_F(EditorUiTest, StaticFramesDoNotRewriteSectionClasses)
{
    for (int frame = 0; frame < 3; ++frame)
    {
        panel->Build(snapshot);
        context->Update();
    }
    const size_t writes = panel->PanelPropertyWriteCount();
    for (int frame = 0; frame < 100; ++frame)
    {
        panel->Build(snapshot);
        context->Update();
    }
    // 展开状态、色块底色与控件值都走 widgetDocument 的值缓存，静止帧不产生任何属性写入。
    EXPECT_EQ(panel->PanelPropertyWriteCount(), writes);
    EXPECT_FALSE(document->GetElementById("details-section-parameters")->IsClassSet("collapsed"));
}

TEST_F(EditorUiTest, SaveIncludesPendingEditsInTheSameBatch)
{
    SetValue("parameter-0-0", "0.7");
    panel->RequestSave();
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& batch = std::get<ExecuteEditorCommandBatchPayload>(sink.commands[0].payload);
    ASSERT_EQ(batch.commands.size(), 2u);
    EXPECT_EQ(batch.commands[0].type, EditorCommandType::SetMaterialParameterOverride);
    EXPECT_EQ(batch.commands[1].type, EditorCommandType::SaveMaterialInstanceDocument);
}

TEST_F(EditorUiTest, QueuedEditKeepsTheRevisionItWasAuthoredAgainst)
{
    SetValue("parameter-0-0", "0.7");
    snapshot.activeDocument->revision = 8;
    panel->Build(snapshot);
    ASSERT_EQ(sink.commands.size(), 1u);
    EXPECT_EQ(sink.commands[0].expectedDocumentRevision, 7u);
}

TEST_F(EditorUiTest, SnapshotRefreshPreservesFocusedInput)
{
    auto* control = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("parameter-0-0"));
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(control->Focus());
    control->SetValue("-");
    snapshot.activeDocument->parameters[0].effectiveValue.values[0] = 0.8f;
    panel->Build(snapshot);
    EXPECT_EQ(control->GetValue(), "-");
    EXPECT_EQ(context->GetFocusElement(), control);
}

TEST_F(EditorUiTest, DirtyCloseRequiresAnExplicitDecision)
{
    Click("[data-action=close]");
    EXPECT_TRUE(panel->HasModal());
    EXPECT_TRUE(sink.commands.empty());
    Click("[data-action=close-cancel]");
    EXPECT_FALSE(panel->HasModal());
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, DiscardUsesTheExistingDirtyDocumentPolicy)
{
    Click("[data-action=close]");
    Click("[data-action=close-discard]");
    ASSERT_EQ(sink.commands.size(), 1u);
    EXPECT_EQ(sink.commands[0].type, EditorCommandType::CloseMaterialInstanceAsset);
    const auto& close = std::get<CloseMaterialInstanceAssetPayload>(sink.commands[0].payload);
    EXPECT_EQ(close.dirtyPolicy, EditorDirtyDocumentPolicy::DiscardChanges);
}

TEST_F(EditorUiTest, SaveAndCloseWaitsForAnAuthoritativeCleanSnapshot)
{
    Click("[data-action=close]");
    Click("[data-action=close-save]");
    panel->Build(snapshot);
    ASSERT_EQ(sink.commands.size(), 1u);
    EXPECT_EQ(sink.commands[0].type, EditorCommandType::SaveMaterialInstanceDocument);
    EXPECT_EQ(sink.commands[0].expectedDocumentRevision, 7u);
    panel->Build(snapshot);
    EXPECT_EQ(sink.commands.size(), 1u);
    snapshot.documentTabs[0].dirty = false;
    snapshot.activeDocument->status = EditorDocumentStatus::Clean;
    snapshot.activeDocument->revision = 8;
    panel->Build(snapshot);
    ASSERT_EQ(sink.commands.size(), 2u);
    EXPECT_EQ(sink.commands[1].type, EditorCommandType::CloseMaterialInstanceAsset);
}

TEST_F(EditorUiTest, ValidateIncludesPendingEditsInTheSameBatch)
{
    SetValue("parameter-0-0", "0.7");
    Click("[data-action=validate]");
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& batch = std::get<ExecuteEditorCommandBatchPayload>(sink.commands[0].payload);
    ASSERT_EQ(batch.commands.size(), 2u);
    EXPECT_EQ(batch.commands[1].type, EditorCommandType::ValidateMaterialInstanceDocument);
}

TEST_F(EditorUiTest, FailedSaveKeepsTheDocumentOpenAndAllowsRetry)
{
    Click("[data-action=close]");
    Click("[data-action=close-save]");
    panel->Build(snapshot);
    snapshot.activeDocument->status = EditorDocumentStatus::SaveFailed;
    snapshot.statusMessage = "Source digest conflict";
    panel->Build(snapshot);
    EXPECT_TRUE(panel->HasModal());
    ASSERT_EQ(sink.commands.size(), 1u);
    EXPECT_NE(document->GetElementById("close-document-status")->GetInnerRML().find("conflict"), std::string::npos);
    Click("[data-action=close-save]");
    panel->Build(snapshot);
    ASSERT_EQ(sink.commands.size(), 2u);
    EXPECT_EQ(sink.commands[1].type, EditorCommandType::SaveMaterialInstanceDocument);
}

TEST_F(EditorUiTest, ResetThenEditUsesTheBaseValueForUntouchedComponents)
{
    snapshot.activeDocument->parameters[0].overrideValue =
        EditorParameterValue{EditorParameterType::Vec3, {0.9f, 0.8f, 0.7f, 0.0f}};
    snapshot.activeDocument->parameters[0].effectiveValue =
        *snapshot.activeDocument->parameters[0].overrideValue;
    panel->Build(snapshot);
    Click("[data-action=reset-parameter]");
    SetValue("parameter-0-0", "0.4");
    panel->Build(snapshot);
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& batch = std::get<ExecuteEditorCommandBatchPayload>(sink.commands[0].payload);
    ASSERT_EQ(batch.commands.size(), 1u);
    const auto& parameter = std::get<SetMaterialParameterOverridePayload>(batch.commands[0].payload);
    EXPECT_EQ(std::get<EditorVec3>(parameter.value), (EditorVec3{0.4f, 0.2f, 0.3f}));
}

TEST_F(EditorUiTest, TextureOptionsTrackAssetRefreshWithoutSubmittingAnEdit)
{
    snapshot.activeDocument->schemaDigest = "schema-with-texture";
    snapshot.activeDocument->textures.push_back({"albedo", "", "T_base.json", "T_base.json", std::nullopt});
    panel->Build(snapshot);
    snapshot.textureAssets.push_back({"T_new.json", true});
    snapshot.activeDocument->textures[0].effectiveAssetPath = "T_external.json";
    panel->Build(snapshot);
    auto* selector = dynamic_cast<Rml::ElementFormControlSelect*>(document->GetElementById("texture-0"));
    ASSERT_NE(selector, nullptr);
    EXPECT_EQ(selector->GetValue(), "T_external.json");
    ASSERT_EQ(selector->GetNumOptions(), 2);
    EXPECT_EQ(selector->GetOption(1)->GetAttribute<std::string>("value", ""), "T_new.json");
    panel->Build(snapshot);
    EXPECT_EQ(selector->GetNumOptions(), 2);
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, InvalidOfflineSettingDoesNotReachRuntime)
{
    SetValue("offline-samples", "-1");
    EXPECT_TRUE(runtimeSink.actions.empty());
}

TEST_F(EditorUiTest, OfflineSettingQueuesTypedRuntimeAction)
{
    SetValue("offline-samples", "64");
    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions[0].type, UiActionType::SetOfflineSetting);
    EXPECT_EQ(runtimeSink.actions[0].stringValue, "samples");
    EXPECT_EQ(runtimeSink.actions[0].intValue, 64);
}

TEST_F(EditorUiTest, AssetLabelsAreEscapedAsText)
{
    snapshot.assets[0].assetPath = "Maps/SC_test/Materials/MI_<injected>.json";
    panel->Build(snapshot);
    EXPECT_EQ(document->QuerySelector("injected"), nullptr);
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(EditorUiTest, InvalidDocumentCandidateDoesNotDetachTheActiveWorkspace)
{
    auto* candidate = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
    ASSERT_NE(candidate, nullptr);
    candidate->GetElementById("scene-models")->SetId("missing");
    std::string error;
    EXPECT_FALSE(panel->Attach(*candidate, error));
    EXPECT_FALSE(error.empty());
    context->UnloadDocument(candidate);
    SetValue("parameter-0-0", "0.7");
    panel->Build(snapshot);
    EXPECT_EQ(sink.commands.size(), 1u);
}

TEST_F(EditorUiTest, MenuBarOpensOneMenuAtATimeAndClosesOnItemSelection)
{
    Click("#menu-file-title");
    context->Update();
    EXPECT_TRUE(panel->IsMenuOpen("file"));
    EXPECT_TRUE(document->GetElementById("menu-file")->IsClassSet("open"));
    ASSERT_TRUE(document->GetElementById("menu-file-popup")->IsVisible());

    // 打开第二个菜单必须收起第一个，不能同时展开两个弹层。
    Click("#menu-edit-title");
    context->Update();
    EXPECT_TRUE(panel->IsMenuOpen("edit"));
    EXPECT_FALSE(panel->IsMenuOpen("file"));
    EXPECT_FALSE(document->GetElementById("menu-file-popup")->IsVisible());

    // 选择条目复用工具栏命令路径，并在提交后收起菜单。
    Click("#menu-edit-validate");
    context->Update();
    EXPECT_FALSE(panel->IsMenuOpen("edit"));
    ASSERT_FALSE(sink.commands.empty());
    EXPECT_EQ(sink.commands.back().type, EditorCommandType::ValidateMaterialInstanceDocument);
}

TEST_F(EditorUiTest, MenuBarClosesWhenClickingOutsideIt)
{
    Click("#menu-window-title");
    context->Update();
    ASSERT_TRUE(panel->IsMenuOpen("window"));
    Click("#save-asset");
    context->Update();
    EXPECT_FALSE(panel->IsMenuOpen("window"));
    EXPECT_FALSE(document->GetElementById("menu-window-popup")->IsVisible());
}

TEST_F(EditorUiTest, EditUndoRedoArePresentButDisabled)
{
    Click("#menu-edit-title");
    context->Update();
    auto* undo = document->GetElementById("menu-edit-undo");
    auto* redo = document->GetElementById("menu-edit-redo");
    ASSERT_NE(undo, nullptr);
    ASSERT_NE(redo, nullptr);
    EXPECT_TRUE(undo->HasAttribute("disabled"));
    EXPECT_TRUE(redo->HasAttribute("disabled"));
    // 这个仓库没有 undo/redo 系统；两个条目只能是占位，不能产生任何命令。
    undo->DispatchEvent("click", {});
    redo->DispatchEvent("click", {});
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());
}

TEST_F(EditorUiTest, FileMenuDocumentCommandsShareTheToolbarAvailability)
{
    // 有活动 MI 文档时 File/Edit 的文档命令与工具栏按钮同样可用。
    EXPECT_FALSE(document->GetElementById("menu-file-save")->HasAttribute("disabled"));
    EXPECT_FALSE(document->GetElementById("menu-edit-reset")->HasAttribute("disabled"));

    Click("#menu-file-save");
    context->Update();
    ASSERT_FALSE(sink.commands.empty());
    EXPECT_EQ(sink.commands.back().type, EditorCommandType::SaveMaterialInstanceDocument);

    // 切到只读 JSON 视图后，两边必须同时禁用。
    panel->ShowMaterialDetails();
    auto json = std::make_shared<EditorJsonDocumentSnapshot>();
    json->assetPath = "Maps/SC_test.json";
    json->kind = EditorAssetKind::Scene;
    snapshot.jsonDocument = json;
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->assets.push_back({"Maps/SC_test.json", EditorAssetKind::Scene, "Level"});
    snapshot.assetCatalog = catalog;
    Refresh();
    auto* list = document->GetElementById("asset-list");
    ASSERT_NE(list->GetChild(0), nullptr);
    list->GetChild(0)->DispatchEvent("dblclick", {});
    Refresh();
    EXPECT_TRUE(document->GetElementById("save-asset")->HasAttribute("disabled"));
    EXPECT_TRUE(document->GetElementById("menu-file-save")->HasAttribute("disabled"));
    EXPECT_TRUE(document->GetElementById("menu-edit-validate")->HasAttribute("disabled"));
}

TEST_F(EditorUiTest, FileOpenLevelListsSceneCatalogAndRequestsWorldLoad)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->revision = 4;
    catalog->assets = {
        {"Meshes/SM_test.json", EditorAssetKind::Mesh, "Mesh"},
        {"Maps/SC_alpha.json", EditorAssetKind::Scene, "Alpha Level"},
        {"Maps/SC_beta.json", EditorAssetKind::Scene, "Beta Level"},
        {"Maps/SC_beta.json", EditorAssetKind::Texture, "Not A Level"}};
    snapshot.assetCatalog = catalog;
    panel->Build(snapshot);
    context->Update();
    ASSERT_EQ(panel->MenuLevelPaths().size(), 2u);
    EXPECT_EQ(panel->MenuLevelPaths()[0], "Maps/SC_alpha.json");
    EXPECT_EQ(panel->MenuLevelPaths()[1], "Maps/SC_beta.json");

    Click("#menu-file-title");
    context->Update();
    // 子菜单由真实指针悬停展开：CPU 测试同样走 RmlUi 的 hover 判定。
    auto* entry = document->GetElementById("menu-file-open-level");
    ASSERT_NE(entry, nullptr);
    const auto point = entry->GetAbsoluteOffset(Rml::BoxArea::Border) + Rml::Vector2f(20, 6);
    context->ProcessMouseMove(static_cast<int>(point.x), static_cast<int>(point.y), 0);
    context->Update();
    ASSERT_TRUE(document->GetElementById("menu-file-levels")->IsVisible());
    auto* item = document->GetElementById("menu-level-1");
    ASSERT_NE(item, nullptr);
    EXPECT_NE(item->GetInnerRML().find("Beta Level"), std::string::npos);
    item->DispatchEvent("click", {});
    context->Update();
    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.back().type, UiActionType::LoadWorld);
    EXPECT_EQ(runtimeSink.actions.back().stringValue, "Maps/SC_beta.json");
    EXPECT_FALSE(panel->IsMenuOpen("file"));
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, WindowMenuCheckboxMatchesPanelVisibility)
{
    Click("#menu-window-title");
    context->Update();
    auto* item = document->GetElementById("menu-window-outliner");
    ASSERT_NE(item, nullptr);
    auto* check = item->QuerySelector(".menu-check");
    ASSERT_NE(check, nullptr);
    EXPECT_FALSE(check->GetInnerRML().empty());
    EXPECT_TRUE(panel->IsPanelVisible("outliner"));

    item->DispatchEvent("click", {});
    Refresh();
    EXPECT_FALSE(panel->IsPanelVisible("outliner"));
    EXPECT_FALSE(panel->IsMenuOpen("window"));
    EXPECT_FALSE(document->GetElementById("workspace-outliner-tab")->IsVisible());

    Click("#menu-window-title");
    Refresh();
    check = document->GetElementById("menu-window-outliner")->QuerySelector(".menu-check");
    ASSERT_NE(check, nullptr);
    EXPECT_TRUE(check->GetInnerRML().empty());
    document->GetElementById("menu-window-outliner")->DispatchEvent("click", {});
    Refresh();
    EXPECT_TRUE(panel->IsPanelVisible("outliner"));
    EXPECT_TRUE(document->GetElementById("workspace-outliner-tab")->IsVisible());
    EXPECT_TRUE(system.errors.empty()) << Errors();
}

TEST_F(EditorUiTest, AboutPanelShowsUiAndLayoutPathAndBlocksWorkspaceCommands)
{
    EXPECT_FALSE(document->GetElementById("about-dialog")->IsVisible());
    Click("#menu-help-about");
    Refresh();
    EXPECT_TRUE(panel->IsAboutVisible());
    EXPECT_TRUE(document->GetElementById("about-dialog")->IsVisible());
    EXPECT_NE(document->GetElementById("about-dialog")->GetInnerRML().find("RmlUi"), std::string::npos);
    EXPECT_NE(document->GetInnerRML().find("workspace-v1.json"), std::string::npos);
    EXPECT_TRUE(panel->HasModal());

    // About 打开时工作区命令不生效，只有它自己的关闭按钮可用。
    const size_t commands = sink.commands.size();
    Click("#menu-file-save");
    EXPECT_EQ(sink.commands.size(), commands);
    Click("#about-close");
    Refresh();
    EXPECT_FALSE(panel->IsAboutVisible());
    EXPECT_FALSE(panel->HasModal());
}

TEST_F(EditorUiTest, ViewportToolbarKeepsOneActiveToolSpaceAndViewMode)
{
    const std::vector<std::string> tools = {
        "viewport-tool-select", "viewport-tool-move", "viewport-tool-rotate", "viewport-tool-scale"};
    for (const auto& clicked : tools)
    {
        Click("#" + clicked);
        context->Update();
        for (const auto& id : tools)
            EXPECT_EQ(document->GetElementById(id)->IsClassSet("active"), id == clicked) << id;
    }
    EXPECT_EQ(panel->ViewportTransformTool(), "scale");

    for (const auto& clicked : {std::string("viewport-space-world"), std::string("viewport-space-local")})
    {
        Click("#" + clicked);
        context->Update();
        EXPECT_EQ(document->GetElementById("viewport-space-world")->IsClassSet("active"),
            clicked == "viewport-space-world");
        EXPECT_EQ(document->GetElementById("viewport-space-local")->IsClassSet("active"),
            clicked == "viewport-space-local");
    }
    EXPECT_EQ(panel->ViewportTransformSpace(), "local");

    // 只有 view mode 有真实后端：Lit=0，Unlit=1，未实现的 Wireframe 保持禁用。
    Click("#viewport-view-lit");
    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.back().type, UiActionType::SetDebugViewMode);
    EXPECT_EQ(runtimeSink.actions.back().intValue, 0);
    EXPECT_TRUE(document->GetElementById("viewport-view-lit")->IsClassSet("active"));
    EXPECT_FALSE(document->GetElementById("viewport-view-unlit")->IsClassSet("active"));

    Click("#viewport-view-unlit");
    ASSERT_EQ(runtimeSink.actions.size(), 2u);
    EXPECT_EQ(runtimeSink.actions.back().intValue, 1);
    // 高亮跟随运行时快照而不是工具栏自己的第二份状态：命令回声之前 Lit 仍是权威状态。
    EXPECT_TRUE(document->GetElementById("viewport-view-lit")->IsClassSet("active"));
    UiViewModelSnapshot view;
    view.debugViewMode = 1;
    panel->UpdateRuntimeView(view);
    context->Update();
    EXPECT_TRUE(document->GetElementById("viewport-view-unlit")->IsClassSet("active"));
    EXPECT_FALSE(document->GetElementById("viewport-view-lit")->IsClassSet("active"));

    auto* wireframe = document->GetElementById("viewport-view-wireframe");
    ASSERT_NE(wireframe, nullptr);
    EXPECT_TRUE(wireframe->HasAttribute("disabled"));
    EXPECT_FALSE(wireframe->GetAttribute<Rml::String>("title", "").empty());
    wireframe->DispatchEvent("click", {});
    EXPECT_EQ(runtimeSink.actions.size(), 2u);
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, TransformToolsChangeNoCommandAndStayCachedOnStaticFrames)
{
    Click("#viewport-tool-rotate");
    context->Update();
    // 没有 gizmo 也没有 transform 编辑后端：工具与坐标系只驱动高亮，不提交任何命令。
    EXPECT_EQ(panel->ViewportTransformTool(), "rotate");
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());

    const size_t writes = panel->PanelPropertyWriteCount();
    for (int frame = 0; frame < 100; ++frame)
    {
        panel->Build(snapshot);
        context->Update();
    }
    EXPECT_EQ(panel->PanelPropertyWriteCount(), writes);
    EXPECT_TRUE(document->GetElementById("viewport-tool-rotate")->IsClassSet("active"));
    EXPECT_TRUE(document->GetElementById("viewport-view-lit")->IsClassSet("active"));
}

TEST_F(EditorUiTest, ViewportToolbarOverlayIsNotASceneHit)
{
    auto* toolbar = document->GetElementById("viewport-toolbar");
    ASSERT_NE(toolbar, nullptr);
    EXPECT_FALSE(panel->IsSceneViewportElement(toolbar));
    EXPECT_FALSE(panel->IsSceneViewportElement(document->GetElementById("viewport-tool-move")));
    EXPECT_TRUE(panel->IsSceneViewportElement(document->GetElementById("editor-viewport-image")));
    const auto origin = toolbar->GetAbsoluteOffset(Rml::BoxArea::Border);
    auto* image = document->GetElementById("editor-viewport-image");
    const auto imageOrigin = image->GetAbsoluteOffset(Rml::BoxArea::Border);
    // 覆盖层落在图像左上角内部，但命中属于编辑器。
    EXPECT_GE(origin.x, imageOrigin.x - 1.0f);
    EXPECT_GE(origin.y, imageOrigin.y - 1.0f);
    EXPECT_LE(origin.y, imageOrigin.y + 40.0f);
    const auto* overlayHit = context->GetElementAtPoint({origin.x + 4.0f, origin.y + 4.0f});
    ASSERT_NE(overlayHit, nullptr);
    EXPECT_NE(overlayHit, image);
    EXPECT_FALSE(panel->IsSceneViewportElement(overlayHit));
    // 图像内部但避开工具条的位置仍然是场景命中。
    EXPECT_TRUE(panel->IsSceneViewportElement(
        context->GetElementAtPoint({imageOrigin.x + 40.0f, imageOrigin.y + 140.0f})));
}

} // namespace

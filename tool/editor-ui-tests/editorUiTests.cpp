#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
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
#include "editor/ui/editorSceneActorTree.h"
#include "editor/scene/editorSceneActorBuilder.h"
#include "sceneNode.h"
#include "world/meshSceneObjectIdentity.h"
#include "world/world.h"
#include "world/worldSnapshotBuilder.h"

namespace
{
using namespace VL;
using namespace VL::EditorUi;

// 单 section、带有效材质实例的 mesh actor；字段组合与 World→快照的映射保持一致。
EditorSceneActorSnapshot MeshActor(const std::string& name, uint64_t generation,
    RuntimeId objectId = 11)
{
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::Mesh;
    actor.worldGeneration = generation;
    actor.scenePath = "Maps/SC_test/SC_test.json";
    actor.identity = SceneActorIdentity(EditorSceneActorType::Mesh, name);
    actor.displayName = name;
    actor.sceneObjectIdentity = name;
    actor.meshObjectId = objectId;
    EditorSceneMeshSectionSnapshot section;
    section.objectName = name;
    section.objectId = objectId;
    section.meshKey = "SM_test|0|Body";
    section.materialKey = "M_test";
    section.materialInstancePath = "Maps/SC_test/Materials/MI_test.json";
    section.materialSlotIndex = 0;
    section.materialSlotName = "Surface";
    actor.sections.push_back(std::move(section));
    return actor;
}

EditorSceneActorSnapshot SunActor(float intensity, bool primary)
{
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::DirectionalLight;
    actor.worldGeneration = 1;
    actor.scenePath = "Maps/SC_test/SC_test.json";
    actor.displayName = "Sun_Light";
    actor.identity = SceneActorIdentity(EditorSceneActorType::DirectionalLight, actor.displayName);
    actor.light.type = LightSnapshotType::Directional;
    actor.light.color = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
    actor.light.intensity = intensity;
    actor.primaryDirectionalLight = primary;
    return actor;
}

EditorSceneActorSnapshot PointLightActor(const std::string& name, float intensity, float radius)
{
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::PointLight;
    actor.worldGeneration = 1;
    actor.scenePath = "Maps/SC_test/SC_test.json";
    actor.displayName = name;
    actor.identity = SceneActorIdentity(EditorSceneActorType::PointLight, name);
    actor.light.type = LightSnapshotType::Point;
    actor.light.color = Eigen::Vector3f(1.0f, 0.5f, 0.25f);
    actor.light.intensity = intensity;
    actor.light.position = Eigen::Vector3f(-3.5f, 3.5f, 6.0f);
    actor.light.radius = radius;
    return actor;
}

EditorSceneActorSnapshot CameraActor(const std::string& name)
{
    EditorSceneActorSnapshot actor;
    actor.type = EditorSceneActorType::Camera;
    actor.worldGeneration = 1;
    actor.scenePath = "Maps/SC_test/SC_test.json";
    actor.displayName = name;
    actor.identity = SceneActorIdentity(EditorSceneActorType::Camera, name);
    actor.camera.position = Eigen::Vector3f(0.0f, 0.0f, 16.0f);
    actor.camera.forward = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    actor.camera.horizontalFovDegrees = 80.0f;
    actor.camera.clipNear = 0.1f;
    actor.camera.clipFar = 100.0f;
    return actor;
}

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
    bool RequestActorSelection(const std::string& actorIdentity) override
    {
        selectedActorIdentity = actorIdentity;
        return true;
    }
    std::optional<Editor::Selection::MaterialInstanceModelContext> selectedModel;
    std::optional<Editor::Selection::MaterialInstanceSelection> selectedMaterial;
    std::string selectedActorIdentity;
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
    // select 的 SetValue 已经通过 WidgetDropDown::OnValueChange 派发 change；
    // 再手动派发一次会重复提交同一条命令，因此这里只写控件值。
    void SelectOption(const std::string& id, const std::string& value)
    {
        auto* control = Control(id);
        ASSERT_NE(control, nullptr);
        control->SetValue(value);
    }
    void Click(const std::string& selector)
    {
        auto* element = document->QuerySelector(selector);
        ASSERT_NE(element, nullptr);
        element->DispatchEvent("click", {});
    }
    // 大纲行的文本：actor 行把名称放在图标后的 span 里，分组行直接是文本。
    static std::string RowLabel(Rml::Element* element)
    {
        auto* span = element->QuerySelector("span");
        return span == nullptr ? element->GetInnerRML() : span->GetInnerRML();
    }
    std::vector<std::string> OutlinerLabels(const char* action) const
    {
        Rml::ElementList elements;
        document->GetElementById("scene-models")
            ->QuerySelectorAll(elements, "[data-action=" + std::string(action) + "]");
        std::vector<std::string> labels;
        for (auto* element : elements)
        {
            // 分组行的折叠按钮与标题行共用同一个动作；标签只取标题行。
            if (element->IsClassSet("list-item")) labels.push_back(RowLabel(element));
        }
        return labels;
    }
    std::string OutlinerCount() const
    {
        auto* count = document->GetElementById("outliner-count");
        return count == nullptr ? std::string() : count->GetInnerRML();
    }
    // 大纲已挂载的行（分组/actor/槽行三种形态共用 .tree-row）。
    std::vector<Rml::Element*> MountedRows() const
    {
        std::vector<Rml::Element*> rows;
        auto* tree = document->GetElementById("scene-models");
        for (int index = 0; index < tree->GetNumChildren(); ++index)
        {
            Rml::Element* child = tree->GetChild(index);
            if (child->IsClassSet("tree-row")) rows.push_back(child);
        }
        return rows;
    }
    std::vector<Rml::Element*> OutlinerSpacers() const
    {
        std::vector<Rml::Element*> spacers;
        auto* tree = document->GetElementById("scene-models");
        for (int index = 0; index < tree->GetNumChildren(); ++index)
        {
            Rml::Element* child = tree->GetChild(index);
            if (child->IsClassSet("outliner-spacer")) spacers.push_back(child);
        }
        return spacers;
    }
    // 按标签找行元素（事件寻址用的 data-index 不适合写死在测试里）。
    Rml::Element* OutlinerRow(const std::string& action, const std::string& label) const
    {
        Rml::ElementList elements;
        document->GetElementById("scene-models")
            ->QuerySelectorAll(elements, "[data-action=" + action + "]");
        for (auto* element : elements)
            if (element->IsClassSet("list-item") && RowLabel(element) == label) return element;
        return nullptr;
    }
    // 行的展开按钮：动作在 .tree-toggle 上，标签在同一个 .tree-row 的标题按钮上。
    Rml::Element* OutlinerToggle(const std::string& label) const
    {
        for (auto* row : MountedRows())
        {
            auto* item = row->QuerySelector(".list-item");
            if (item == nullptr || RowLabel(item) != label) continue;
            return row->QuerySelector(".tree-toggle");
        }
        return nullptr;
    }
    // 行的可见性开关：与 OutlinerToggle 同一套定位方式，动作在 .tree-trailing 上。
    Rml::Element* OutlinerVisibilityToggle(const std::string& label) const
    {
        for (auto* row : MountedRows())
        {
            auto* item = row->QuerySelector(".list-item");
            if (item == nullptr || RowLabel(item) != label) continue;
            return row->QuerySelector(".tree-trailing");
        }
        return nullptr;
    }
    Rml::Element* OutlinerRowElement(const std::string& label) const
    {
        for (auto* row : MountedRows())
        {
            auto* item = row->QuerySelector(".list-item");
            if (item != nullptr && RowLabel(item) == label) return row;
        }
        return nullptr;
    }
    // 眼睛按钮里的图标路径：用来断言"可见 = eye.png / 隐藏 = eye-off.png"。
    static std::string TrailingIcon(Rml::Element* toggle)
    {
        auto* image = toggle == nullptr ? nullptr : toggle->QuerySelector("img");
        return image == nullptr ? std::string() : image->GetAttribute<std::string>("src", "");
    }
    float OutlinerRowHeight() const
    {
        const auto rows = MountedRows();
        return rows.empty() ? 0.0f : rows.front()->GetBox().GetSize(Rml::BoxArea::Margin).y;
    }
    void SettleOutliner(int frames = 4)
    {
        for (int frame = 0; frame < frames; ++frame) Refresh();
    }
    void SelectActorInOutliner(EditorSceneActorSnapshot actor)
    {
        const std::string identity = actor.identity;
        AddActor(std::move(actor));
        panel->Build(snapshot);
        snapshot.selectedActorIdentity = identity;
        panel->Build(snapshot);
        context->Update();
    }
    // 面板快照按 shared_ptr<const ...> 借用大纲列表，测试同样只发布不可变副本。
    void PublishActors()
    {
        snapshot.sceneActors =
            std::make_shared<const std::vector<EditorSceneActorSnapshot>>(actors);
    }
    void AddActor(EditorSceneActorSnapshot actor)
    {
        actors.push_back(std::move(actor));
        PublishActors();
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
    // 右键上下文菜单在 mouseup 打开：RmlUi 6.2 没有 contextmenu 事件，右键只表现为带
    // button=1 的 mousedown/mouseup。坐标必须真实经过 RmlUi 的 hover 链，否则面板拿到的
    // 目标元素不是被点的那一行。
    void RightClick(Rml::Element* element)
    {
        ASSERT_NE(element, nullptr);
        RightClickAt(element->GetAbsoluteOffset(Rml::BoxArea::Border) + Rml::Vector2f(4.0f, 4.0f));
    }
    void RightClickAt(Rml::Vector2f point)
    {
        context->ProcessMouseMove(static_cast<int>(std::lround(point.x)),
            static_cast<int>(std::lround(point.y)), 0);
        context->ProcessMouseButtonDown(1, 0);
        context->ProcessMouseButtonUp(1, 0);
        context->Update();
    }
    void ClickContextItem(const std::string& action)
    {
        auto* item = ContextMenuItem(action);
        ASSERT_NE(item, nullptr) << action;
        item->DispatchEvent("click", {});
        context->Update();
    }
    Rml::Element* ContextMenu() const { return document->GetElementById("editor-context-menu"); }
    Rml::Element* ContextMenuItem(const std::string& action) const
    {
        auto* menu = ContextMenu();
        return menu == nullptr ? nullptr
            : menu->QuerySelector("[data-action=" + action + "]");
    }
    // view mode 的条目共用 context-view-mode 动作，用 data-index（debug view 取值）区分。
    Rml::Element* ContextViewModeItem(int mode) const
    {
        auto* menu = ContextMenu();
        if (menu == nullptr) return nullptr;
        Rml::ElementList matches;
        menu->QuerySelectorAll(matches, "[data-action=context-view-mode]");
        for (auto* item : matches)
            if (item->GetAttribute<int>("data-index", -1) == mode) return item;
        return nullptr;
    }
    std::vector<Rml::Element*> ContextMenuItems() const
    {
        std::vector<Rml::Element*> items;
        auto* menu = ContextMenu();
        if (menu == nullptr) return items;
        Rml::ElementList matches;
        menu->QuerySelectorAll(matches, ".menu-item");
        for (auto* item : matches) items.push_back(item);
        return items;
    }
    std::string ContextMenuTitle() const
    {
        auto* title = document->GetElementById("editor-context-menu-title");
        return title == nullptr ? std::string() : title->GetInnerRML();
    }
    Rml::Element* BrowserItem(const std::string& path) const
    {
        Rml::ElementList items;
        document->GetElementById("asset-list")->QuerySelectorAll(items, "[data-action=browser-item]");
        for (auto* item : items)
            if (item->GetAttribute<std::string>("data-path", "") == path) return item;
        return nullptr;
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
    std::vector<EditorSceneActorSnapshot> actors;
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
        EXPECT_TRUE(document->GetElementById("menu-file-save")->HasAttribute("disabled"));
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

TEST_F(EditorUiTest, ExpandedOutlinerMeshRoutesChildSelectionThroughTheExistingSink)
{
    AddActor(MeshActor("Sphere_01", 1));
    panel->Build(snapshot);
    ASSERT_NE(document->QuerySelector("[data-action=outliner-actor]"), nullptr);
    EXPECT_EQ(document->QuerySelector("[data-action=outliner-slot]"), nullptr);
    Click("[data-action=outliner-expand]");
    panel->Build(snapshot);
    ASSERT_NE(document->QuerySelector("[data-action=outliner-slot]"), nullptr);
    Click("[data-action=outliner-slot]");
    ASSERT_TRUE(selectionSink.selectedMaterial);
    EXPECT_EQ(selectionSink.selectedMaterial->worldGeneration, 1u);
    EXPECT_EQ(selectionSink.selectedMaterial->objectId, 11u);
    EXPECT_EQ(selectionSink.selectedMaterial->materialSlotIndex, 0u);
    EXPECT_EQ(selectionSink.selectedMaterial->materialInstancePath,
        snapshot.activeDocument->assetPath);
    // 槽位选中仍走迁移前的 RequestMaterialSelection，没有第二条打开文档的路径。
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, OutlinerMeshSelectionKeepsThePreviousModelRouting)
{
    AddActor(MeshActor("Sphere_01", 1));
    panel->Build(snapshot);
    Click("[data-action=outliner-actor]");
    ASSERT_TRUE(selectionSink.selectedModel);
    EXPECT_EQ(selectionSink.selectedModel->objectId, 11u);
    EXPECT_EQ(selectionSink.selectedModel->objectIdentity, "Sphere_01");
    EXPECT_EQ(selectionSink.selectedModel->materials.size(), 1u);
    // 非网格 actor 的选择路径不能被网格行借用。
    EXPECT_TRUE(selectionSink.selectedActorIdentity.empty());
}

TEST_F(EditorUiTest, WorldReplacementClearsTreeExpansionEvenWhenObjectNamesMatch)
{
    AddActor(MeshActor("Same_Name", 1));
    panel->Build(snapshot);
    Click("[data-action=outliner-expand]");
    panel->Build(snapshot);
    ASSERT_NE(document->QuerySelector("[data-action=outliner-slot]"), nullptr);
    // 同一个名字出现在新 World 里时，展开状态必须被清掉，不能沿用旧身份。
    actors[0].worldGeneration = 2;
    for (auto& section : actors[0].sections) section.objectId = 21;
    PublishActors();
    panel->Build(snapshot);
    EXPECT_EQ(document->QuerySelector("[data-action=outliner-slot]"), nullptr);
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

TEST_F(EditorUiTest, OutlinerGroupsActorsByTypeWithStableOrderAndCount)
{
    AddActor(MeshActor("Zeta", 1));
    AddActor(MeshActor("alpha", 1, 12));
    AddActor(PointLightActor("Key_Light", 250.0f, 20.0f));
    AddActor(CameraActor("Camera_01"));
    panel->Build(snapshot);
    context->Update();

    // 分组顺序固定（低基数类型在前，Meshes 最后），组内按 ASCII 大小写不敏感排序
    // （alpha 在 Zeta 之前）。
    EXPECT_EQ(OutlinerLabels("outliner-group"),
        (std::vector<std::string>{"Point Lights (1)", "Cameras (1)", "Meshes (2)"}));
    EXPECT_EQ(OutlinerLabels("outliner-actor"),
        (std::vector<std::string>{"Key_Light", "Camera_01", "alpha", "Zeta"}));
    EXPECT_EQ(OutlinerCount(), "4 actors");
    EXPECT_FALSE(document->GetElementById("outliner-empty")->IsVisible());
    // 每次选择都要有类型图标：4 个 actor 行各一个（行尾眼睛不在 .list-item 里，
    // 由下面专门断言可见性开关的用例覆盖）。
    Rml::ElementList icons;
    document->GetElementById("scene-models")->QuerySelectorAll(icons, ".list-item img.row-icon");
    EXPECT_EQ(icons.size(), 4u);
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());
}

TEST_F(EditorUiTest, OutlinerSearchAndTypeFilterOnlyChangeVisibility)
{
    AddActor(MeshActor("Sphere_01", 1));
    AddActor(MeshActor("球体体_01", 1, 13));
    AddActor(PointLightActor("Key_Light", 250.0f, 20.0f));
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(OutlinerCount(), "3 actors");

    // ASCII 大小写折叠：查询 "SPHERE" 命中 "Sphere_01"，但不命中多字节名称。
    SetValue("outliner-search", "SPHERE");
    panel->Build(snapshot);
    EXPECT_EQ(OutlinerLabels("outliner-actor"), (std::vector<std::string>{"Sphere_01"}));
    EXPECT_EQ(OutlinerCount(), "1 / 3 actors");
    // UTF-8 多字节名称保持原样参与匹配。
    SetValue("outliner-search", "球体");
    panel->Build(snapshot);
    EXPECT_EQ(OutlinerLabels("outliner-actor"), (std::vector<std::string>{"球体体_01"}));

    SetValue("outliner-search", "");
    SelectOption("outliner-type", "pointLight");
    panel->Build(snapshot);
    EXPECT_EQ(OutlinerLabels("outliner-actor"), (std::vector<std::string>{"Key_Light"}));
    EXPECT_EQ(OutlinerLabels("outliner-group"), (std::vector<std::string>{"Point Lights (1)"}));

    // 未知过滤值按 "全部" 处理，不产生静默空结果。
    SelectOption("outliner-type", "not-a-type");
    panel->Build(snapshot);
    EXPECT_EQ(OutlinerCount(), "3 actors");

    // 搜索与过滤是纯浏览状态：不提交资产命令，也不请求运行时动作。
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());
}

TEST_F(EditorUiTest, OutlinerTypeFilterOptionsMatchTheGroupSpecs)
{
    // RmlUi 把 option 移进 select 的内部 widget，只能通过控件 API 读取选项。
    auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(
        document->GetElementById("outliner-type"));
    ASSERT_NE(select, nullptr);
    std::vector<std::string> values;
    for (int index = 0; index < select->GetNumOptions(); ++index)
        values.push_back(select->GetOption(index)->GetAttribute<std::string>("value", ""));
    std::vector<std::string> expected = {"all"};
    for (const EditorSceneActorGroupSpec& spec : SceneActorGroupSpecs())
        expected.push_back(spec.key);
    // 过滤器取值与类型清单同源，避免 RML 与分组规范各自漂移。
    EXPECT_EQ(values, expected);
    for (const EditorSceneActorGroupSpec& spec : SceneActorGroupSpecs())
    {
        EXPECT_NE(std::string(spec.icon).find("icons/"), std::string::npos) << spec.key;
        EXPECT_STRNE(SceneActorGroupTitle(spec.type), "") << spec.key;
    }
}

TEST_F(EditorUiTest, OutlinerEmptyStateDistinguishesNoWorldFromNoMatch)
{
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(OutlinerCount(), "0 actors");
    EXPECT_EQ(document->GetElementById("outliner-empty")->GetInnerRML(),
        "No actors in the active world.");
    EXPECT_TRUE(document->GetElementById("outliner-empty")->IsVisible(true));
    EXPECT_FALSE(document->GetElementById("scene-models")->IsVisible(true));

    // World 换代（0 → 1）会清掉搜索与折叠状态：先发布 actor 列表再输入查询。
    AddActor(MeshActor("Sphere_01", 1));
    panel->Build(snapshot);
    context->Update();
    SetValue("outliner-search", "no-such-actor");
    panel->Build(snapshot);
    EXPECT_EQ(document->GetElementById("outliner-empty")->GetInnerRML(),
        "No actors match the current filter.");
    EXPECT_EQ(OutlinerCount(), "0 / 1 actor");
}

TEST_F(EditorUiTest, OutlinerGroupCollapseHidesItsActorsAndKeepsTheFilterCount)
{
    AddActor(MeshActor("Sphere_01", 1));
    AddActor(MeshActor("Sphere_02", 1, 12));
    AddActor(PointLightActor("Key_Light", 250.0f, 20.0f));
    panel->Build(snapshot);
    context->Update();
    ASSERT_EQ(OutlinerLabels("outliner-actor").size(), 3u);
    // 折叠 Meshes：该组的 actor 行不再挂进 DOM，其他组不受影响。
    OutlinerRow("outliner-group", "Meshes (2)")->DispatchEvent("click", {});
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(OutlinerLabels("outliner-actor"), (std::vector<std::string>{"Key_Light"}));
    EXPECT_EQ(OutlinerCount(), "3 actors");
    // 展开回去：折叠状态只影响可见性，不改变筛选集合或 actor 身份。
    OutlinerRow("outliner-group", "Meshes (2)")->DispatchEvent("click", {});
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(OutlinerLabels("outliner-actor"),
        (std::vector<std::string>{"Key_Light", "Sphere_01", "Sphere_02"}));
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());
}

TEST_F(EditorUiTest, OutlinerNonMeshSelectionRoutesToTheActorSinkAndFillsDetails)
{
    SelectActorInOutliner(PointLightActor("Key_Point_Light", 250.0f, 12.5f));
    Click("[data-action=outliner-actor]");
    EXPECT_EQ(selectionSink.selectedActorIdentity, "pointLight|Key_Point_Light");
    // 非网格 actor 不借用网格的 MI 选择路径。
    EXPECT_FALSE(selectionSink.selectedModel.has_value());
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());

    // 冻结快照里的选中身份驱动 Details：typed 参数由同一份 actor 数据渲染。
    auto* section = document->GetElementById("details-section-actor-pointLight");
    ASSERT_NE(section, nullptr);
    EXPECT_FALSE(section->IsClassSet("collapsed"));
    EXPECT_EQ(document->GetElementById("actor-field-intensity")->GetInnerRML(), "250");
    EXPECT_EQ(document->GetElementById("actor-field-radius")->GetInnerRML(), "12.5");
    EXPECT_EQ(document->GetElementById("actor-field-position")->GetInnerRML(),
        "-3.5, 3.5, 6");
    // 点光没有方向与锥角字段。
    EXPECT_EQ(document->GetElementById("actor-field-direction"), nullptr);
    EXPECT_EQ(document->GetElementById("actor-field-cone-outer"), nullptr);
}

TEST_F(EditorUiTest, DetailsShowsTheSelectedActorWhenNoDocumentIsOpen)
{
    snapshot.activeDocument.reset();
    snapshot.selectedDocumentPath.clear();
    SelectActorInOutliner(CameraActor("Camera_01"));

    // 没有打开的 MI 文档时 Details 不再为空：actor 分组承载相机参数。
    EXPECT_EQ(document->GetElementById("details-section-general"), nullptr);
    auto* section = document->GetElementById("details-section-actor-camera");
    ASSERT_NE(section, nullptr);
    EXPECT_EQ(document->GetElementById("actor-field-hfov")->GetInnerRML(), "80");
    EXPECT_EQ(document->GetElementById("actor-field-clip-far")->GetInnerRML(), "100");
    EXPECT_EQ(document->GetElementById("actor-field-position")->GetInnerRML(), "0, 0, 16");
    EXPECT_EQ(document->GetElementById("actor-field-forward")->GetInnerRML(), "0, 0, -1");
    EXPECT_EQ(document->GetElementById("actor-field-name")->GetInnerRML(), "Camera_01");
}

TEST_F(EditorUiTest, DetailsActorFieldsFollowTheFrozenSnapshot)
{
    EditorSceneActorSnapshot light = PointLightActor("Key_Point_Light", 250.0f, 12.5f);
    const std::string identity = light.identity;
    AddActor(light);
    PublishActors();
    snapshot.selectedActorIdentity = identity;
    snapshot.activeDocument.reset();
    panel->Build(snapshot);
    context->Update();
    auto* radius = document->GetElementById("actor-field-radius");
    ASSERT_NE(radius, nullptr);
    EXPECT_EQ(radius->GetInnerRML(), "12.5");

    // 值变化只刷新这一行，不重建分组 DOM（节点身份保持）。
    actors[0].light.radius = 30.0f;
    actors[0].light.position = Eigen::Vector3f(1.0f, 2.0f, 3.0f);
    PublishActors();
    panel->Build(snapshot);
    context->Update();
    EXPECT_EQ(document->GetElementById("actor-field-radius"), radius);
    EXPECT_EQ(radius->GetInnerRML(), "30");
    EXPECT_EQ(document->GetElementById("actor-field-position")->GetInnerRML(), "1, 2, 3");
}

TEST_F(EditorUiTest, PrimaryDirectionalLightExposesTheTwoExistingRuntimeCommands)
{
    snapshot.activeDocument.reset();
    SelectActorInOutliner(SunActor(2.5f, true));

    auto* intensity = Control("actor-sun-intensity");
    ASSERT_NE(intensity, nullptr);
    EXPECT_EQ(intensity->GetValue(), "2.5");
    auto* slider = Control("actor-sun-intensity-slider");
    ASSERT_NE(slider, nullptr);
    EXPECT_EQ(slider->GetAttribute<std::string>("type", ""), "range");
    EXPECT_FLOAT_EQ(slider->GetAttribute<float>("min", -1.0f), 0.0f);
    EXPECT_FLOAT_EQ(slider->GetAttribute<float>("max", -1.0f), 10.0f);
    // 可编辑字段不出现在只读行里，同一值不会有两行。
    EXPECT_EQ(document->GetElementById("actor-field-intensity"), nullptr);

    SetValue("actor-sun-intensity", "3.5");
    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions[0].type, UiActionType::SetSunIntensity);
    EXPECT_FLOAT_EQ(runtimeSink.actions[0].floatValue, 3.5f);
    // 滑条与文本输入共用同一个 action 地址，但快照只刷新文本输入：把超范围值写进
    // 范围控件会让 RmlUi 截断它并自己派发一次 change，覆盖用户的精确输入。
    for (int frame = 0; frame < 3; ++frame)
    {
        panel->Build(snapshot);
        context->Update();
    }
    EXPECT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(slider->GetAttribute<float>("value", 0.0f), 2.5f);

    // 与运行时页一致：同值不发命令，非完整有限数只给出提示。
    SetValue("actor-sun-intensity", "2.5");
    EXPECT_EQ(runtimeSink.actions.size(), 1u);
    SetValue("actor-sun-intensity", "abc");
    Refresh();
    EXPECT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_NE(Notice().find("finite number"), std::string::npos);
    // 超出运行时页 sun 滑条的区间：明确拒绝，不提交一个会被那个隐藏控件截断的值。
    SetValue("actor-sun-intensity", "13");
    Refresh();
    EXPECT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_NE(Notice().find("between 0 and 10"), std::string::npos);

    // CSM 开关沿用运行时页的 Enabled/Disabled 按钮：一次点击只提交一条命令。
    auto* enable = document->GetElementById("actor-csm-enable");
    auto* disable = document->GetElementById("actor-csm-disable");
    ASSERT_NE(enable, nullptr);
    ASSERT_NE(disable, nullptr);
    EXPECT_TRUE(disable->IsClassSet("selected"));
    EXPECT_FALSE(enable->IsClassSet("selected"));
    Click("#actor-csm-enable");
    ASSERT_EQ(runtimeSink.actions.size(), 2u);
    EXPECT_EQ(runtimeSink.actions[1].type, UiActionType::SetCsmCastShadows);
    EXPECT_EQ(runtimeSink.actions[1].intValue, 1);
    // 已经是关闭状态：重复点击不产生第二条命令。
    Click("#actor-csm-disable");
    EXPECT_EQ(runtimeSink.actions.size(), 2u);
    // 编辑只走既有 UiAction 路由，不产生资产文档命令。
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, ActorIntensitySliderRangeMatchesTheRuntimeSunSlider)
{
    // 两个控件共用 SetSunIntensity：范围不一致时，运行时页那个隐藏滑条的 data binding
    // 会把 Details 里的超范围输入截断后回写一条新命令（渲染验证时实测到的 13→10 抖动）。
    std::ifstream runtimePage(std::string(VL_EDITOR_ASSET_ROOT) + "/runtime-control.rml");
    ASSERT_TRUE(runtimePage.is_open());
    const std::string rml((std::istreambuf_iterator<char>(runtimePage)),
        std::istreambuf_iterator<char>());
    const size_t id = rml.find("id=\"sun-intensity-slider\"");
    ASSERT_NE(id, std::string::npos);
    const size_t elementEnd = rml.find('>', id);
    ASSERT_NE(elementEnd, std::string::npos);
    const std::string element = rml.substr(id, elementEnd - id);
    const auto attribute = [&element](const std::string& name)
    {
        const size_t at = element.find(name + "=\"");
        EXPECT_NE(at, std::string::npos) << name;
        const size_t begin = at + name.size() + 2;
        return std::stof(element.substr(begin, element.find('"', begin) - begin));
    };

    snapshot.activeDocument.reset();
    SelectActorInOutliner(SunActor(1.0f, true));
    auto* slider = Control("actor-sun-intensity-slider");
    ASSERT_NE(slider, nullptr);
    EXPECT_FLOAT_EQ(slider->GetAttribute<float>("min", -1.0f), attribute("min"));
    EXPECT_FLOAT_EQ(slider->GetAttribute<float>("max", -1.0f), attribute("max"));
}

TEST_F(EditorUiTest, NonPrimaryDirectionalLightKeepsItsIntensityReadOnly)
{
    snapshot.activeDocument.reset();
    SelectActorInOutliner(SunActor(4.0f, false));
    EXPECT_EQ(document->GetElementById("actor-field-intensity")->GetInnerRML(), "4");
    EXPECT_EQ(Control("actor-sun-intensity"), nullptr);
    EXPECT_EQ(document->GetElementById("actor-csm-enable"), nullptr);
    EXPECT_EQ(document->GetElementById("actor-csm-disable"), nullptr);
}

TEST_F(EditorUiTest, OutlinerStaticFramesKeepActorFieldsAndCountCached)
{
    SelectActorInOutliner(PointLightActor("Key_Point_Light", 250.0f, 12.5f));
    auto* radius = document->GetElementById("actor-field-radius");
    ASSERT_NE(radius, nullptr);
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
    // 大纲 markup、计数与只读字段都走值缓存：静止帧写 0 次属性，也不重建节点。
    EXPECT_EQ(panel->PanelPropertyWriteCount(), writes);
    EXPECT_EQ(document->GetElementById("actor-field-radius"), radius);
    EXPECT_EQ(OutlinerCount(), "1 actor");
}

// 窗口化的行高假设：分组行/actor 行/槽行必须同高。
// 这里量真实 RmlUi 布局的 margin box（不是从 RCSS 推导），因为 indentation 只改
// padding-left，而 .tree-toggle/.tree-spacer 是同一个 26px 盒子。
TEST_F(EditorUiTest, OutlinerRemountsAfterDetachAndReattach)
{
    AddActor(MeshActor("Sphere_01", 1));
    SettleOutliner();
    ASSERT_NE(OutlinerRow("outliner-actor", "Sphere_01"), nullptr);

    // RML 热重载会在同一个调用里 Detach + Attach，而 UiSubsystem 仍逐帧 Build：
    // Detach 期间的 Build 不能触碰文档，也不能把"已挂载"记成真。
    panel->Detach();
    panel->Build(snapshot);
    std::string error;
    ASSERT_TRUE(panel->Attach(*document, error)) << error;
    SettleOutliner();
    EXPECT_NE(OutlinerRow("outliner-actor", "Sphere_01"), nullptr);
    EXPECT_EQ(OutlinerCount(), "1 actor");
    // 重新 Attach 后行高仍从已挂载行实测：spacer 只出现在内容真正的尾部。
    EXPECT_GT(OutlinerRowHeight(), 0.0f);
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, OutlinerRowHeightsAreUniformAcrossRowKinds)
{
    AddActor(MeshActor("Sphere_01", 1));
    AddActor(PointLightActor("Key_Light", 250.0f, 20.0f));
    SettleOutliner();
    panel->Build(snapshot);
    // 展开 mesh actor：槽行（depth 1）是唯一可能因缩进而变高的行形态。
    auto* toggle = OutlinerToggle("Sphere_01");
    ASSERT_NE(toggle, nullptr);
    toggle->DispatchEvent("click", {});
    SettleOutliner();

    float groupHeight = 0.0f;
    float actorHeight = 0.0f;
    float slotHeight = 0.0f;
    for (auto* element : MountedRows())
    {
        const float height = element->GetBox().GetSize(Rml::BoxArea::Margin).y;
        ASSERT_GT(height, 0.0f);
        if (element->IsClassSet("group-row")) groupHeight = height;
        else if (element->IsClassSet("slot-row")) slotHeight = height;
        else actorHeight = height;
    }
    EXPECT_GT(groupHeight, 0.0f);
    EXPECT_GT(actorHeight, 0.0f);
    EXPECT_GT(slotHeight, 0.0f);
    EXPECT_FLOAT_EQ(groupHeight, actorHeight);
    EXPECT_FLOAT_EQ(actorHeight, slotHeight);
    // 实测行高就是窗口用的行高：顶部 spacer 必须是"整行高 × 被跳过行数"。
    EXPECT_FLOAT_EQ(OutlinerRowHeight(), actorHeight);
}

TEST_F(EditorUiTest, OutlinerMountsABoundedWindowAndKeepsTheRestReachable)
{
    // 与默认球阵同一量级的场景：122 mesh + 低基数类型。
    for (int index = 0; index < 122; ++index)
        AddActor(MeshActor("Sphere_" + std::to_string(100 + index), 1,
            static_cast<RuntimeId>(100 + index)));
    AddActor(SunActor(1.0f, true));
    AddActor(PointLightActor("Key_Light", 250.0f, 20.0f));
    AddActor(CameraActor("Camera_01"));
    SettleOutliner();

    const auto rows = MountedRows();
    ASSERT_FALSE(rows.empty());
    EXPECT_LE(rows.size(), SceneActorMaxMountedRows);
    EXPECT_LT(rows.size(), 122u);
    auto* tree = document->GetElementById("scene-models");
    EXPECT_LE(static_cast<size_t>(tree->GetNumChildren()), SceneActorMaxMountedRows + 2);
    // 计数始终是筛选结果，不是挂载行数。
    EXPECT_EQ(OutlinerCount(), "125 actors");
    // 低基数分组在默认展开状态下就在窗口里：不需要手工折叠 Meshes 才能看到灯/相机/环境。
    EXPECT_NE(OutlinerRow("outliner-actor", "Sun_Light"), nullptr);
    EXPECT_NE(OutlinerRow("outliner-actor", "Key_Light"), nullptr);
    EXPECT_NE(OutlinerRow("outliner-actor", "Camera_01"), nullptr);

    // 未挂载行留在 spacer 里：滚动范围是整棵树，而不是挂载的那一段。
    // 停在顶部时只有尾部 spacer（头部没有可跳过的行），两者高度之和补齐整棵树。
    const auto spacers = OutlinerSpacers();
    ASSERT_EQ(spacers.size(), 1u);
    const float rowHeight = OutlinerRowHeight();
    ASSERT_GT(rowHeight, 0.0f);
    const auto& box = tree->GetBox();
    const float paddingY = box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top) +
        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom);
    EXPECT_NEAR(spacers.front()->GetBox().GetSize(Rml::BoxArea::Content).y,
        static_cast<float>(129 - rows.size()) * rowHeight, 1.0f);
    // 4 个分组行 + 125 个 actor 行：整棵树的滚动范围必须完整保留。
    EXPECT_NEAR(tree->GetScrollHeight(), 129.0f * rowHeight + paddingY, rowHeight);

    // 静止帧不重挂 DOM：行节点指针保持同一批。
    const auto nodes = rows;
    SettleOutliner(30);
    EXPECT_EQ(MountedRows(), nodes);
}

TEST_F(EditorUiTest, OutlinerScrollMountsAnotherWindowAndReachesTheLastActor)
{
    for (int index = 0; index < 120; ++index)
        AddActor(MeshActor("Sphere_" + std::to_string(100 + index), 1,
            static_cast<RuntimeId>(100 + index)));
    SettleOutliner();
    auto* tree = document->GetElementById("scene-models");
    const auto firstWindow = OutlinerLabels("outliner-actor");
    ASSERT_FALSE(firstWindow.empty());
    ASSERT_LT(firstWindow.size(), 120u);
    const float rowHeight = OutlinerRowHeight();
    ASSERT_GT(rowHeight, 0.0f);
    EXPECT_FLOAT_EQ(OutlinerRowHeight(), tree->QuerySelector(".tree-row")
        ->GetBox().GetSize(Rml::BoxArea::Margin).y);

    // 滚动后窗口必须换一批行：首个挂载行已经不是第一行，顶部 spacer 补上被跳过的行高。
    tree->SetScrollTop(6.0f * rowHeight);
    SettleOutliner();
    const auto scrolled = OutlinerLabels("outliner-actor");
    ASSERT_FALSE(scrolled.empty());
    EXPECT_NE(scrolled.front(), firstWindow.front());
    const auto spacers = OutlinerSpacers();
    ASSERT_EQ(spacers.size(), 2u);
    EXPECT_GT(spacers.front()->GetBox().GetSize(Rml::BoxArea::Content).y, 0.0f);

    // 滚到底：最后一行必须挂载，尾部 spacer 消失——尾页可达，不是截断到前 N 行。
    tree->SetScrollTop(tree->GetScrollHeight());
    SettleOutliner();
    EXPECT_EQ(OutlinerLabels("outliner-actor").back(), "Sphere_219");
    EXPECT_NE(OutlinerRow("outliner-actor", "Sphere_219"), nullptr);
    const auto tailSpacers = OutlinerSpacers();
    ASSERT_EQ(tailSpacers.size(), 1u);
    // 唯一剩下的 spacer 在头部，且等于被跳过的行高之和。
    EXPECT_GT(tailSpacers.front()->GetBox().GetSize(Rml::BoxArea::Content).y, 0.0f);
    EXPECT_TRUE(MountedRows().back()->IsClassSet("actor-row"));
}

TEST_F(EditorUiTest, OutlinerCollapsedTreeFitsWithoutSpacersAndKeepsTheCount)
{
    for (int index = 0; index < 122; ++index)
        AddActor(MeshActor("Sphere_" + std::to_string(100 + index), 1,
            static_cast<RuntimeId>(100 + index)));
    AddActor(PointLightActor("Key_Light", 250.0f, 20.0f));
    SettleOutliner();
    ASSERT_LT(OutlinerLabels("outliner-actor").size(), 123u);

    // 折叠 Meshes：122 个子行既不挂 DOM 也不占窗口槽位，因此不再需要 spacer。
    auto* group = OutlinerRow("outliner-group", "Meshes (122)");
    ASSERT_NE(group, nullptr);
    group->DispatchEvent("click", {});
    SettleOutliner();
    EXPECT_TRUE(OutlinerSpacers().empty());
    EXPECT_EQ(OutlinerLabels("outliner-actor"), (std::vector<std::string>{"Key_Light"}));
    // 折叠只影响挂载，不影响计数语义。
    EXPECT_EQ(OutlinerCount(), "123 actors");
    // 展开回来：子行重新进入窗口，spacer 也回来。
    OutlinerRow("outliner-group", "Meshes (122)")->DispatchEvent("click", {});
    SettleOutliner();
    EXPECT_EQ(OutlinerSpacers().size(), 1u);
    EXPECT_EQ(OutlinerCount(), "123 actors");
}

TEST_F(EditorUiTest, OutlinerSortReordersInsideGroupsAndKeepsGroupOrderAndState)
{
    for (int index = 0; index < 40; ++index)
        AddActor(MeshActor("Mesh_" + std::to_string(10 + index), 1,
            static_cast<RuntimeId>(10 + index)));
    AddActor(PointLightActor("Key_Light", 250.0f, 20.0f));
    AddActor(PointLightActor("A_Light", 250.0f, 20.0f));
    SettleOutliner();
    EXPECT_EQ(OutlinerLabels("outliner-group"),
        (std::vector<std::string>{"Point Lights (2)", "Meshes (40)"}));
    EXPECT_EQ(OutlinerLabels("outliner-actor").front(), "A_Light");

    // 排序控件只提供两种顺序，取值与 Content Browser 的 sort 控件同一套键。
    auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(
        document->GetElementById("outliner-sort"));
    ASSERT_NE(select, nullptr);
    ASSERT_EQ(select->GetNumOptions(), 2);
    EXPECT_EQ(select->GetOption(0)->GetAttribute<std::string>("value", ""), "name-asc");
    EXPECT_EQ(select->GetOption(1)->GetAttribute<std::string>("value", ""), "name-desc");
    // 过滤栏不换行：排序控件必须完整落在过滤栏矩形内，不能被裁掉。
    auto* bar = document->QuerySelector(".outliner-filterbar");
    ASSERT_NE(bar, nullptr);
    const auto barOrigin = bar->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto barSize = bar->GetBox().GetSize(Rml::BoxArea::Border);
    const auto sortOrigin = select->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto sortSize = select->GetBox().GetSize(Rml::BoxArea::Border);
    EXPECT_GT(sortSize.x, 0.0f);
    EXPECT_GE(sortOrigin.x, barOrigin.x - 1.0f);
    EXPECT_LE(sortOrigin.x + sortSize.x, barOrigin.x + barSize.x + 1.0f);

    // 组内降序：分组顺序（稳定契约）不动，Meshes 组内从 Mesh_49 开始。
    SelectOption("outliner-sort", "name-desc");
    SettleOutliner();
    EXPECT_EQ(OutlinerLabels("outliner-group"),
        (std::vector<std::string>{"Point Lights (2)", "Meshes (40)"}));
    EXPECT_EQ(OutlinerLabels("outliner-actor").front(), "Key_Light");
    EXPECT_NE(OutlinerRow("outliner-actor", "Mesh_49"), nullptr);
    // 窗口化之后降序末尾的行不在窗口里，这是窗口语义而不是排序丢失。
    EXPECT_EQ(OutlinerRow("outliner-actor", "Mesh_10"), nullptr);
    SelectOption("outliner-sort", "name-asc");
    SettleOutliner();
    EXPECT_EQ(OutlinerLabels("outliner-actor").front(), "A_Light");
    EXPECT_NE(OutlinerRow("outliner-actor", "Mesh_10"), nullptr);

    // 选中身份与展开身份都不参与排序：排序往返后选中行与槽行必须原样还在。
    snapshot.selectedActorIdentity = SceneActorIdentity(EditorSceneActorType::Mesh, "Mesh_10");
    Refresh();
    auto* expand = OutlinerToggle("Mesh_10");
    ASSERT_NE(expand, nullptr);
    expand->DispatchEvent("click", {});
    SettleOutliner();
    ASSERT_NE(document->QuerySelector("[data-action=outliner-slot]"), nullptr);
    auto* tree = document->GetElementById("scene-models");
    const float rowHeight = OutlinerRowHeight();
    ASSERT_GT(rowHeight, 0.0f);
    tree->SetScrollTop(5.0f * rowHeight);
    SettleOutliner();
    const float scrollTop = tree->GetScrollTop();
    ASSERT_GT(scrollTop, 0.0f);

    // 滚动位置保持原样：排序只重排组内顺序，不回到顶部。
    SelectOption("outliner-sort", "name-desc");
    SettleOutliner();
    EXPECT_FLOAT_EQ(tree->GetScrollTop(), scrollTop);
    EXPECT_EQ(OutlinerCount(), "42 actors");
    SelectOption("outliner-sort", "name-asc");
    SettleOutliner();
    // 回到升序仍停在原滚动位置：窗口里是 Mesh_10 附近的同一批行，不是列表顶部。
    EXPECT_FLOAT_EQ(tree->GetScrollTop(), scrollTop);
    ASSERT_NE(OutlinerRow("outliner-actor", "Mesh_10"), nullptr);
    EXPECT_TRUE(OutlinerRow("outliner-actor", "Mesh_10")->IsClassSet("selected"));
    ASSERT_NE(document->QuerySelector("[data-action=outliner-slot]"), nullptr);
    EXPECT_EQ(OutlinerCount(), "42 actors");
    // 排序是纯浏览状态：不提交资产命令，也不请求运行时动作。
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(runtimeSink.actions.empty());
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
    // 点工具条上的任意位置都会让菜单关闭；这里点标题栏空白处，因为工具条上已不再重复
    // 排列文档命令按钮（它们只在 File/Edit 菜单里）。
    Click("#editor-toolbar h1");
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

// 工具条是动作条，不是菜单项的第二次罗列。把文档命令按钮重新排回工具条会让顶栏变成一条
// 与 File/Edit 重复的按钮带（用户报的"顶部几个 save 按钮很不协调"），所以这里把它钉住。
TEST_F(EditorUiTest, ToolbarDoesNotRestateTheDocumentMenuCommands)
{
    const char* forbidden[] = {"save-asset", "revert-asset", "reset-asset",
                               "validate-asset", "reload-asset"};
    for (const char* id : forbidden)
        EXPECT_EQ(document->GetElementById(id), nullptr)
            << id << " duplicates a File/Edit menu command in the toolbar";

    // 每个文档命令在工具条里出现零次，在菜单里恰好一次。
    // 注意 #editor-menubar 本身就在 #editor-toolbar 内部，所以不能直接断言
    // "#editor-toolbar button[data-action=save] 为空" —— 那会把菜单项算成工具条按钮
    // （实测过：选择器返回的是 menu-file-save）。这里逐个排除 menubar 子树。
    const char* commands[] = {"save", "revert", "reload", "reset-all", "validate"};
    auto* toolbar = document->GetElementById("editor-toolbar");
    auto* menubar = document->GetElementById("editor-menubar");
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(menubar, nullptr);
    for (const char* command : commands)
    {
        const std::string selector = std::string("[data-action=") + command + "]";
        Rml::ElementList matches;
        toolbar->QuerySelectorAll(matches, selector);
        int outsideMenu = 0;
        for (Rml::Element* element : matches)
        {
            bool insideMenu = false;
            for (Rml::Element* ancestor = element; ancestor != nullptr;
                 ancestor = ancestor->GetParentNode())
                if (ancestor == menubar) { insideMenu = true; break; }
            if (!insideMenu) ++outsideMenu;
        }
        EXPECT_EQ(outsideMenu, 0)
            << "toolbar restates the document command outside the menu: " << command;
        EXPECT_EQ(static_cast<int>(matches.size()), 1)
            << "document command must exist exactly once, in the menu: " << command;
    }

    // 防止上面的断言空洞通过：工具条在 menubar 之外确实还有按钮，
    // 断言必须建立在真正遍历到这些按钮的基础上。
    Rml::ElementList toolbarButtons;
    toolbar->QuerySelectorAll(toolbarButtons, "button");
    int outside = 0;
    for (Rml::Element* element : toolbarButtons)
    {
        bool insideMenu = false;
        for (Rml::Element* ancestor = element; ancestor != nullptr;
             ancestor = ancestor->GetParentNode())
            if (ancestor == menubar) { insideMenu = true; break; }
        if (!insideMenu) ++outside;
    }
    EXPECT_EQ(outside, 3) << "expected exactly Reset Layout / Settings / Close Editor; "
                             "if this changed, re-check the assertions above are not vacuous";
    EXPECT_NE(toolbar->QuerySelector("[data-action=workspace-reset]"), nullptr);
}

// 空标签没有意义，而且用户实际看到过 —— 选中标签 94px 宽、整条高度内既无文字也无图标。
// 成因是 resource 文档面板在 resourceTabs 里找不到自己时整段跳过 inner RML 写入。
// `#document-tabs`（Details 区 Material Instance 标题下那排标签）此前**没有任何测试**，
// 因此它的形态问题能一直存在。这里用真实的 Bistro 资产名推入多个文档，钉住两件事：
// 每个标签都必须渲染出来，且每个标签的文字必须只占一行 —— 标签是一行文字，不是磁贴。
TEST_F(EditorUiTest, DocumentTabRowKeepsEveryOpenDocumentOnOneLine)
{
    // 名字长度接近真实资产（Bistro 的 MI 名就是这个量级），足以暴露换行与截断。
    const std::vector<std::string> paths = {
        "Maps/SC_bistro_exterior_modular/Materials/MI_Bistro_Modular_Antenna_Metal.json",
        "Maps/SC_bistro_exterior_modular/Materials/MI_Bistro_Modular_Ashtray.json",
        "Maps/SC_bistro_exterior_modular/Materials/MI_Bistro_Awning_Fabric_Cv.json",
    };
    snapshot.documentTabs.clear();
    for (const std::string& path : paths)
        snapshot.documentTabs.push_back({path, false});
    snapshot.selectedDocumentPath = paths[1];
    Refresh();

    auto* strip = document->GetElementById("document-tabs");
    ASSERT_NE(strip, nullptr);

    // 每个打开的文档都要有一个标签；漏掉一个就等于"多开"丢文档。
    const int children = strip->GetNumChildren();
    EXPECT_EQ(children, static_cast<int>(paths.size()))
        << "expected one tab per open document, inner RML: " << strip->GetInnerRML();

    // 标签文字必须在一行内。`#document-tabs` 是 display:flex 且 max-height:65px，一旦标签
    // 内容自动换行，标签就会长成两三行高、把整排撑到限高并开始裁剪 —— 这正是"多开以后
    // Details 那块很奇怪"的样子。这里用元素高度钉住：单个标签不得高于一排文字。
    int index = 0;
    for (int childIndex = 0; childIndex < children; ++childIndex)
    {
        Rml::Element* tab = strip->GetChild(childIndex);
        ASSERT_NE(tab, nullptr);
        const float height = tab->GetBox().GetSize().y;
        EXPECT_GT(height, 0.0f) << "tab " << childIndex << " has no height";
        EXPECT_LE(height, 30.0f)
            << "document tab " << childIndex << " is " << height
            << "px tall; a one-line tab should not wrap. Inner RML: " << tab->GetInnerRML();
        ++index;
    }
    EXPECT_EQ(index, static_cast<int>(paths.size()));
}

// 多开资产时，两处"标签"必须各自正确：
//   (a) `#document-tabs` —— Details 区打开的 MI 文档标签；
//   (b) `#resource-editor` —— 从 Content Browser 打开非 MI 资产时出现的资源页。
// 之前 (a) 完全没有测试，这个用例补上，并且专门检查**多个**文档下的行为。
TEST_F(EditorUiTest, OpeningSeveralDocumentsFillsTheDetailsTabRowWithReadableTabs)
{
    const std::vector<std::string> paths = {
        "Maps/SC_bistro/Materials/MI_Antenna_Metal.json",
        "Maps/SC_bistro/Materials/MI_Ashtray.json",
        "Maps/SC_bistro/Materials/MI_Awning_Fabric.json",
    };
    snapshot.documentTabs.clear();
    for (const std::string& path : paths)
        snapshot.documentTabs.push_back({path, false});
    snapshot.selectedDocumentPath = paths[0];
    // details 面板必须在场：viewingJson / resourceEditorVisible 都会把整块藏掉。
    snapshot.jsonDocument.reset();
    Refresh();

    auto* strip = document->GetElementById("document-tabs");
    ASSERT_NE(strip, nullptr);
    ASSERT_EQ(strip->GetNumChildren(), static_cast<int>(paths.size()))
        << "one tab per open document; inner RML: " << strip->GetInnerRML();

    // 每个标签都要有非空可见文字，并且文字是它自己的文件名而不是别人的。
    for (size_t index = 0; index < paths.size(); ++index)
    {
        Rml::Element* tab = strip->GetChild(static_cast<int>(index));
        ASSERT_NE(tab, nullptr);
        const std::string markup = tab->GetInnerRML();
        const std::string filename =
            std::filesystem::path(paths[index]).filename().string();
        EXPECT_NE(markup.find(filename), std::string::npos)
            << "tab " << index << " should carry its own filename " << filename
            << ", markup: " << markup;
    }

    // 点第 3 个标签必须选中第 3 个文档。标签提交的是 SelectMaterialInstanceDocument
    // （切换已打开的文档），不是 OpenMaterialInstanceAsset（那是从资产磁贴新开一个）。
    // 注意：动作挂在标签**内部**的 list-item 按钮上，外层只是 .document-tab 容器，
    // 所以必须点内层按钮；点容器不会派发任何东西。
    auto* thirdTab = strip->GetChild(2)->QuerySelector("[data-action=tab]");
    ASSERT_NE(thirdTab, nullptr);
    thirdTab->DispatchEvent("click", {});
    ASSERT_FALSE(sink.commands.empty());
    EXPECT_EQ(sink.commands.back().type, EditorCommandType::SelectMaterialInstanceDocument);
    const auto& selected =
        std::get<MaterialInstanceAssetPathPayload>(sink.commands.back().payload);
    EXPECT_EQ(selected.assetPath, paths[2])
        << "clicking the third tab must select the third document, not the first";
}

TEST_F(EditorUiTest, EveryDockTabCarriesAVisibleLabel)
{
    for (const char* panel : {"viewport", "outliner", "details", "content", "renderer", "resource"})
    {
        auto* tab = document->GetElementById(std::string("workspace-") + panel + "-tab");
        ASSERT_NE(tab, nullptr) << panel;
        const std::string markup = tab->GetInnerRML();
        EXPECT_FALSE(markup.empty()) << panel << " tab has no inner RML at all";
        // 去掉标签后必须还有可见文字，否则标签只剩一个图标。
        std::string text = markup;
        for (std::string::size_type tag = text.find('<'); tag != std::string::npos;
             tag = text.find('<', tag))
        {
            const auto close = text.find('>', tag);
            if (close == std::string::npos) break;
            text.erase(tag, close - tag + 1);
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
            text.erase(text.begin());
        EXPECT_FALSE(text.empty()) << panel << " tab renders no label text: " << markup;
    }
}

TEST_F(EditorUiTest, FileMenuDocumentCommandsShareTheToolbarAvailability)
{
    // 有活动 MI 文档时 File/Edit 的文档命令可用（工具条不再重复这些命令）。
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

// ---------------------------------------------------------------------------
// 逐 actor 可见性：World / 快照侧回归。
// 这里刻意用真实 World + 真实 WorldSnapshotBuilder（两个源文件编进 editor_ui_tests）：
// 过滤发生在快照构建处、渲染器不再做第二次判断，因此"隐藏 = 不可见"必须在快照这一层
// 被证明，而不是只证明大纲那一行的图标变了。
// ---------------------------------------------------------------------------

WorldMeshObject VisibilityTestMesh(const std::string& name)
{
    WorldMeshObject object;
    object.debugName = name;
    object.sceneObjectIdentity = name;
    object.meshKey = "SM_test|0|Body";
    object.materialKey = "M_test";
    object.materialInstanceKey = "Maps/SC_test/Materials/MI_test.json";
    object.localBoundsMin = Eigen::Vector3f(-1.0f, -1.0f, -1.0f);
    object.localBoundsMax = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
    return object;
}

World MakeVisibilityTestWorld()
{
    World world(7, "Maps/SC_test/SC_test.json");
    auto camera = std::make_shared<Camera>();
    // 不走 Camera 默认构造里的窗口尺寸读取：测试不依赖 config/ 的当前工作目录。
    camera->SetProjection(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    world.SetCamera(std::move(camera));
    world.AddMeshObject("Sphere_A", VisibilityTestMesh("Sphere_A"));
    world.AddMeshObject("Sphere_B", VisibilityTestMesh("Sphere_B"));
    world.AddDirectionalLight("Sun_Light", std::make_shared<DirectionalLight>());
    world.AddPointLight("Key_Light", std::make_shared<PointLight>());
    world.AddSquareAreaLight("Area_Light", WorldSquareAreaLight{});
    return world;
}

WorldSnapshot BuildVisibilityTestSnapshot(const World& world)
{
    WorldSnapshotBuilder builder;
    WorldSnapshotBuildDesc desc;
    desc.worldGeneration = world.GetGeneration();
    auto result = builder.Build(world, desc);
    EXPECT_FALSE(result.IsFailure());
    return result.IsFailure() ? WorldSnapshot{} : std::move(result.Value());
}

size_t CountLightSnapshots(const WorldSnapshot& snapshot, LightSnapshotType type)
{
    size_t count = 0;
    for (const LightSnapshot& light : snapshot.lights)
        if (light.type == type) ++count;
    return count;
}

size_t CountMeshDraws(const WorldSnapshot& snapshot, const std::string& debugName)
{
    size_t count = 0;
    for (const MeshDrawSnapshot& draw : snapshot.meshDraws)
        if (draw.debugName == debugName) ++count;
    return count;
}

TEST_F(EditorUiTest, WorldVisibilitySettersRejectNamesThatAreNotInTheActiveWorld)
{
    World world = MakeVisibilityTestWorld();

    // 默认全部可见：可见性是运行时状态，重载场景一律从"全部可见"开始。
    for (const auto& entry : world.GetMeshObjects()) EXPECT_TRUE(entry.second.visible);
    EXPECT_TRUE(world.GetPrimaryDirectionalLight()->IsVisible());

    EXPECT_TRUE(world.SetMeshSceneObjectVisible("Sphere_A", false));
    EXPECT_FALSE(world.GetMeshObjects().at("Sphere_A").visible);
    EXPECT_TRUE(world.GetMeshObjects().at("Sphere_B").visible);

    EXPECT_TRUE(world.SetLightVisible("Sun_Light", false));
    EXPECT_FALSE(world.GetPrimaryDirectionalLight()->IsVisible());
    EXPECT_TRUE(world.GetPointLights().at("Key_Light")->IsVisible());

    EXPECT_TRUE(world.SetSquareAreaLightVisible("Area_Light", false));
    EXPECT_FALSE(world.GetSquareAreaLights().at("Area_Light").visible);

    // 陈旧或伪造身份必须报告失败，而不是静默改不到东西或套用到别的对象上。
    EXPECT_FALSE(world.SetMeshSceneObjectVisible("Sphere_Gone", false));
    EXPECT_FALSE(world.SetLightVisible("Sphere_A", false));
    EXPECT_FALSE(world.SetSquareAreaLightVisible("Key_Light", false));
}

TEST_F(EditorUiTest, SnapshotBuilderOmitsInvisibleMeshesAndLights)
{
    World world = MakeVisibilityTestWorld();

    const WorldSnapshot visible = BuildVisibilityTestSnapshot(world);
    ASSERT_EQ(visible.meshDraws.size(), 2u);
    EXPECT_EQ(CountLightSnapshots(visible, LightSnapshotType::Directional), 1u);
    EXPECT_EQ(CountLightSnapshots(visible, LightSnapshotType::Point), 1u);
    // 方形面光同样按可见性过滤：实时路径目前不接受它，但快照语义要一致。
    EXPECT_EQ(CountLightSnapshots(visible, LightSnapshotType::SquareArea), 1u);

    ASSERT_TRUE(world.SetMeshSceneObjectVisible("Sphere_A", false));
    ASSERT_TRUE(world.SetLightVisible("Sun_Light", false));
    ASSERT_TRUE(world.SetSquareAreaLightVisible("Area_Light", false));

    const WorldSnapshot hidden = BuildVisibilityTestSnapshot(world);
    // meshDraws 同时是出图、拾取、MaterialInstance 上下文与阴影 pass 的唯一来源，
    // 因此"不可见"在这一处生效就等于对整条渲染路径生效。
    ASSERT_EQ(hidden.meshDraws.size(), 1u);
    EXPECT_EQ(CountMeshDraws(hidden, "Sphere_A"), 0u);
    EXPECT_EQ(CountMeshDraws(hidden, "Sphere_B"), 1u);
    EXPECT_EQ(CountLightSnapshots(hidden, LightSnapshotType::Directional), 0u);
    EXPECT_EQ(CountLightSnapshots(hidden, LightSnapshotType::Point), 1u);
    EXPECT_EQ(CountLightSnapshots(hidden, LightSnapshotType::SquareArea), 0u);

    // 重新显示必须回到与最初一致的数量与身份。
    ASSERT_TRUE(world.SetMeshSceneObjectVisible("Sphere_A", true));
    ASSERT_TRUE(world.SetLightVisible("Sun_Light", true));
    ASSERT_TRUE(world.SetSquareAreaLightVisible("Area_Light", true));
    const WorldSnapshot restored = BuildVisibilityTestSnapshot(world);
    EXPECT_EQ(restored.meshDraws.size(), visible.meshDraws.size());
    EXPECT_EQ(restored.lights.size(), visible.lights.size());
    EXPECT_EQ(CountMeshDraws(restored, "Sphere_A"), 1u);
    EXPECT_EQ(CountLightSnapshots(restored, LightSnapshotType::Directional), 1u);
}

TEST_F(EditorUiTest, SceneActorSnapshotPublishesTheWorldVisibilityFlag)
{
    World world = MakeVisibilityTestWorld();
    ASSERT_TRUE(world.SetMeshSceneObjectVisible("Sphere_A", false));
    ASSERT_TRUE(world.SetLightVisible("Sun_Light", false));

    const std::vector<EditorSceneActorSnapshot> actors = BuildEditorSceneActors(world);
    ASSERT_FALSE(actors.empty());

    bool sawHiddenMesh = false;
    bool sawVisibleMesh = false;
    bool sawHiddenSun = false;
    bool sawVisiblePointLight = false;
    for (const EditorSceneActorSnapshot& actor : actors)
    {
        if (actor.identity == "mesh|Sphere_A")
        {
            sawHiddenMesh = true;
            EXPECT_FALSE(actor.visible);
        }
        if (actor.identity == "mesh|Sphere_B")
        {
            sawVisibleMesh = true;
            EXPECT_TRUE(actor.visible);
        }
        if (actor.identity == "directionalLight|Sun_Light")
        {
            sawHiddenSun = true;
            EXPECT_FALSE(actor.visible);
        }
        if (actor.identity == "pointLight|Key_Light")
        {
            sawVisiblePointLight = true;
            EXPECT_TRUE(actor.visible);
        }
        // 相机/环境/风剖面没有渲染侧可见性，因此恒为可见，也不显示眼睛。
        if (actor.type == EditorSceneActorType::Camera ||
            actor.type == EditorSceneActorType::Environment)
        {
            EXPECT_TRUE(actor.visible);
        }
    }
    EXPECT_TRUE(sawHiddenMesh);
    EXPECT_TRUE(sawVisibleMesh);
    EXPECT_TRUE(sawHiddenSun);
    EXPECT_TRUE(sawVisiblePointLight);

    // 大纲眼睛的可寻址身份必须就是这里发布的稳定身份，否则执行端会拒绝命令。
    EditorSceneActorIdentity parsed;
    ASSERT_TRUE(ParseSceneActorIdentity("directionalLight|Sun_Light", parsed));
    EXPECT_EQ(parsed.type, EditorSceneActorType::DirectionalLight);
    EXPECT_EQ(parsed.name, "Sun_Light");
}

TEST_F(EditorUiTest, OutlinerVisibilityToggleQueuesOneCommandAndDisturbsNothingElse)
{
    AddActor(MeshActor("Sphere_01", 1));
    AddActor(SunActor(1.0f, true));
    AddActor(CameraActor("Camera_01"));
    SettleOutliner();

    // 只有真正有渲染侧可见性状态的对象才有眼睛；相机没有，因此不为它摆一个假控件。
    ASSERT_NE(OutlinerVisibilityToggle("Sphere_01"), nullptr);
    ASSERT_NE(OutlinerVisibilityToggle("Sun_Light"), nullptr);
    EXPECT_EQ(OutlinerVisibilityToggle("Camera_01"), nullptr);

    // 挂载前记录会被"点眼睛"影响的一切：行高、展开标记、计数、选中态与滚动偏移。
    const float rowHeight = OutlinerRowHeight();
    const size_t rowCount = MountedRows().size();
    auto* tree = document->GetElementById("scene-models");
    const float scrollBefore = tree->GetScrollTop();
    auto* expandToggle = OutlinerToggle("Sphere_01");
    ASSERT_NE(expandToggle, nullptr);
    const std::string expandBefore = expandToggle->GetInnerRML();

    auto* eye = OutlinerVisibilityToggle("Sphere_01");
    ASSERT_NE(eye, nullptr);
    eye->DispatchEvent("click", {});

    // 恰好一条运行时命令，走既有 UiAction 路由；不提交任何资产命令，也不改选择。
    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.front().type, UiActionType::SetActorVisible);
    EXPECT_EQ(runtimeSink.actions.front().stringValue, "mesh|Sphere_01");
    EXPECT_EQ(runtimeSink.actions.front().intValue, 0);
    EXPECT_TRUE(sink.commands.empty());
    EXPECT_TRUE(selectionSink.selectedActorIdentity.empty());
    EXPECT_TRUE(selectionSink.selectedModel.has_value() == false);
    EXPECT_TRUE(selectionSink.selectedMaterial.has_value() == false);

    // 行几何、展开状态、计数与滚动位置都不受影响：点的不是行本身。
    EXPECT_FLOAT_EQ(OutlinerRowHeight(), rowHeight);
    EXPECT_EQ(MountedRows().size(), rowCount);
    EXPECT_FLOAT_EQ(tree->GetScrollTop(), scrollBefore);
    ASSERT_NE(OutlinerToggle("Sphere_01"), nullptr);
    EXPECT_EQ(OutlinerToggle("Sphere_01")->GetInnerRML(), expandBefore);

    // 眼睛必须真的落在树的可视矩形内：它曾经是绝对定位 + 未约束尺寸的 32px 图标，
    // 布局上存在、像素上完全不可见，合成点击仍然"通过"。这条断言把几何钉住。
    auto* eyeRow = OutlinerRowElement("Sphere_01");
    ASSERT_NE(eyeRow, nullptr);
    auto* eyeAfter = OutlinerVisibilityToggle("Sphere_01");
    ASSERT_NE(eyeAfter, nullptr);
    const auto eyeOrigin = eyeAfter->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto eyeSize = eyeAfter->GetBox().GetSize(Rml::BoxArea::Border);
    const auto treeOrigin = tree->GetAbsoluteOffset(Rml::BoxArea::Border);
    const auto treeSize = tree->GetBox().GetSize(Rml::BoxArea::Border);
    EXPECT_GT(eyeSize.x, 0.0f);
    EXPECT_GT(eyeSize.y, 0.0f);
    EXPECT_GE(eyeOrigin.x, treeOrigin.x);
    EXPECT_LE(eyeOrigin.x + eyeSize.x, treeOrigin.x + treeSize.x + 0.5f);
    // 图标本身也必须真的有 14px 的非零盒，否则按钮在、像素不在。
    auto* eyeImage = eyeAfter->QuerySelector("img");
    ASSERT_NE(eyeImage, nullptr);
    const auto imageSize = eyeImage->GetBox().GetSize(Rml::BoxArea::Border);
    EXPECT_FLOAT_EQ(imageSize.x, 14.0f);
    EXPECT_FLOAT_EQ(imageSize.y, 14.0f);
    // 行高仍然由 RCSS 固定为 30px：眼睛不能把行撑高。
    EXPECT_FLOAT_EQ(eyeRow->GetBox().GetSize(Rml::BoxArea::Margin).y, rowHeight);
}

TEST_F(EditorUiTest, HiddenActorRowShowsEyeOffAndStillRoutesDetailsAndSelection)
{
    AddActor(MeshActor("Sphere_01", 1));
    AddActor(SunActor(1.0f, true));
    SettleOutliner();

    EXPECT_EQ(TrailingIcon(OutlinerVisibilityToggle("Sphere_01")), "icons/eye.png");
    ASSERT_NE(OutlinerRowElement("Sphere_01"), nullptr);
    EXPECT_FALSE(OutlinerRowElement("Sphere_01")->IsClassSet("hidden-row"));

    // 运行时把该 actor 置为隐藏；面板只消费冻结快照，不访问 World。
    actors[0].visible = false;
    PublishActors();
    SettleOutliner();

    // 隐藏必须有两个可测信号：图标换成 eye-off，行得到 hidden-row class。
    EXPECT_EQ(TrailingIcon(OutlinerVisibilityToggle("Sphere_01")), "icons/eye-off.png");
    ASSERT_NE(OutlinerRowElement("Sphere_01"), nullptr);
    EXPECT_TRUE(OutlinerRowElement("Sphere_01")->IsClassSet("hidden-row"));
    // 相邻 actor 与筛选计数不受影响：隐藏不改变行集合，也不改变"有哪些对象"。
    EXPECT_EQ(TrailingIcon(OutlinerVisibilityToggle("Sun_Light")), "icons/eye.png");
    EXPECT_EQ(OutlinerCount(), "2 actors");

    // 隐藏的行仍然可选中、仍驱动 Details：隐藏不是"从大纲里移除"，
    // 也不关闭它的材质实例文档（mesh actor 的槽行与 MI 路由都不看 visible）。
    snapshot.activeDocument.reset();
    snapshot.selectedDocumentPath.clear();
    panel->Build(snapshot);
    snapshot.selectedActorIdentity = "mesh|Sphere_01";
    panel->Build(snapshot);
    context->Update();
    EXPECT_NE(document->GetElementById("details-section-actor-mesh"), nullptr);
    EXPECT_NE(OutlinerRow("outliner-actor", "Sphere_01"), nullptr);
    // 点击隐藏行仍然走原来的 mesh actor 选择路由。
    OutlinerRow("outliner-actor", "Sphere_01")->DispatchEvent("click", {});
    EXPECT_EQ(selectionSink.selectedModel.has_value(), true);
    // 但"点眼睛"本身不产生选择：清空后单独点眼睛，选择 sink 必须保持不变。
    selectionSink.selectedModel.reset();
    auto* eye = OutlinerVisibilityToggle("Sphere_01");
    ASSERT_NE(eye, nullptr);
    eye->DispatchEvent("click", {});
    EXPECT_FALSE(selectionSink.selectedModel.has_value());
    // 眼睛再次点击发的是"显示"而非"隐藏"，且身份仍是稳定字符串。
    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.front().type, UiActionType::SetActorVisible);
    EXPECT_EQ(runtimeSink.actions.front().stringValue, "mesh|Sphere_01");
    EXPECT_EQ(runtimeSink.actions.front().intValue, 1);

    // 恢复可见后回到初始态：眼睛图标与行 class 都必须还原。
    actors[0].visible = true;
    PublishActors();
    SettleOutliner();
    EXPECT_EQ(TrailingIcon(OutlinerVisibilityToggle("Sphere_01")), "icons/eye.png");
    ASSERT_NE(OutlinerRowElement("Sphere_01"), nullptr);
    EXPECT_FALSE(OutlinerRowElement("Sphere_01")->IsClassSet("hidden-row"));
}

// ---------------------------------------------------------------------------
// 多 section 网格的整 actor 可见性：大纲的一行代表整个场景网格对象，
// 写入端必须按 sceneObjectIdentity 一次写全它的所有 section。
// 这里仍然用真实 World + 真实 WorldSnapshotBuilder + 真实 actor builder：
// 本次修的是"大纲身份"与"World 键"的失配，只有真实两端才能证明它们对齐。
// ---------------------------------------------------------------------------

// 一个 section 的 WorldMeshObject：对象名是 meshObjects 的键，场景对象身份是
// 大纲聚合 actor 用的键；单 section 对象两者相同，多 section 对象共享身份。
WorldMeshObject VisibilitySection(
    const std::string& objectName,
    const std::string& sceneObjectIdentity,
    uint32_t materialSlotIndex,
    bool visible = true)
{
    WorldMeshObject object = VisibilityTestMesh(objectName);
    object.sceneObjectIdentity = sceneObjectIdentity;
    object.materialSlotIndex = materialSlotIndex;
    object.visible = visible;
    return object;
}

// 真实多 section 模型的命名形态（rendererMeshLoader 的规则）：section 对象名是
// "<场景对象名>::<section 名>"，身份则是场景对象名本身。车轮是 3 个材质槽的网格。
World MakeMultiSectionVisibilityTestWorld()
{
    World world = MakeVisibilityTestWorld();
    world.AddMeshObject("SM_car_wheel_bl::M_Wheel_Tread",
        VisibilitySection("SM_car_wheel_bl::M_Wheel_Tread", "SM_car_wheel_bl", 0));
    world.AddMeshObject("SM_car_wheel_bl::M_Wheel_Brake",
        VisibilitySection("SM_car_wheel_bl::M_Wheel_Brake", "SM_car_wheel_bl", 1));
    world.AddMeshObject("SM_car_wheel_bl::M_Wheel_Hub",
        VisibilitySection("SM_car_wheel_bl::M_Wheel_Hub", "SM_car_wheel_bl", 2));
    return world;
}

size_t CountMeshDrawsBySceneObjectIdentity(const WorldSnapshot& snapshot, const std::string& identity)
{
    size_t count = 0;
    for (const MeshDrawSnapshot& draw : snapshot.meshDraws)
        if (draw.sceneObjectIdentity == identity) ++count;
    return count;
}

std::vector<const EditorSceneActorSnapshot*> FindActorSnapshot(
    const std::vector<EditorSceneActorSnapshot>& actors, const std::string& sceneObjectIdentity)
{
    std::vector<const EditorSceneActorSnapshot*> matches;
    for (const EditorSceneActorSnapshot& actor : actors)
        if (actor.type == EditorSceneActorType::Mesh && actor.sceneObjectIdentity == sceneObjectIdentity)
            matches.push_back(&actor);
    return matches;
}

TEST_F(EditorUiTest, MultiSectionMeshVisibilityAppliesToEverySectionAtOnce)
{
    World world = MakeMultiSectionVisibilityTestWorld();
    const std::string identity = "SM_car_wheel_bl";

    // 前提：这个 actor 确实由三个 section 组成，且它的身份不是任何一个 section 的键。
    ASSERT_EQ(world.GetMeshObjects().count(identity), 0u);
    ASSERT_EQ(world.GetMeshObjects().count("SM_car_wheel_bl::M_Wheel_Hub"), 1u);

    std::vector<EditorSceneActorSnapshot> actors = BuildEditorSceneActors(world);
    std::vector<const EditorSceneActorSnapshot*> actorMatches = FindActorSnapshot(actors, identity);
    ASSERT_EQ(actorMatches.size(), 1u);
    const EditorSceneActorSnapshot* actor = actorMatches.front();
    EXPECT_EQ(actor->identity, "mesh|SM_car_wheel_bl");
    EXPECT_EQ(actor->sections.size(), 3u);
    EXPECT_TRUE(actor->visible);
    ASSERT_TRUE(world.SetLightVisible("Sun_Light", false));

    // 按身份隐藏：三个 section 必须一起改变可见性。修复前这里返回 false，
    // meshObjects 里根本没有以身份为键的条目。
    ASSERT_TRUE(world.SetMeshSceneObjectVisible(identity, false));
    EXPECT_FALSE(world.GetMeshObjects().at("SM_car_wheel_bl::M_Wheel_Tread").visible);
    EXPECT_FALSE(world.GetMeshObjects().at("SM_car_wheel_bl::M_Wheel_Brake").visible);
    EXPECT_FALSE(world.GetMeshObjects().at("SM_car_wheel_bl::M_Wheel_Hub").visible);
    // 另一个 actor 不受影响。
    EXPECT_TRUE(world.GetMeshObjects().at("Sphere_A").visible);

    // 快照必须同时省略全部三个 section；灯光过滤同帧生效，证明这一帧真的重新构建过。
    const WorldSnapshot hidden = BuildVisibilityTestSnapshot(world);
    EXPECT_EQ(CountMeshDrawsBySceneObjectIdentity(hidden, identity), 0u);
    EXPECT_EQ(CountMeshDraws(hidden, "SM_car_wheel_bl::M_Wheel_Tread"), 0u);
    EXPECT_EQ(CountMeshDraws(hidden, "SM_car_wheel_bl::M_Wheel_Brake"), 0u);
    EXPECT_EQ(CountMeshDraws(hidden, "SM_car_wheel_bl::M_Wheel_Hub"), 0u);
    EXPECT_EQ(CountLightSnapshots(hidden, LightSnapshotType::Directional), 0u);
    EXPECT_EQ(CountMeshDraws(hidden, "Sphere_A"), 1u);

    // actor 快照读的是冻结的 World：整行隐藏，且"任一 section 可见"的读数与
    // 逐 section 事实一致（点击之后两侧必然一致，不存在半隐藏的行）。
    actors = BuildEditorSceneActors(world);
    actorMatches = FindActorSnapshot(actors, identity);
    ASSERT_EQ(actorMatches.size(), 1u);
    EXPECT_FALSE(actorMatches.front()->visible);
    EXPECT_FALSE(world.IsMeshSceneObjectVisible(identity));
    for (const EditorSceneMeshSectionSnapshot& section : actorMatches.front()->sections)
        EXPECT_FALSE(world.GetMeshObjects().at(section.objectName).visible);

    // 重新显示同样一次写全：三个 section 全部回到可见，快照恢复三份 draw。
    ASSERT_TRUE(world.SetMeshSceneObjectVisible(identity, true));
    EXPECT_TRUE(world.GetMeshObjects().at("SM_car_wheel_bl::M_Wheel_Tread").visible);
    EXPECT_TRUE(world.GetMeshObjects().at("SM_car_wheel_bl::M_Wheel_Brake").visible);
    EXPECT_TRUE(world.GetMeshObjects().at("SM_car_wheel_bl::M_Wheel_Hub").visible);
    EXPECT_TRUE(world.IsMeshSceneObjectVisible(identity));
    actors = BuildEditorSceneActors(world);
    actorMatches = FindActorSnapshot(actors, identity);
    ASSERT_EQ(actorMatches.size(), 1u);
    EXPECT_TRUE(actorMatches.front()->visible);

    const WorldSnapshot shown = BuildVisibilityTestSnapshot(world);
    EXPECT_EQ(CountMeshDrawsBySceneObjectIdentity(shown, identity), 3u);
}

TEST_F(EditorUiTest, InconsistentSectionsToggleToAllHiddenWhenAnySectionIsVisible)
{
    World world = MakeVisibilityTestWorld();
    const std::string identity = "Sphere_A";

    // 先造出不一致状态。写入端总是整 actor 写，所以这个状态本轮不可达，
    // 但字段默认可见、上一版构建可能留下混合状态，因此规则必须确定：
    // 行按"任一 section 可见"显示为可见，眼睛发出的就是这一读数的反面，
    // 即第一次点击的目标态是"全部隐藏"，一次点击后所有 section 必然一致。
    world.AddMeshObject("Sphere_A::Section_Body",
        VisibilitySection("Sphere_A::Section_Body", identity, 0));
    world.AddMeshObject("Sphere_A::Section_Glass",
        VisibilitySection("Sphere_A::Section_Glass", identity, 1, false));

    std::vector<EditorSceneActorSnapshot> actors = BuildEditorSceneActors(world);
    std::vector<const EditorSceneActorSnapshot*> matches = FindActorSnapshot(actors, identity);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_TRUE(matches.front()->visible);
    EXPECT_EQ(matches.front()->sections.size(), 3u);
    EXPECT_TRUE(world.IsMeshSceneObjectVisible(identity));
    // 行状态取自与建造相同的 World 状态，不是另一条规则。
    EXPECT_EQ(matches.front()->visible, world.IsMeshSceneObjectVisible(identity));

    // 不一致状态下按身份写入 = 全部隐藏（与行上眼睛发出的 0 一致）。
    ASSERT_TRUE(world.SetMeshSceneObjectVisible(identity, false));
    EXPECT_FALSE(world.GetMeshObjects().at("Sphere_A").visible);
    EXPECT_FALSE(world.GetMeshObjects().at("Sphere_A::Section_Body").visible);
    EXPECT_FALSE(world.GetMeshObjects().at("Sphere_A::Section_Glass").visible);
    actors = BuildEditorSceneActors(world);
    matches = FindActorSnapshot(actors, identity);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_FALSE(matches.front()->visible);
    const WorldSnapshot hidden = BuildVisibilityTestSnapshot(world);
    EXPECT_EQ(CountMeshDrawsBySceneObjectIdentity(hidden, identity), 0u);
    EXPECT_EQ(CountMeshDraws(hidden, "Sphere_A::Section_Body"), 0u);
    EXPECT_EQ(CountMeshDraws(hidden, "Sphere_A::Section_Glass"), 0u);

    // 第二次点击必然写"全部可见"：收敛后两个方向都不再留下混合状态。
    ASSERT_TRUE(world.SetMeshSceneObjectVisible(identity, true));
    EXPECT_TRUE(world.GetMeshObjects().at("Sphere_A").visible);
    EXPECT_TRUE(world.GetMeshObjects().at("Sphere_A::Section_Body").visible);
    EXPECT_TRUE(world.GetMeshObjects().at("Sphere_A::Section_Glass").visible);
    actors = BuildEditorSceneActors(world);
    matches = FindActorSnapshot(actors, identity);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_TRUE(matches.front()->visible);
    const WorldSnapshot shown = BuildVisibilityTestSnapshot(world);
    EXPECT_EQ(CountMeshDrawsBySceneObjectIdentity(shown, identity), 3u);
}

TEST_F(EditorUiTest, MeshActorIdentityResolutionMatchesTheWritePathForSingleAndMultiSection)
{
    World world = MakeMultiSectionVisibilityTestWorld();

    // 单 section actor 的身份 == 对象名：行为与修复前完全一致。
    const WorldMeshObject& single = world.GetMeshObjects().at("Sphere_A");
    EXPECT_EQ(MeshSceneObjectIdentity(single, "Sphere_A"), "Sphere_A");

    // 多 section actor：身份来自 sceneObjectIdentity，不等于任何 section 键。
    const WorldMeshObject& multi = world.GetMeshObjects().at("SM_car_wheel_bl::M_Wheel_Hub");
    EXPECT_EQ(MeshSceneObjectIdentity(multi, "SM_car_wheel_bl::M_Wheel_Hub"), "SM_car_wheel_bl");
    // 空身份的兜底（跨旧数据/直接构造的 WorldMeshObject）也必须两边一致。
    WorldMeshObject unnamed;
    EXPECT_EQ(MeshSceneObjectIdentity(unnamed, "Sphere_A"), "Sphere_A");

    // 真实 actor builder 发布的 mesh 稳定身份必须是写入端认得的那一个字符串。
    const std::vector<EditorSceneActorSnapshot> actors = BuildEditorSceneActors(world);
    std::vector<const EditorSceneActorSnapshot*> matches =
        FindActorSnapshot(actors, MeshSceneObjectIdentity(multi, "SM_car_wheel_bl::M_Wheel_Hub"));
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front()->sceneObjectIdentity, "SM_car_wheel_bl");
    EditorSceneActorIdentity parsed;
    ASSERT_TRUE(ParseSceneActorIdentity(matches.front()->identity, parsed));
    EXPECT_EQ(parsed.type, EditorSceneActorType::Mesh);
    EXPECT_EQ(parsed.name, matches.front()->sceneObjectIdentity);
    EXPECT_TRUE(world.SetMeshSceneObjectVisible(parsed.name, false));
    EXPECT_FALSE(world.IsMeshSceneObjectVisible("SM_car_wheel_bl"));
}

TEST_F(EditorUiTest, MissingMeshIdentityIsStillRejectedInAMultiSectionWorld)
{
    World world = MakeMultiSectionVisibilityTestWorld();

    // 多 section actor 必须不再被拒绝……
    EXPECT_TRUE(world.SetMeshSceneObjectVisible("SM_car_wheel_bl", false));
    // ……但身份确实不在当前 World 时仍然拒绝（旧场景、已改名、伪造身份），
    // 并且 section 键永远不是 actor 身份：缺陷期的失败写法下发到这里必须仍然返回 false，
    // 不能被当成"改到了这个 actor"。
    EXPECT_FALSE(world.SetMeshSceneObjectVisible("SM_car_wheel_bl::M_Wheel_Hub", false));
    EXPECT_FALSE(world.SetMeshSceneObjectVisible("SM_car_wheel_bl::M_Wheel_Missing", false));
    EXPECT_FALSE(world.SetMeshSceneObjectVisible("Wheel_From_Another_Scene", false));
    // 被拒绝的写入不得产生任何副作用：只有显式下发的那一个 actor 被改了。
    EXPECT_FALSE(world.IsMeshSceneObjectVisible("SM_car_wheel_bl"));
    EXPECT_TRUE(world.GetMeshObjects().at("Sphere_A").visible);
}

TEST_F(EditorUiTest, OutlinerRowRightClickOpensAContextMenuForThatActor)
{
    AddActor(SunActor(3.0f, true));
    SettleOutliner();
    ASSERT_FALSE(panel->IsContextMenuOpen());

    auto* row = OutlinerRowElement("Sun_Light");
    ASSERT_NE(row, nullptr);
    const Rml::Vector2f point =
        row->GetAbsoluteOffset(Rml::BoxArea::Border) + Rml::Vector2f(4.0f, 4.0f);
    RightClickAt(point);

    EXPECT_TRUE(panel->IsContextMenuOpen());
    EXPECT_TRUE(ContextMenu()->IsVisible());
    // 标题行给出目标名：菜单指向的对象与被右键的那一行是同一个。
    EXPECT_NE(ContextMenuTitle().find("Sun_Light"), std::string::npos);
    EXPECT_NE(ContextMenuItem("context-outliner-visible"), nullptr);
    // 位置由每次调用的右键坐标决定，不是固定位置也不是上一次的位置。
    const auto offset = ContextMenu()->GetAbsoluteOffset(Rml::BoxArea::Border);
    EXPECT_NEAR(offset.x, point.x, 1.5f);
    EXPECT_NEAR(offset.y, point.y, 1.5f);
}

// 菜单目标必须是"被右键的那一行"，不是面板的当前选择：右键 B 而选中 A 时，
// 可见性开关只能改 B。这条断言就是目标解析的判据。
TEST_F(EditorUiTest, ContextMenuVisibilityToggleTargetsTheRightClickedActorNotTheSelection)
{
    EditorSceneActorSnapshot mesh = MeshActor("Body_A", 1);
    EditorSceneActorSnapshot light = PointLightActor("Key_Point_Light", 4.0f, 12.0f);
    const std::string meshIdentity = mesh.identity;
    const std::string lightIdentity = light.identity;
    AddActor(std::move(mesh));
    AddActor(std::move(light));
    snapshot.selectedActorIdentity = meshIdentity;
    SettleOutliner();
    ASSERT_NE(meshIdentity, lightIdentity);

    RightClick(OutlinerRowElement("Key_Point_Light"));
    ASSERT_TRUE(panel->IsContextMenuOpen());
    EXPECT_EQ(panel->ContextMenuTarget(), lightIdentity);
    ClickContextItem("context-outliner-visible");

    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.back().type, UiActionType::SetActorVisible);
    EXPECT_EQ(runtimeSink.actions.back().stringValue, lightIdentity);
    EXPECT_EQ(runtimeSink.actions.back().intValue, 0);
}

TEST_F(EditorUiTest, ContextMenuVisibilityItemFollowsAndInvertsTheActorState)
{
    EditorSceneActorSnapshot light = PointLightActor("Key_Point_Light", 4.0f, 12.0f);
    light.visible = false;
    const std::string identity = light.identity;
    AddActor(std::move(light));
    SettleOutliner();

    RightClick(OutlinerRowElement("Key_Point_Light"));
    ASSERT_TRUE(panel->IsContextMenuOpen());
    auto* item = ContextMenuItem("context-outliner-visible");
    ASSERT_NE(item, nullptr);
    // 条目文案来自冻结的可见性；命令发出的是它的反面，与行尾眼睛同一语义。
    EXPECT_NE(item->GetInnerRML().find("Show in Viewport"), std::string::npos);
    ClickContextItem("context-outliner-visible");

    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.back().stringValue, identity);
    EXPECT_EQ(runtimeSink.actions.back().intValue, 1);
}

// 相机没有渲染侧可见性状态，也没有别的逐对象动作：与其放一个点了没有反应的条目，
// 不如不提供菜单（与大纲行尾不显示眼睛是同一条规则）。
TEST_F(EditorUiTest, ContextMenuIsNotOfferedForAnActorWithoutAnActorScopedAction)
{
    AddActor(CameraActor("ShotCam_A"));
    SettleOutliner();

    RightClick(OutlinerRowElement("ShotCam_A"));
    EXPECT_FALSE(panel->IsContextMenuOpen());
    EXPECT_FALSE(ContextMenu()->IsVisible());
    EXPECT_TRUE(ContextMenuItems().empty());
    EXPECT_TRUE(runtimeSink.actions.empty());
    EXPECT_TRUE(sink.commands.empty());
}

TEST_F(EditorUiTest, ContextMenuClosesAfterAnItemIsChosen)
{
    AddActor(SunActor(3.0f, true));
    SettleOutliner();
    RightClick(OutlinerRowElement("Sun_Light"));
    ASSERT_TRUE(panel->IsContextMenuOpen());
    ASSERT_TRUE(ContextMenu()->IsVisible());

    ClickContextItem("context-outliner-visible");

    EXPECT_FALSE(panel->IsContextMenuOpen());
    EXPECT_FALSE(ContextMenu()->IsVisible());
    EXPECT_EQ(runtimeSink.actions.size(), 1u);
}

TEST_F(EditorUiTest, ContextMenuClosesWhenClickingOutsideIt)
{
    AddActor(SunActor(3.0f, true));
    SettleOutliner();
    RightClick(OutlinerRowElement("Sun_Light"));
    ASSERT_TRUE(panel->IsContextMenuOpen());

    // 菜单栏标题在工具条里，既不在弹层内部也不是任何菜单条目。
    Click("#menu-edit-title");
    context->Update();

    EXPECT_FALSE(panel->IsContextMenuOpen());
    EXPECT_FALSE(ContextMenu()->IsVisible());
}

// mesh 的材质槽条目走与点击槽行完全相同的选择路由：同一个 sink、同一份槽身份。
TEST_F(EditorUiTest, ContextMenuMaterialItemUsesTheExistingSlotSelectionRoute)
{
    AddActor(MeshActor("SM_A", 1));
    SettleOutliner();
    RightClick(OutlinerRowElement("SM_A"));
    ASSERT_TRUE(panel->IsContextMenuOpen());

    ClickContextItem("context-actor-material");

    ASSERT_TRUE(selectionSink.selectedMaterial.has_value());
    EXPECT_EQ(selectionSink.selectedMaterial->materialInstancePath,
        "Maps/SC_test/Materials/MI_test.json");
    EXPECT_EQ(selectionSink.selectedMaterial->materialSlotName, "Surface");
    EXPECT_TRUE(selectionSink.selectedActorIdentity.empty());
}

TEST_F(EditorUiTest, BrowserAssetContextMenuOpensTheAssetThroughTheExistingOpenCommand)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->revision = 2;
    catalog->assets = {
        {"Maps/SC_test/Materials/MI_test.json", EditorAssetKind::MaterialInstance, "Test Material"},
        {"Meshes/SM_test.json", EditorAssetKind::Mesh, "Test Mesh"}};
    snapshot.assetCatalog = catalog;
    Refresh();

    auto* tile = BrowserItem("Maps/SC_test/Materials/MI_test.json");
    ASSERT_NE(tile, nullptr);
    RightClick(tile);
    ASSERT_TRUE(panel->IsContextMenuOpen());
    EXPECT_NE(ContextMenuTitle().find("Test Material"), std::string::npos);
    ASSERT_NE(ContextMenuItem("context-browser-open"), nullptr);

    ClickContextItem("context-browser-open");

    ASSERT_EQ(sink.commands.size(), 1u);
    EXPECT_EQ(sink.commands.back().type, EditorCommandType::OpenMaterialInstanceAsset);
    EXPECT_FALSE(panel->IsContextMenuOpen());
}

// 目录节点在只读目录里没有条目，也没有真实可执行的目录级动作：不提供菜单，
// 而不是拿"打开目录"这类与双击重复的条目把菜单填满。
TEST_F(EditorUiTest, BrowserFolderRightClickDoesNotOpenAContextMenu)
{
    auto catalog = std::make_shared<EditorJsonAssetCatalogSnapshot>();
    catalog->assets = {
        {"Maps/SC_alpha/MI_a.json", EditorAssetKind::MaterialInstance, "A"}};
    snapshot.assetCatalog = catalog;
    Refresh();
    // 只在非递归视图里才出现目录条目（递归视图只列资产）。
    Click("#browser-recursive");
    Refresh();

    auto* folder = BrowserItem("Maps");
    ASSERT_NE(folder, nullptr);
    RightClick(folder);
    EXPECT_FALSE(panel->IsContextMenuOpen());
    EXPECT_TRUE(ContextMenuItems().empty());
    EXPECT_TRUE(sink.commands.empty());
}

// 视口没有拾取后端，因此菜单只放视口级动作：Lit / Unlit 与 CSM 设置持久化，
// 不含任何需要"鼠标下是哪个对象"的条目。
TEST_F(EditorUiTest, ViewportContextMenuOffersOnlyViewportWideCommands)
{
    Click("#workspace-viewport-tab");
    Refresh();
    auto* image = document->GetElementById("editor-viewport-image");
    ASSERT_NE(image, nullptr);
    const auto box = image->GetBox().GetSize(Rml::BoxArea::Border);
    ASSERT_GT(box.x, 0.0f);
    ASSERT_GT(box.y, 0.0f);

    RightClickAt(image->GetAbsoluteOffset(Rml::BoxArea::Border) + box * 0.5f);

    ASSERT_TRUE(panel->IsContextMenuOpen());
    EXPECT_EQ(ContextMenuTitle(), "Viewport");
    EXPECT_EQ(ContextMenuItems().size(), 3u);
    EXPECT_NE(ContextViewModeItem(0), nullptr);
    EXPECT_NE(ContextViewModeItem(1), nullptr);
    EXPECT_NE(ContextMenuItem("context-save-csm"), nullptr);
}

TEST_F(EditorUiTest, ViewportContextMenuViewModeChecksTheRuntimeModeAndReusesTheDebugViewCommand)
{
    UiViewModelSnapshot view;
    view.debugViewMode = 1;
    panel->UpdateRuntimeView(view);
    Click("#workspace-viewport-tab");
    Refresh();
    auto* image = document->GetElementById("editor-viewport-image");
    ASSERT_NE(image, nullptr);
    const auto box = image->GetBox().GetSize(Rml::BoxArea::Border);
    ASSERT_GT(box.y, 0.0f);
    RightClickAt(image->GetAbsoluteOffset(Rml::BoxArea::Border) + box * 0.5f);

    ASSERT_TRUE(panel->IsContextMenuOpen());
    // 勾选来自冻结的运行时快照，不是面板的第二份状态。
    auto* lit = ContextViewModeItem(0);
    auto* unlit = ContextViewModeItem(1);
    ASSERT_NE(lit, nullptr);
    ASSERT_NE(unlit, nullptr);
    EXPECT_TRUE(lit->QuerySelector(".menu-check")->GetInnerRML().empty());
    EXPECT_FALSE(unlit->QuerySelector(".menu-check")->GetInnerRML().empty());

    lit->DispatchEvent("click", {});
    context->Update();

    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.back().type, UiActionType::SetDebugViewMode);
    EXPECT_EQ(runtimeSink.actions.back().intValue, 0);
}

// CSM 设置要写进活动场景文件：没有活动 World 时条目明确禁用，点了不产生命令，
// 而不是提交一条注定被拒绝的运行时命令（与 Edit 的 Undo/Redo 同一表现规则）。
TEST_F(EditorUiTest, ViewportContextMenuDisablesCsmSaveWithoutAnActiveWorld)
{
    Click("#workspace-viewport-tab");
    Refresh();
    UiViewModelSnapshot view;
    panel->UpdateRuntimeView(view);
    auto* image = document->GetElementById("editor-viewport-image");
    ASSERT_NE(image, nullptr);
    const auto box = image->GetBox().GetSize(Rml::BoxArea::Border);
    ASSERT_GT(box.y, 0.0f);
    RightClickAt(image->GetAbsoluteOffset(Rml::BoxArea::Border) + box * 0.5f);

    auto* save = ContextMenuItem("context-save-csm");
    ASSERT_NE(save, nullptr);
    EXPECT_TRUE(save->HasAttribute("disabled"));
    ClickContextItem("context-save-csm");
    EXPECT_TRUE(runtimeSink.actions.empty());
}

TEST_F(EditorUiTest, ViewportContextMenuSavesCsmSettingsThroughTheExistingRuntimeCommand)
{
    UiViewModelSnapshot view;
    view.activeWorldPath = "Maps/SC_test/SC_test.json";
    panel->UpdateRuntimeView(view);
    Click("#workspace-viewport-tab");
    Refresh();
    auto* image = document->GetElementById("editor-viewport-image");
    ASSERT_NE(image, nullptr);
    const auto box = image->GetBox().GetSize(Rml::BoxArea::Border);
    ASSERT_GT(box.y, 0.0f);
    RightClickAt(image->GetAbsoluteOffset(Rml::BoxArea::Border) + box * 0.5f);

    auto* save = ContextMenuItem("context-save-csm");
    ASSERT_NE(save, nullptr);
    EXPECT_FALSE(save->HasAttribute("disabled"));
    ClickContextItem("context-save-csm");

    ASSERT_EQ(runtimeSink.actions.size(), 1u);
    EXPECT_EQ(runtimeSink.actions.back().type, UiActionType::SaveCsmSettingsToScene);
}

} // namespace

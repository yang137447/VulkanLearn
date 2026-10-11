#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/layout/layoutWidgets.h"
#include "ui/widgets/navigation/tabWidgets.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"
#include "ui/widgets/widgetDocument.h"
#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "ui/uiColorTransform.h"

namespace
{
using namespace VL::UiWidgets;

class WidgetTestSystem : public Rml::SystemInterface
{
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& text) override
    {
        if (type <= Rml::Log::LT_WARNING) errors += text + "\n";
        return true;
    }
    std::string errors;
};

class WidgetTestRenderer : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override
    {
        if (source == VL::SceneViewportTextureSource)
        {
            dimensions = {1, 1};
            return 1;
        }
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
};

TEST(UiColorTransform, OpaqueDarkCssColorIsLinearizedWithoutByteQuantization)
{
    const auto color = VL::UiPremultipliedSrgbToLinear(40, 40, 40, 255);
    EXPECT_NEAR(color[0], 0.02121901f, 0.0000001f);
    EXPECT_FLOAT_EQ(color[0], color[1]);
    EXPECT_FLOAT_EQ(color[3], 1.0f);
}

TEST(UiColorTransform, UnassociatesBeforeGammaConversionAndRepremultiplies)
{
    const auto color = VL::UiPremultipliedSrgbToLinear(128, 64, 0, 128);
    EXPECT_FLOAT_EQ(color[0], 128.0f / 255.0f);
    EXPECT_NEAR(color[1], 0.21404114f * 128.0f / 255.0f, 0.0000001f);
    EXPECT_FLOAT_EQ(color[2], 0.0f);
    EXPECT_FLOAT_EQ(color[3], 128.0f / 255.0f);
}

TEST(UiColorTransform, TransparentVertexCannotContributeColor)
{
    EXPECT_EQ(VL::UiPremultipliedSrgbToLinear(0, 0, 0, 0), (std::array<float, 4>{}));
}

TEST(UiColorTransform, SrgbLinearBranchKeepsStandardBreakpoint)
{
    EXPECT_FLOAT_EQ(VL::UiSrgbToLinear(0.0f), 0.0f);
    EXPECT_NEAR(VL::UiSrgbToLinear(0.04045f), 0.003130805f, 0.0000001f);
    EXPECT_FLOAT_EQ(VL::UiSrgbToLinear(1.0f), 1.0f);
}

TEST(UiColorTransform, LoadedStraightRgbaIsLinearizedAndPremultipliedOnce)
{
    std::vector<uint8_t> pixels{128, 128, 128, 128};
    VL::ConvertUiPixelsToLinear(pixels, false);
    EXPECT_EQ(pixels, (std::vector<uint8_t>{28, 28, 28, 128}));
}

TEST(UiColorTransform, GeneratedGlyphCoverageIsNotPremultipliedTwice)
{
    std::vector<uint8_t> pixels{64, 64, 64, 64};
    VL::ConvertUiPixelsToLinear(pixels, true);
    EXPECT_EQ(pixels, (std::vector<uint8_t>{64, 64, 64, 64}));
}

TEST(UiColorTransform, GeneratedColoredPixelsAreUnassociatedBeforeConversion)
{
    std::vector<uint8_t> pixels{64, 64, 64, 128};
    VL::ConvertUiPixelsToLinear(pixels, true);
    EXPECT_EQ(pixels, (std::vector<uint8_t>{27, 27, 27, 128}));
}

TEST(UiColorTransform, LinearToSrgbMatchesTheEditorPaletteCurve)
{
    // 编辑器面向 sRGB 显示线性存储的颜色：两端必须精确落在 0 和 1（8bit 读数才是全黑/全白）。
    EXPECT_FLOAT_EQ(VL::UiLinearToSrgb(0.0f), 0.0f);
    EXPECT_NEAR(VL::UiLinearToSrgb(1.0f), 1.0f, 0.000001f);
    EXPECT_NEAR(VL::UiLinearToSrgb(0.0031308f), 0.0404499f, 0.000001f);
    // 标准中灰：线性 0.214041（sRGB 0.5）与逐字节读数 128 对应。
    EXPECT_NEAR(VL::UiLinearToSrgb(0.21404114f), 0.5f, 0.0002f);
    EXPECT_EQ(VL::UiSrgbHexByte(VL::UiLinearToSrgb(0.21404114f)), 128u);
}

TEST(UiColorTransform, LinearToSrgbKeepsHdrValuesInsteadOfClampingThem)
{
    // 扩展公式：emissive 之类的线性值越过 1.0 时必须原样保留，截断只发生在读数里。
    EXPECT_NEAR(VL::UiLinearToSrgb(4.0f), 1.8248f, 0.001f);
    EXPECT_GT(VL::UiLinearToSrgb(4.0f), 1.0f);
    EXPECT_EQ(VL::UiSrgbHexByte(VL::UiLinearToSrgb(4.0f)), 255u);
    EXPECT_EQ(VL::UiSrgbHexByte(-1.0f), 0u);
    EXPECT_EQ(VL::UiSrgbHexByte(1.0f), 255u);
    EXPECT_EQ(VL::UiSrgbHexByte(0.0f), 0u);
}

TEST(UiColorTransform, LinearSrgbRoundTripIsStableForDisplayableValues)
{
    for (const float linear : {0.0f, 0.0031308f, 0.05f, 0.21404114f, 0.5f, 1.0f})
    {
        EXPECT_NEAR(VL::UiSrgbToLinear(VL::UiLinearToSrgb(linear)), linear, 0.00001f) << linear;
    }
}

TEST(UiColorTransform, TransparentImagePixelsCannotLeakColor)
{
    std::vector<uint8_t> pixels{200, 100, 50, 0, 255, 255, 255, 255};
    VL::ConvertUiPixelsToLinear(pixels, false);
    EXPECT_EQ(pixels, (std::vector<uint8_t>{0, 0, 0, 0, 255, 255, 255, 255}));
}

class UiWidgetTest : public testing::Test
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
        context = Rml::CreateContext("widget-test", {1280, 720});
        ASSERT_NE(context, nullptr);
        document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
        ASSERT_NE(document, nullptr);
        panel = std::make_unique<VL::EditorUi::MaterialInstanceAssetEditorPanel>();
        panel->SetWorkspaceLayoutPath({});
        std::string error;
        ASSERT_TRUE(panel->Attach(*document, error)) << error;
        view.Attach(*document);
        document->Show();
        ASSERT_TRUE(system.errors.empty()) << system.errors;
    }
    void TearDown() override
    {
        view.Detach();
        panel.reset();
        if (context != nullptr) Rml::RemoveContext(context->GetName());
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }
    void Show(const std::string& markup)
    {
        view.SetMarkup("details-content", markup);
        context->Update();
        ASSERT_TRUE(system.errors.empty()) << system.errors;
    }
    WidgetTestSystem system;
    WidgetTestRenderer renderer;
    WidgetDocument view;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    std::unique_ptr<VL::EditorUi::MaterialInstanceAssetEditorPanel> panel;
};

TEST_F(UiWidgetTest, LabelsCannotInjectElementsOrAttributes)
{
    Show(Button("quoted\"id", {"action\"name", 3, 2}, "<injected/>") +
        Input("input", {"change"}, "\"/><injected/>"));
    EXPECT_EQ(document->QuerySelector("injected"), nullptr);
    ASSERT_NE(document->GetElementById("quoted\"id"), nullptr);
    EXPECT_EQ(document->GetElementById("quoted\"id")->GetAttribute<std::string>("data-action", ""), "action\"name");
}

TEST_F(UiWidgetTest, SelectReplacementRemovesPreviousInternalOptions)
{
    Show(Select("choice", {"choice"}, Option("first", "First")));
    auto* selector = dynamic_cast<Rml::ElementFormControlSelect*>(document->GetElementById("choice"));
    ASSERT_NE(selector, nullptr);
    ASSERT_EQ(selector->GetNumOptions(), 1);
    view.SetMarkup("choice", Option("second", "Second"));
    view.SetControlValue("choice", "second");
    EXPECT_EQ(selector->GetNumOptions(), 1);
    EXPECT_EQ(selector->GetValue(), "second");
}

TEST_F(UiWidgetTest, FocusedInputKeepsUnfinishedText)
{
    Show(Input("number", {"number"}, "1"));
    auto* input = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("number"));
    ASSERT_NE(input, nullptr);
    ASSERT_TRUE(input->Focus());
    input->SetValue("-");
    view.SetControlValue("number", "9");
    EXPECT_EQ(input->GetValue(), "-");
}

TEST_F(UiWidgetTest, DisabledGenericButtonReceivesStyleAndEventState)
{
    Show(Button("command", {"command"}, "Command"));
    view.SetDisabled("command", true);
    auto* button = document->GetElementById("command");
    ASSERT_NE(button, nullptr);
    EXPECT_TRUE(button->HasAttribute("disabled"));
    EXPECT_TRUE(button->IsClassSet("disabled"));
    view.SetDisabled("command", false);
    EXPECT_FALSE(button->HasAttribute("disabled"));
    EXPECT_FALSE(button->IsClassSet("disabled"));
}

TEST_F(UiWidgetTest, UnchangedMarkupPreservesChildIdentity)
{
    const auto markup = Input("number", {"change"}, "1");
    Show(markup);
    auto* input = document->GetElementById("number");
    view.SetMarkup("details-content", markup);
    EXPECT_EQ(input, document->GetElementById("number"));
}

TEST_F(UiWidgetTest, RepeatedPropertyValueDoesNotInvalidateTheDocumentAgain)
{
    Show(Input("number", {"change"}, "1"));
    view.SetProperty("number", "width", "100px");
    ASSERT_EQ(view.PropertyWriteCount(), 1u);
    for (int frame = 0; frame < 100; ++frame)
        view.SetProperty("number", "width", "100px");
    EXPECT_EQ(view.PropertyWriteCount(), 1u);
    view.SetProperty("number", "width", "120px");
    EXPECT_EQ(view.PropertyWriteCount(), 2u);
}

TEST_F(UiWidgetTest, ReplacedSubtreeCannotReusePreviousNodeProperties)
{
    Show(Input("number", {"change"}, "1"));
    view.SetProperty("number", "width", "100px");
    Show(Input("number", {"change"}, "2"));
    view.SetProperty("number", "width", "100px");
    EXPECT_EQ(view.PropertyWriteCount(), 2u);
}

TEST_F(UiWidgetTest, ReattachInvalidatesThePreviousDocumentCache)
{
    Show(Input("number", {"change"}, "1"));
    view.Detach();
    document->GetElementById("details-content")->SetInnerRML("");
    view.Attach(*document);
    Show(Input("number", {"change"}, "1"));
    EXPECT_NE(document->GetElementById("number"), nullptr);
}

TEST_F(UiWidgetTest, TextRefreshPreservesHostAndUnrelatedPropertyCaches)
{
    Show("<div id=\"metrics\">60 FPS</div>" + Input("number", {"change"}, "1"));
    view.SetProperty("metrics", "width", "100px");
    view.SetProperty("number", "width", "100px");
    for (int frame = 0; frame < 100; ++frame)
    {
        view.SetMarkup("metrics", std::to_string(frame) + " FPS");
        view.SetProperty("metrics", "width", "100px");
        view.SetProperty("number", "width", "100px");
    }
    EXPECT_EQ(view.PropertyWriteCount(), 2u);
}

TEST_F(UiWidgetTest, ReplacedSubtreeInvalidatesDescendantMarkupCache)
{
    Show("<div id=\"child\">First</div>");
    view.SetMarkup("child", "Cached");
    Show("<div id=\"child\">Replacement</div>");
    view.SetMarkup("child", "Cached");
    EXPECT_EQ(document->GetElementById("child")->GetInnerRML(), "Cached");
}

TEST_F(UiWidgetTest, HorizontalLayoutKeepsNumericComponentsWithinTheInspector)
{
    Show(PropertyGroup("Vector", NumericComponents({
        {"x", "X", "0.1", {"change", 0, 0}},
        {"y", "Y", "0.2", {"change", 0, 1}},
        {"z", "Z", "0.3", {"change", 0, 2}},
        {"w", "W", "1.0", {"change", 0, 3}}}), "reset", {"reset"}));
    for (const Rml::Vector2i dimensions : {Rml::Vector2i(1280, 720), Rml::Vector2i(960, 540)})
    {
        context->SetDimensions(dimensions);
        context->Update();
        auto* inspector = document->GetElementById("editor-inspector");
        const float right = inspector->GetAbsoluteOffset(Rml::BoxArea::Content).x +
            inspector->GetBox().GetSize(Rml::BoxArea::Content).x;
        float previousRight = 0.0f;
        for (const char* id : {"x", "y", "z", "w"})
        {
            auto* input = document->GetElementById(id);
            const float left = input->GetAbsoluteOffset(Rml::BoxArea::Border).x;
            const float width = input->GetBox().GetSize(Rml::BoxArea::Border).x;
            EXPECT_GT(width, 20.0f);
            EXPECT_GE(left, previousRight);
            EXPECT_LE(left + width, right + 1.0f);
            previousRight = left + width;
        }
    }
}

TEST_F(UiWidgetTest, NumericComponentRangeAddsASliderSharingTheSameAddress)
{
    Show(PropertyGroup("Vector", NumericComponents({
        {"x", "X", "0.4", {"parameter", 3, 0}, true, -2.0f, 6.0f},
        {"y", "Y", "0.5", {"parameter", 3, 1}}}), "reset", {"reset"}));
    auto* text = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("x"));
    auto* slider = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("x-slider"));
    ASSERT_NE(text, nullptr);
    ASSERT_NE(slider, nullptr);
    EXPECT_EQ(slider->GetAttribute<std::string>("type", ""), "range");
    EXPECT_FLOAT_EQ(slider->GetAttribute<float>("min", 0.0f), -2.0f);
    EXPECT_FLOAT_EQ(slider->GetAttribute<float>("max", 0.0f), 6.0f);
    // RmlUi 的范围控件默认 step=1，会把浮点滑条量化成整数，组件必须显式给出步长。
    EXPECT_GT(slider->GetAttribute<float>("step", 0.0f), 0.0f);
    EXPECT_LT(slider->GetAttribute<float>("step", 1.0f), 1.0f);
    // 拖动与输入必须落在同一条业务地址上：同一个 action/index/component。
    for (const char* attribute : {"data-action", "data-index", "data-component"})
    {
        EXPECT_EQ(slider->GetAttribute<std::string>(attribute, ""),
            text->GetAttribute<std::string>(attribute, "")) << attribute;
    }
    // 没有范围的分量只保留文本输入，精确输入始终可用。
    EXPECT_NE(document->GetElementById("y"), nullptr);
    EXPECT_EQ(document->GetElementById("y-slider"), nullptr);
}

TEST_F(UiWidgetTest, DegenerateSliderRangeOmitsTheStepAttribute)
{
    Show(NumericComponents({{"x", "X", "0.0", {"parameter", 0, 0}, true, 0.25f, 0.25f}}));
    auto* slider = document->GetElementById("x-slider");
    ASSERT_NE(slider, nullptr);
    // min==max 时区间宽度为 0，不能写出 step="0"（RmlUi 会忽略它并回落到整数步长）。
    EXPECT_FALSE(slider->HasAttribute("step"));
    EXPECT_FLOAT_EQ(slider->GetAttribute<float>("min", 0.0f), 0.25f);
}

TEST_F(UiWidgetTest, RangeControlKeepsStableValueOnRepeatedSnapshotPushes)
{
    Show(Slider("range-a", {"parameter", 0, 0}, "0.5", -1.0f, 1.0f));
    auto* slider = document->GetElementById("range-a");
    ASSERT_NE(slider, nullptr);
    // 范围控件的 GetValue() 是 "%f" 文本，快照推送 9 位有效数字；两者数值相等时不得回写属性，
    // 否则静止帧会反复改写同一个 value（属性类型会从控件自己的浮点退化成快照字符串）。
    view.SetControlValue("range-a", "0.500000001");
    ASSERT_NE(slider->GetAttribute("value"), nullptr);
    EXPECT_EQ(slider->GetAttribute("value")->GetType(), Rml::Variant::FLOAT);
    EXPECT_NEAR(slider->GetAttribute<float>("value", 0.0f), 0.5f, 0.002f);
    // 数值确实变化时仍然推送。
    view.SetControlValue("range-a", "-0.25");
    EXPECT_NEAR(slider->GetAttribute<float>("value", 0.0f), -0.25f, 0.002f);
}

TEST_F(UiWidgetTest, ColorPreviewCarriesSwatchAndSrgbHexReadout)
{
    const ColorReadout grey = MakeColorReadout({0.21404114f, 0.21404114f, 0.21404114f, 1.0f});
    EXPECT_EQ(grey.hex, "#808080FF");
    EXPECT_EQ(grey.rgb, "rgb(128,128,128)");
    // 线性 0/1 与 alpha 边界：alpha 不做 gamma 变换。
    EXPECT_EQ(MakeColorReadout({0.0f, 0.0f, 0.0f, 0.0f}).hex, "#00000000");
    EXPECT_EQ(MakeColorReadout({1.0f, 1.0f, 1.0f, 1.0f}).hex, "#FFFFFFFF");
    EXPECT_EQ(MakeColorReadout({0.0f, 0.0f, 0.0f, 0.5f}).hex, "#00000080");

    Show(ColorPreview("swatch", "hex", grey));
    ASSERT_NE(document->GetElementById("swatch"), nullptr);
    EXPECT_EQ(document->GetElementById("hex")->GetInnerRML(), "#808080FF");
    // 色块底色不透明：当前 overlay 不合成半透明背景，alpha 只在读数里表达。
    EXPECT_EQ(document->QuerySelector(".color-swatch"), document->GetElementById("swatch"));
}

TEST_F(UiWidgetTest, CollapsibleSectionTogglesThroughItsClassOnly)
{
    const std::string markup = CollapsibleSection("details-section-parameters", "parameters",
        "Parameters", "<div id=\"body-child\">Value</div>", {"section"}, true);
    Show(markup);
    auto* section = document->GetElementById("details-section-parameters");
    auto* header = document->GetElementById("details-section-parameters-title");
    auto* child = document->GetElementById("body-child");
    ASSERT_NE(section, nullptr);
    ASSERT_NE(header, nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(header->GetAttribute<std::string>("data-action", ""), "section");
    EXPECT_EQ(header->GetAttribute<std::string>("data-section", ""), "parameters");
    EXPECT_FALSE(section->IsClassSet("collapsed"));

    view.SetClass("details-section-parameters", "collapsed", true);
    context->Update();
    EXPECT_TRUE(section->IsClassSet("collapsed"));
    // collapsed 只作用在分组根节点上，可见性必须连带祖先一起判断。
    EXPECT_FALSE(child->IsVisible(true));
    // 切换只改 class：子节点还是同一个 DOM 节点，焦点与滚动位置不会被重建丢掉。
    EXPECT_EQ(document->GetElementById("body-child"), child);
    EXPECT_TRUE(header->Focus());
    EXPECT_EQ(context->GetFocusElement(), header);
    view.SetClass("details-section-parameters", "collapsed", false);
    context->Update();
    EXPECT_TRUE(child->IsVisible(true));
    EXPECT_EQ(context->GetFocusElement(), header);
}

TEST_F(UiWidgetTest, TreeRowCarriesIndependentToggleAndSelectionAddresses)
{
    Show(TreeRow({"Parent", {"select", 4}, {"expand", 4}, 0, true, true, true}) +
        TreeRow({"Child", {"select-child", 4, 2}, {}, 1}));
    const auto* toggle = document->QuerySelector(".tree-toggle");
    ASSERT_NE(toggle, nullptr);
    EXPECT_EQ(toggle->GetAttribute<std::string>("data-action", ""), "expand");
    EXPECT_EQ(toggle->GetAttribute<std::string>("aria-expanded", ""), "true");
    const auto* child = document->QuerySelector("[data-action=select-child]");
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->GetAttribute<int>("data-index", -1), 4);
    EXPECT_EQ(child->GetAttribute<int>("data-component", -1), 2);
}

TEST_F(UiWidgetTest, DocumentTabSeparatesActivationFromClosing)
{
    Show(DocumentTab("Document", {"activate", 2}, {"close", 2}, true));
    ASSERT_NE(document->QuerySelector(".document-tab .selected"), nullptr);
    EXPECT_NE(document->QuerySelector(".tab-close[data-action=close]"), nullptr);
}

TEST_F(UiWidgetTest, TreeRowIconAndRowClassStayInsideTheSelectionButton)
{
    TreeRowData group;
    group.label = "Meshes";
    group.select = {"group", 0};
    group.toggle = {"group", 0};
    group.hasChildren = true;
    group.expanded = true;
    group.rowClass = "group-row";
    TreeRowData actor;
    actor.label = "Sphere_01";
    actor.select = {"actor", 3};
    actor.toggle = {"expand", 3};
    actor.hasChildren = true;
    actor.icon = "icons/mesh.png";
    actor.rowClass = "actor-row";
    Show(TreeRow(group) + TreeRow(actor));

    auto* row = document->QuerySelector(".tree-row.group-row");
    ASSERT_NE(row, nullptr);
    auto* actorRow = document->QuerySelector(".tree-row.actor-row");
    ASSERT_NE(actorRow, nullptr);
    // 图标是按钮内容的一部分：点击靶面仍是一个 list-item，而不是多出一个控件。
    auto* icon = actorRow->QuerySelector("img.row-icon");
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->GetAttribute<std::string>("src", ""), "icons/mesh.png");
    auto* button = actorRow->QuerySelector("[data-action=actor]");
    ASSERT_NE(button, nullptr);
    EXPECT_NE(button->QuerySelector("img.row-icon"), nullptr);
    EXPECT_EQ(button->QuerySelector("span")->GetInnerRML(), "Sphere_01");
    EXPECT_EQ(actorRow->QuerySelector("[data-action=expand]")->GetAttribute<int>("data-index", -1), 3);
    // 没有图标的行不生成 img。
    EXPECT_EQ(row->QuerySelector("img.row-icon"), nullptr);
}

TEST_F(UiWidgetTest, TreeRowTrailingControlSitsOutsideTheSelectionButton)
{
    TreeRowData actor;
    actor.label = "Sphere_01";
    actor.select = {"actor", 3};
    actor.toggle = {"expand", 3};
    actor.hasChildren = true;
    actor.icon = "icons/mesh.png";
    actor.rowClass = "actor-row";
    actor.hiddenRow = true;
    actor.hasTrailing = true;
    actor.trailing = {"visible", 3};
    actor.trailingIcon = "icons/eye-off.png";
    actor.trailingLabel = "Show in viewport";
    actor.trailingAlternate = true;
    // 没有行尾控件的行（分组行）不产生第二个按钮。
    TreeRowData plain;
    plain.label = "Meshes";
    plain.select = {"group", 0};
    plain.rowClass = "group-row";
    Show(TreeRow(plain) + TreeRow(actor));

    auto* row = document->QuerySelector(".tree-row.actor-row");
    ASSERT_NE(row, nullptr);
    // hiddenRow 是固定 class，不是宿主可控文本。
    EXPECT_TRUE(row->IsClassSet("hidden-row"));
    EXPECT_FALSE(document->QuerySelector(".tree-row.group-row")->IsClassSet("hidden-row"));

    auto* trailing = row->QuerySelector(".tree-trailing");
    ASSERT_NE(trailing, nullptr);
    // 行尾控件是独立的动作地址：点击它不会走上行选中动作。
    EXPECT_EQ(trailing->GetAttribute<std::string>("data-action", ""), "visible");
    EXPECT_EQ(trailing->GetAttribute<int>("data-index", -1), 3);
    EXPECT_EQ(trailing->GetAttribute<std::string>("aria-label", ""), "Show in viewport");
    EXPECT_TRUE(trailing->IsClassSet("alternate"));
    auto* icon = trailing->QuerySelector("img");
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->GetAttribute<std::string>("src", ""), "icons/eye-off.png");
    // 选择按钮里仍然只有行首类型图标一个 img，行尾控件不是它的子节点。
    auto* select = row->QuerySelector("[data-action=actor]");
    ASSERT_NE(select, nullptr);
    EXPECT_EQ(select->QuerySelector(".tree-trailing"), nullptr);
    EXPECT_EQ(select->QuerySelector("img.row-icon")->GetAttribute<std::string>("src", ""),
        "icons/mesh.png");
    EXPECT_EQ(document->QuerySelector(".tree-row.group-row .tree-trailing"), nullptr);
}

TEST_F(UiWidgetTest, TreeRowEscapesLabelAndKeepsIconPath)
{
    Show(TreeRow({"<b>x</b>", {"select", 0}, {}, 0, false, false, false, "icons/mesh.png"}));
    // 标签只作为文本进入行；图标路径写入 src 属性，不产生额外元素。
    EXPECT_EQ(document->QuerySelector("b"), nullptr);
    auto* icon = document->QuerySelector("img.row-icon");
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->GetAttribute<std::string>("src", ""), "icons/mesh.png");
}

TEST_F(UiWidgetTest, PropertyControlRowHasNoResetButton)
{
    Show(PropertyControlRow("<b>Intensity</b>", Input("value", {"change", 0}, "1")));
    auto* row = document->QuerySelector(".property-row");
    ASSERT_NE(row, nullptr);
    // 没有 Reset 语义就不生成 Reset 按钮，避免出现点了没有效果的控件。
    EXPECT_EQ(row->QuerySelector("button"), nullptr);
    EXPECT_EQ(row->QuerySelector("label")->GetInnerRML(), "&lt;b&gt;Intensity&lt;/b&gt;");
    EXPECT_NE(row->QuerySelector("[data-action=change]"), nullptr);
}

TEST_F(UiWidgetTest, PropertyInfoCanCarryAnAddressableValueSpan)
{
    Show(PropertyInfo("Position", "1, 2, 3", "actor-field-position") +
        PropertyInfo("Plain", "value"));
    const auto* value = document->GetElementById("actor-field-position");
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->GetInnerRML(), "1, 2, 3");
    // 不给 id 的行保持原有结构，不产生第二个可寻址节点。
    Rml::ElementList rows;
    document->QuerySelectorAll(rows, ".property-info");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[1]->QuerySelector("span")->GetAttribute<std::string>("id", ""), "");
}

TEST_F(UiWidgetTest, ScrollBoxAndVerticalLayoutComposeWithoutParserWarnings)
{
    Show(ScrollBox("scroll", VerticalBox(ListRow({"select", 1}, "Item one") +
        ListRow({"select", 2}, "Item two"))));
    ASSERT_NE(document->GetElementById("scroll"), nullptr);
    EXPECT_NE(document->QuerySelector("#scroll .vertical-box"), nullptr);
}

TEST(UiWidgetRefresh, NestedRefreshRestoresThePreviousSuppressionState)
{
    bool refreshing = false;
    {
        WidgetRefreshScope outer(refreshing);
        EXPECT_TRUE(refreshing);
        {
            WidgetRefreshScope inner(refreshing);
            EXPECT_TRUE(refreshing);
        }
        EXPECT_TRUE(refreshing);
    }
    EXPECT_FALSE(refreshing);
}

// 大纲窗口的纯数学：这里不需要 RmlUi 文档，与 Content Browser 的窗口化分开覆盖。
namespace SceneActorTreeTests
{
using namespace VL::EditorUi;

EditorSceneActorSnapshot TreeActor(EditorSceneActorType type, const std::string& name)
{
    EditorSceneActorSnapshot actor;
    actor.type = type;
    actor.worldGeneration = 1;
    actor.displayName = name;
    actor.identity = SceneActorIdentity(type, name);
    return actor;
}

// 带一个可打开材质槽的 mesh actor：槽行只由 section 的 MI 路径产生。
EditorSceneActorSnapshot TreeMeshActor(const std::string& name, const std::string& slotName)
{
    EditorSceneActorSnapshot actor = TreeActor(EditorSceneActorType::Mesh, name);
    EditorSceneMeshSectionSnapshot section;
    section.objectName = name;
    section.materialInstancePath = "Maps/SC_test/Materials/MI_test.json";
    section.materialSlotName = slotName;
    actor.sections.push_back(std::move(section));
    return actor;
}

TEST_F(UiWidgetTest, SceneActorWindowMountsABoundedSliceAndKeepsTheRemainingRange)
{
    const float rowHeight = 28.0f;
    const float clientHeight = 118.0f;
    const size_t rowCount = 130;
    const auto top = ComputeSceneActorWindow(rowCount, clientHeight, 0.0f, rowHeight);
    EXPECT_EQ(top.begin, 0u);
    EXPECT_GT(top.end, 0u);
    EXPECT_LE(top.end - top.begin, SceneActorMaxMountedRows);
    EXPECT_FLOAT_EQ(top.top, 0.0f);
    // 未挂载行的总高度必须留在 spacer 里，否则滚动范围会被窗口截短。
    EXPECT_FLOAT_EQ(top.bottom, static_cast<float>(rowCount - top.end) * rowHeight);

    // 窗口始终覆盖滚动偏移处的那一行：这是"滚动后挂载另一批行"的纯数学前提。
    for (const float fraction : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
    {
        const float scroll = fraction * static_cast<float>(rowCount) * rowHeight;
        const auto window = ComputeSceneActorWindow(rowCount, clientHeight, scroll, rowHeight);
        const size_t scrollRow = std::min(
            static_cast<size_t>(scroll / rowHeight), rowCount - 1);
        EXPECT_GE(scrollRow, window.begin);
        EXPECT_LT(scrollRow, window.end);
        EXPECT_FLOAT_EQ(window.top, static_cast<float>(window.begin) * rowHeight);
        EXPECT_FLOAT_EQ(window.bottom, static_cast<float>(rowCount - window.end) * rowHeight);
    }

    // 滚到底必须能挂到真正的最后一行，而不是停在第一个窗口。
    const auto bottom = ComputeSceneActorWindow(
        rowCount, clientHeight, static_cast<float>(rowCount) * rowHeight, rowHeight);
    EXPECT_EQ(bottom.end, rowCount);
    EXPECT_FLOAT_EQ(bottom.bottom, 0.0f);
}

TEST_F(UiWidgetTest, SceneActorWindowStaysWithinTheMountedRowBudget)
{
    for (const size_t rowCount : {size_t{1}, size_t{200}, size_t{5000}, size_t{50000}})
    {
        for (const float clientHeight : {1.0f, 118.0f, 20000.0f})
        {
            for (const float fraction : {0.0f, 0.5f, 1.0f})
            {
                const float rowHeight = 28.0f;
                const float scroll = fraction * static_cast<float>(rowCount) * rowHeight;
                const auto window = ComputeSceneActorWindow(
                    rowCount, clientHeight, scroll, rowHeight);
                EXPECT_LE(window.end - window.begin, SceneActorMaxMountedRows) << rowCount;
                EXPECT_LE(window.end, rowCount);
                EXPECT_GE(window.top, 0.0f);
                EXPECT_GE(window.bottom, 0.0f);
                // 挂载量与 spacer 高度之和仍是整棵树：窗口化不改变滚动范围。
                EXPECT_FLOAT_EQ(window.top + window.bottom +
                    static_cast<float>(window.end - window.begin) * rowHeight,
                    static_cast<float>(rowCount) * rowHeight);
            }
        }
    }
    // 退化输入不产生越界窗口：首帧没有实测行高时用 RCSS 兜底值。
    const auto empty = ComputeSceneActorWindow(0, 118.0f, 0.0f, 0.0f);
    EXPECT_EQ(empty.begin, 0u);
    EXPECT_EQ(empty.end, 0u);
    EXPECT_FLOAT_EQ(empty.rowHeight, SceneActorRowHeightFallback);
    EXPECT_FLOAT_EQ(empty.top, 0.0f);
    EXPECT_FLOAT_EQ(empty.bottom, 0.0f);
}

TEST_F(UiWidgetTest, CollapsedSceneActorGroupsContributeOnlyTheirHeader)
{
    std::vector<EditorSceneActorSnapshot> actors;
    for (int index = 0; index < 40; ++index)
        actors.push_back(TreeMeshActor("Mesh_" + std::to_string(index), "Surface"));
    actors.push_back(TreeActor(EditorSceneActorType::PointLight, "Key_Light"));

    EditorSceneActorTreeExpansion collapsed;
    collapsed.collapsedGroups.insert("mesh");
    const auto rows = BuildSceneActorRows(
        actors, "", "all", SceneActorSortOrder::NameAscending, collapsed);
    // 折叠组只留表头：子行不进入行集合，因此不占窗口槽位、也不产生 spacer 高度。
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_TRUE(rows[0].isGroup);
    EXPECT_EQ(rows[0].label, "Point Lights");
    EXPECT_TRUE(rows[0].expanded);
    EXPECT_EQ(rows[0].actorCount, 1u);
    EXPECT_EQ(rows[1].label, "Key_Light");
    EXPECT_EQ(rows[2].label, "Meshes");
    EXPECT_FALSE(rows[2].expanded);
    EXPECT_EQ(rows[2].actorCount, 40u);
    const auto window = ComputeSceneActorWindow(rows.size(), 118.0f, 0.0f, 28.0f);
    EXPECT_EQ(window.begin, 0u);
    EXPECT_EQ(window.end, rows.size());
    EXPECT_FLOAT_EQ(window.top, 0.0f);
    EXPECT_FLOAT_EQ(window.bottom, 0.0f);

    // 展开后同样的输入必须把 40 个子行放回行集合，并且重新需要窗口。
    const auto expanded = BuildSceneActorRows(actors, "", "all",
        SceneActorSortOrder::NameAscending, EditorSceneActorTreeExpansion{});
    EXPECT_EQ(expanded.size(), 43u);
    const auto scrolled = ComputeSceneActorWindow(expanded.size(), 118.0f, 0.0f, 28.0f);
    EXPECT_LT(scrolled.end - scrolled.begin, expanded.size());
    EXPECT_GT(scrolled.bottom, 0.0f);
}

TEST_F(UiWidgetTest, FullyCollapsedTreeOfEveryGroupFitsWithoutSpacers)
{
    std::vector<EditorSceneActorSnapshot> actors;
    EditorSceneActorTreeExpansion collapsed;
    for (const EditorSceneActorGroupSpec& spec : SceneActorGroupSpecs())
    {
        actors.push_back(TreeActor(spec.type, std::string(spec.title) + "_01"));
        collapsed.collapsedGroups.insert(spec.key);
    }
    const auto rows = BuildSceneActorRows(
        actors, "", "all", SceneActorSortOrder::NameAscending, collapsed);
    // 每个分组只剩表头：行数 == 分组数，子行不参与行集合。
    ASSERT_EQ(rows.size(), SceneActorGroupSpecs().size());
    for (const EditorSceneActorRow& row : rows)
    {
        EXPECT_TRUE(row.isGroup);
        EXPECT_FALSE(row.expanded);
        EXPECT_EQ(row.actorCount, 1u);
    }
    const auto window = ComputeSceneActorWindow(
        rows.size(), 118.0f, 0.0f, SceneActorRowHeightFallback);
    EXPECT_EQ(window.begin, 0u);
    EXPECT_EQ(window.end, rows.size());
    EXPECT_FLOAT_EQ(window.top, 0.0f);
    EXPECT_FLOAT_EQ(window.bottom, 0.0f);
}

TEST_F(UiWidgetTest, ExpandedMeshActorAddsSlotRowsInModelOrder)
{
    std::vector<EditorSceneActorSnapshot> actors;
    actors.push_back(TreeMeshActor("Sphere_01", "Surface"));
    actors.push_back(TreeMeshActor("Sphere_02", "Body"));
    EditorSceneActorTreeExpansion expansion;
    expansion.expandedActors.insert(actors[1].identity);
    const auto rows = BuildSceneActorRows(
        actors, "", "all", SceneActorSortOrder::NameAscending, expansion);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_TRUE(rows[0].isGroup);
    EXPECT_EQ(rows[0].actorCount, 2u);
    EXPECT_FALSE(rows[1].isSlot);
    EXPECT_TRUE(rows[1].hasChildren);       // 有可打开的槽，但没有展开：不产生槽行
    EXPECT_FALSE(rows[1].expanded);         // 展开状态按稳定身份记录，不跟着行下标走
    EXPECT_FALSE(rows[2].isSlot);
    EXPECT_TRUE(rows[2].hasChildren);
    EXPECT_TRUE(rows[2].expanded);
    EXPECT_EQ(rows[2].label, "Sphere_02");
    EXPECT_TRUE(rows[3].isSlot);
    EXPECT_EQ(rows[3].label, "Body");
    EXPECT_EQ(rows[3].actorIndex, 1u);
    EXPECT_EQ(rows[3].slotIndex, 0u);
}

TEST_F(UiWidgetTest, SceneActorSortReordersInsideGroupsAndLeavesGroupOrderFixed)
{
    std::vector<EditorSceneActorSnapshot> actors{
        TreeActor(EditorSceneActorType::Mesh, "Zeta"),
        TreeActor(EditorSceneActorType::Mesh, "alpha"),
        TreeActor(EditorSceneActorType::PointLight, "Key_Light"),
        TreeActor(EditorSceneActorType::PointLight, "A_Light"),
        TreeActor(EditorSceneActorType::Camera, "Camera_01")
    };
    const auto ascending = BuildSceneActorRows(actors, "", "all",
        SceneActorSortOrder::NameAscending, EditorSceneActorTreeExpansion{});
    const auto descending = BuildSceneActorRows(actors, "", "all",
        SceneActorSortOrder::NameDescending, EditorSceneActorTreeExpansion{});
    ASSERT_EQ(ascending.size(), descending.size());
    // 分组顺序由 SceneActorGroupSpecs() 固定：排序只翻转组内顺序，不重排分组。
    for (size_t index = 0; index < ascending.size(); ++index)
        EXPECT_EQ(ascending[index].isGroup, descending[index].isGroup) << index;
    std::vector<std::string> ascendingLabels;
    std::vector<std::string> descendingLabels;
    for (const auto& row : ascending) ascendingLabels.push_back(row.label);
    for (const auto& row : descending) descendingLabels.push_back(row.label);
    EXPECT_EQ(ascendingLabels, (std::vector<std::string>{"Point Lights", "A_Light", "Key_Light",
        "Cameras", "Camera_01", "Meshes", "alpha", "Zeta"}));
    EXPECT_EQ(descendingLabels, (std::vector<std::string>{"Point Lights", "Key_Light", "A_Light",
        "Cameras", "Camera_01", "Meshes", "Zeta", "alpha"}));

    // 排序键与 Content Browser 的 sort 控件同一套取值；未知值不被接受。
    SceneActorSortOrder parsed = SceneActorSortOrder::NameAscending;
    EXPECT_TRUE(ParseSceneActorSortOrder("name-desc", parsed));
    EXPECT_EQ(parsed, SceneActorSortOrder::NameDescending);
    EXPECT_FALSE(ParseSceneActorSortOrder("size", parsed));
    EXPECT_EQ(parsed, SceneActorSortOrder::NameDescending);
    EXPECT_STREQ(SceneActorSortOrderKey(SceneActorSortOrder::NameAscending), "name-asc");
    EXPECT_STREQ(SceneActorSortOrderKey(SceneActorSortOrder::NameDescending), "name-desc");
}

TEST_F(UiWidgetTest, SceneActorRowsSignatureFollowsRowFieldsOnly)
{
    std::vector<EditorSceneActorSnapshot> actors;
    actors.push_back(TreeMeshActor("Sphere_01", "Surface"));
    actors.push_back(TreeActor(EditorSceneActorType::DirectionalLight, "Sun_Light"));
    const uint64_t signature = SceneActorRowsSignature(actors);
    // 只影响 Details 的字段（灯光强度/相机位姿）不能让树重建。
    std::vector<EditorSceneActorSnapshot> numeric = actors;
    numeric[1].light.intensity = 7.5f;
    numeric[0].meshObjectId = 4242;
    EXPECT_EQ(SceneActorRowsSignature(numeric), signature);
    // 行集合/行文本字段必须改变签名。
    std::vector<EditorSceneActorSnapshot> renamed = actors;
    renamed[0].displayName = "Sphere_02";
    EXPECT_NE(SceneActorRowsSignature(renamed), signature);
    std::vector<EditorSceneActorSnapshot> retyped = actors;
    retyped[0].type = EditorSceneActorType::PointLight;
    EXPECT_NE(SceneActorRowsSignature(retyped), signature);
    std::vector<EditorSceneActorSnapshot> slotChanged = actors;
    slotChanged[0].sections[0].materialInstancePath.clear();
    EXPECT_NE(SceneActorRowsSignature(slotChanged), signature);
    std::vector<EditorSceneActorSnapshot> regenerated = actors;
    regenerated[0].worldGeneration = 2;
    regenerated[1].worldGeneration = 2;
    EXPECT_NE(SceneActorRowsSignature(regenerated), signature);
}

TEST_F(UiWidgetTest, SceneActorIdentityRoundTripsThroughItsParsedForm)
{
    // 身份是跨 GT→执行边界的唯一对象引用，因此必须与 SceneActorIdentity() 对称。
    for (const EditorSceneActorType type : {EditorSceneActorType::Mesh,
             EditorSceneActorType::DirectionalLight, EditorSceneActorType::PointLight,
             EditorSceneActorType::SpotLight, EditorSceneActorType::SquareAreaLight})
    {
        EditorSceneActorIdentity parsed;
        ASSERT_TRUE(ParseSceneActorIdentity(SceneActorIdentity(type, "Object_01"), parsed)) <<
            SceneActorGroupTitle(type);
        EXPECT_EQ(parsed.type, type);
        EXPECT_EQ(parsed.name, "Object_01");
    }

    // 对象名本身可以含分隔符：只按第一个分隔符切分，名字里的 '|' 必须原样保留。
    EditorSceneActorIdentity parsed;
    ASSERT_TRUE(ParseSceneActorIdentity("mesh|Weird|Name", parsed));
    EXPECT_EQ(parsed.type, EditorSceneActorType::Mesh);
    EXPECT_EQ(parsed.name, "Weird|Name");

    // 没有分隔符、未知分组键、空名字都必须在解析阶段被拒绝；
    // 空名字若放行，执行端只会给出误导性的"对象不在当前 World"。
    EXPECT_FALSE(ParseSceneActorIdentity("Sphere_01", parsed));
    EXPECT_FALSE(ParseSceneActorIdentity("noSuchGroup|Sphere_01", parsed));
    EXPECT_FALSE(ParseSceneActorIdentity("mesh|", parsed));
}

TEST_F(UiWidgetTest, OnlyRenderableActorTypesOfferAVisibilityToggle)
{
    EXPECT_TRUE(SceneActorHasVisibilityToggle(EditorSceneActorType::Mesh));
    EXPECT_TRUE(SceneActorHasVisibilityToggle(EditorSceneActorType::DirectionalLight));
    EXPECT_TRUE(SceneActorHasVisibilityToggle(EditorSceneActorType::PointLight));
    EXPECT_TRUE(SceneActorHasVisibilityToggle(EditorSceneActorType::SpotLight));
    EXPECT_TRUE(SceneActorHasVisibilityToggle(EditorSceneActorType::SquareAreaLight));
    // 相机/环境/风剖面没有渲染侧可见性状态：不摆一个点了没有效果的控件。
    EXPECT_FALSE(SceneActorHasVisibilityToggle(EditorSceneActorType::Camera));
    EXPECT_FALSE(SceneActorHasVisibilityToggle(EditorSceneActorType::Environment));
    EXPECT_FALSE(SceneActorHasVisibilityToggle(EditorSceneActorType::WindProfile));
}
} // namespace SceneActorTreeTests

} // namespace

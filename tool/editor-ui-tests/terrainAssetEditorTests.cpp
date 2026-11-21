#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <vector>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "editor/command/assetDocumentFields.h"
#include "editor/ui/terrainAssetEditorController.h"

namespace
{
using namespace VL;
using namespace VL::EditorUi;
using Json = nlohmann::json;

class TerrainEditorTestSystem : public Rml::SystemInterface
{
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& text) override
    {
        if (type <= Rml::Log::LT_WARNING) errors += text + "\n";
        return true;
    }
    std::string errors;
};

class TerrainEditorTestRenderer : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String&) override
    {
        dimensions = {1, 1};
        return 1;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

class CapturingSink : public IEditorCommandSink
{
public:
    void Submit(EditorCommandEnvelope command) override
    {
        commands.push_back(std::move(command));
    }
    std::vector<EditorCommandEnvelope> commands;
};

class CapturingMaterialEditSink : public ITerrainMaterialEditSink
{
public:
    struct Call
    {
        std::string materialInstancePath;
        std::string parameter;
        float value = 0.0f;
    };

    bool QueueMaterialFloat(
        const std::string& materialInstancePath,
        const std::string& parameter,
        float value) override
    {
        calls.push_back({materialInstancePath, parameter, value});
        return true;
    }

    std::vector<Call> calls;
};

class TerrainAssetEditorTest : public testing::Test
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
        context = Rml::CreateContext("terrain-asset-test", {1280, 720});
        ASSERT_NE(context, nullptr);
        document = context->LoadDocument(std::string(VL_EDITOR_ASSET_ROOT) + "/editor-workspace.rml");
        ASSERT_NE(document, nullptr);
        document->Show();
        controller.SetCommandSink(&sink);
        controller.SetMaterialEditSink(&materialSink);
        std::string error;
        ASSERT_TRUE(controller.Attach(*document, error)) << error;
        authored = Payload();
        controller.Build(&authored, false);
        context->Update();
        ASSERT_TRUE(system.errors.empty()) << system.errors;
    }
    void TearDown() override
    {
        controller.Detach();
        if (context != nullptr) Rml::RemoveContext(context->GetName());
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }
    EditorAssetDocumentResultPayload Payload() const
    {
        EditorAssetDocumentResultPayload payload;
        payload.assetPath = "Terrains/TR_test.json";
        payload.documentRevision = 4;
        payload.dirty = true;
        payload.heightmapPreviewPath = "Maps/HM.png";
        payload.document = {
            {"name", "TR_test"},
            {"type", "terrain"},
            {"compositedVT", {{"enabled", true}, {"packagePath", "Generated/Validation/TerrainEditor/Cooked/manifest.json"}}},
            {"heightmapPath", "Maps/HM_test.png"},
            {"materialInstancePath", "Maps/MI_test.json"},
            {"size", {1000.0, 800.0}},
            {"heightScale", 120.0},
            {"heightOffset", 5.0},
            {"gridResolution", 16},
            {"chunkQuads", 8},
            {"pageOrigin", {0, 0}},
            {"pageTilesPerSide", 8},
            {"layers", Json::array({
                {{"name", "Grass"}, {"heightRange", {0.0, 1.0}}, {"slopeRange", {0.0, 90.0}}},
                {{"name", "Rock"}, {"heightRange", {0.5, 1.0}}, {"slopeRange", {0.0, 90.0}}}
            })}};
        payload.capabilities = VL::Editor::DescribeAssetCapabilities(
            EditorAssetDocumentKind::Terrain, payload.document);
        return payload;
    }
    Rml::Element* Find(const std::string& id) { return document->GetElementById(id); }

    TerrainEditorTestSystem system;
    TerrainEditorTestRenderer renderer;
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    CapturingSink sink;
    CapturingMaterialEditSink materialSink;
    TerrainAssetEditorController controller;
    EditorAssetDocumentResultPayload authored;
};

TEST_F(TerrainAssetEditorTest, BuildRendersTopLevelFieldsAndLayerCards)
{
    ASSERT_NE(Find("terrain-field-heightScale"), nullptr);
    ASSERT_NE(Find("terrain-field-size"), nullptr);
    ASSERT_NE(Find("terrain-layer-0-noise"), nullptr);
    ASSERT_NE(Find("terrain-layer-1-noise"), nullptr);
    EXPECT_NE(Find("terrain-asset-editor")->QuerySelector("img"), nullptr);
    EXPECT_NE(Find("terrain-asset-editor")->GetInnerRML().find(
        "--terrain-vt-cook"), std::string::npos);
    EXPECT_EQ(dynamic_cast<Rml::ElementFormControl*>(
        Find("terrain-field-heightScale"))->GetValue(), "120.0");
}

TEST_F(TerrainAssetEditorTest, TopLevelChangeSubmitsOneRevisionGatedTerrainCommand)
{
    auto* input = dynamic_cast<Rml::ElementFormControl*>(Find("terrain-field-heightScale"));
    ASSERT_NE(input, nullptr);
    input->SetValue("250");
    ASSERT_TRUE(controller.HandleChange(*input, "terrain-asset-field"));
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& command = sink.commands[0];
    EXPECT_EQ(command.type, EditorCommandType::ModifyTerrainDocument);
    ASSERT_TRUE(command.expectedDocumentRevision.has_value());
    EXPECT_EQ(*command.expectedDocumentRevision, 4u);
    const auto& payload = std::get<ModifyAssetDocumentPayload>(command.payload);
    EXPECT_EQ(payload.assetPath, "Terrains/TR_test.json");
    ASSERT_EQ(payload.changes.size(), 1u);
    EXPECT_EQ(payload.changes[0].field, "heightScale");
    EXPECT_DOUBLE_EQ(std::get<double>(payload.changes[0].value), 250.0);
}

TEST_F(TerrainAssetEditorTest, LayerChangeCarriesLayerIndexAndExpectedName)
{
    auto* input = dynamic_cast<Rml::ElementFormControl*>(Find("terrain-layer-1-noise"));
    ASSERT_NE(input, nullptr);
    input->SetValue("0.35");
    ASSERT_TRUE(controller.HandleChange(*input, "terrain-asset-field"));
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& payload = std::get<ModifyAssetDocumentPayload>(sink.commands[0].payload);
    ASSERT_EQ(payload.changes.size(), 1u);
    ASSERT_TRUE(payload.changes[0].layerIndex.has_value());
    EXPECT_EQ(*payload.changes[0].layerIndex, 1u);
    ASSERT_TRUE(payload.changes[0].expectedLayerName.has_value());
    EXPECT_EQ(*payload.changes[0].expectedLayerName, "Rock");
    EXPECT_DOUBLE_EQ(std::get<double>(payload.changes[0].value), 0.35);
}

TEST_F(TerrainAssetEditorTest, AddAndRemoveLayerSubmitTheWholeTable)
{
    auto* add = Find("terrain-layer-add");
    ASSERT_NE(add, nullptr);
    ASSERT_TRUE(controller.HandleClick(*add, "terrain-layer-add"));
    ASSERT_EQ(sink.commands.size(), 1u);
    {
        const auto& payload = std::get<ModifyAssetDocumentPayload>(sink.commands[0].payload);
        ASSERT_EQ(payload.changes.size(), 1u);
        EXPECT_EQ(payload.changes[0].field, "layers");
        const auto layers = std::get<Json>(payload.changes[0].value);
        ASSERT_EQ(layers.size(), 3u);
        EXPECT_EQ(layers[2]["name"], "Layer_3");
    }
    sink.commands.clear();
    auto* remove = Find("terrain-layer-list")->QuerySelector("[data-action=terrain-layer-remove]");
    ASSERT_NE(remove, nullptr);
    ASSERT_TRUE(controller.HandleClick(*remove, "terrain-layer-remove"));
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& payload = std::get<ModifyAssetDocumentPayload>(sink.commands[0].payload);
    const auto layers = std::get<Json>(payload.changes[0].value);
    ASSERT_EQ(layers.size(), 1u);
    EXPECT_EQ(layers[0]["name"], "Rock");
}

TEST_F(TerrainAssetEditorTest, InvalidInputDoesNotSubmitAndShowsLocalError)
{
    auto* input = dynamic_cast<Rml::ElementFormControl*>(Find("terrain-field-size"));
    ASSERT_NE(input, nullptr);
    input->SetValue("not-a-vector");
    ASSERT_TRUE(controller.HandleChange(*input, "terrain-asset-field"));
    EXPECT_TRUE(sink.commands.empty());
    controller.Build(&authored, false);
    context->Update();
    EXPECT_NE(Find("terrain-asset-status")->GetInnerRML().find("Two components"), std::string::npos);
}

TEST_F(TerrainAssetEditorTest, AddLayerAvoidsExistingNamesAfterMiddleRemoval)
{
    authored.document["layers"] = Json::array({
        {{"name", "Layer_1"}, {"heightRange", {0.0, 1.0}}, {"slopeRange", {0.0, 90.0}}},
        {{"name", "Layer_3"}, {"heightRange", {0.0, 1.0}}, {"slopeRange", {0.0, 90.0}}}
    });
    controller.Build(&authored, false);
    context->Update();
    auto* add = Find("terrain-layer-add");
    ASSERT_NE(add, nullptr);
    ASSERT_TRUE(controller.HandleClick(*add, "terrain-layer-add"));
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& payload = std::get<ModifyAssetDocumentPayload>(sink.commands[0].payload);
    const auto layers = std::get<Json>(payload.changes[0].value);
    ASSERT_EQ(layers.size(), 3u);
    EXPECT_EQ(layers[2]["name"], "Layer_4");
}

TEST_F(TerrainAssetEditorTest, TerrainMaterialButtonOpensAssignedMaterialInstance)
{
    auto* open = Find("terrain-material-open");
    ASSERT_NE(open, nullptr);
    ASSERT_TRUE(controller.HandleClick(*open, "terrain-material-open"));
    ASSERT_EQ(sink.commands.size(), 1u);
    const auto& command = sink.commands[0];
    EXPECT_EQ(command.type, EditorCommandType::OpenMaterialInstanceAsset);
    const auto& payload = std::get<OpenMaterialInstanceAssetPayload>(
        command.payload);
    EXPECT_EQ(payload.assetPath, "Maps/MI_test.json");
}

TEST_F(TerrainAssetEditorTest, ActiveTerrainMaterialEditsGlobalTilingThroughMaterialSink)
{
    MaterialInstanceDocumentSnapshot material;
    material.assetPath = "Maps/MI_test.json";
    material.revision = 7;
    EditorParameterSnapshot tiling;
    tiling.name = "u_uvScale";
    tiling.type = EditorParameterType::Float;
    tiling.effectiveValue.type = EditorParameterType::Float;
    tiling.effectiveValue.values[0] = 4.0f;
    material.parameters.push_back(tiling);

    controller.Build(&authored, false, &material);
    context->Update();

    auto* input = dynamic_cast<Rml::ElementFormControl*>(
        Find("terrain-material-u_uvScale"));
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->GetValue(), "4");
    input->SetValue("8.5");
    ASSERT_TRUE(controller.HandleChange(*input, "terrain-material-parameter"));

    ASSERT_TRUE(sink.commands.empty());
    ASSERT_EQ(materialSink.calls.size(), 1u);
    EXPECT_EQ(materialSink.calls[0].materialInstancePath, "Maps/MI_test.json");
    EXPECT_EQ(materialSink.calls[0].parameter, "u_uvScale");
    EXPECT_FLOAT_EQ(materialSink.calls[0].value, 8.5f);
}

TEST_F(TerrainAssetEditorTest, TerrainMaterialTilingRequiresAnOpenMaterial)
{
    auto* open = Find("terrain-material-open");
    ASSERT_NE(open, nullptr);
    ASSERT_TRUE(controller.HandleClick(*open, "terrain-material-open"));
    sink.commands.clear();

    auto* input = Find("terrain-material-u_uvScale");
    EXPECT_EQ(input, nullptr);
}

} // namespace

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "editor/service/sceneDocumentService.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "editor/scene/editorSceneActorBuilder.h"
#include "render/lighting/physicalLighting.h"
#include "render/camera/cameraExposure.h"
#include "world/world.h"
#include "sceneNode.h"

namespace
{
using namespace VL;
using namespace VL::Editor;
using namespace VL::EditorUi;
using Json = nlohmann::json;

class SceneDocumentTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("vl-scene-save-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "Maps");
        path = root / "Maps/SC_test.json";
        source = {
            {"name", "Scene"}, {"type", "scene"}, {"worldUnit", "cm"},
            {"description", "Disposable document persistence fixture"},
            {"opaque", {{"custom", 123}}},
            {"objects", Json::array({
                {{"name", "Camera"}, {"type", "camera"}, {"position", {0, 0, 300}},
                    {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"fov", 60},
                    {"near_clip", 1}, {"far_clip", 10000}},
                {{"name", "Point"}, {"type", "pointLight"}, {"position", {0.1, 20, 30}},
                    {"rotation", {0, 0, 0}}, {"color", {1, 1, 1}}, {"intensity", 100},
                    {"intensityUnit", "lm"}, {"radius", 75}, {"custom", "preserved"}}
            })}
        };
        source["objects"][0]["exposure"] = DescribeCameraExposure(CameraExposure{});
        Write(source);
        service = std::make_unique<SceneDocumentService>(root);
        std::string error;
        ASSERT_TRUE(service->Open(path.string(), error)) << error;
    }
    void TearDown() override { std::filesystem::remove_all(root); }
    void Write(const Json& json) { std::ofstream file(path); file << json.dump(4); }
    Json Read() { std::ifstream file(path); return Json::parse(file); }
    EditorSceneActorSnapshot Point() const
    {
        EditorSceneActorSnapshot actor;
        actor.scenePath = path.string();
        actor.identity = "pointLight|Point";
        actor.type = EditorSceneActorType::PointLight;
        actor.position = Eigen::Vector3f(0.1f, 20, 30);
        actor.light.color = Eigen::Vector3f::Ones();
        actor.light.intensity = ReadCanonicalLightIntensity(source["objects"][1]);
        return actor;
    }
    void Record(EditorSceneActorSnapshot actor, EditorActorProperty property, float value,
        uint32_t component = 0)
    {
        std::string error;
        ASSERT_TRUE(service->PrepareActorEdit(actor.scenePath, actor.identity, error)) << error;
        service->RecordActorEdit(actor, {1, actor.identity, property, component, value});
    }
    EditorCommandEnvelope SaveCommand() const
    {
        EditorCommandEnvelope command;
        command.commandId = 23;
        command.type = EditorCommandType::SaveSceneDocument;
        command.payload = MaterialInstanceAssetPathPayload{"Maps/SC_test.json"};
        const auto documents = service->ModifiedDocuments();
        command.expectedDocumentRevision = documents.empty() ? 1 : documents.front().revision;
        return command;
    }
    std::filesystem::path root, path;
    Json source;
    std::unique_ptr<SceneDocumentService> service;
};

TEST_F(SceneDocumentTest, PositionSavePreservesUntouchedFieldsAndOriginalNumericPrecision)
{
    auto actor = Point();
    actor.position.y() = 42;
    Record(actor, EditorActorProperty::Position, 42, 1);
    ASSERT_EQ(service->ModifiedDocuments().size(), 1u);
    EXPECT_EQ(Read(), source);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    Json expected = source;
    expected["objects"][1]["position"][1] = 42;
    EXPECT_EQ(Read(), expected);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
}

TEST_F(SceneDocumentTest, ReturningToParsedBaselineClearsDirtyWithoutWriting)
{
    auto actor = Point();
    actor.position.x() = 10;
    Record(actor, EditorActorProperty::Position, 10);
    actor.position.x() = 0.1f;
    Record(actor, EditorActorProperty::Position, 0.1f);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
    EXPECT_EQ(Read(), source);
}

TEST_F(SceneDocumentTest, LumensUnitIsPreservedWhenSavingCanonicalCandela)
{
    auto actor = Point();
    actor.light.intensity = 42;
    Record(actor, EditorActorProperty::Intensity, 42);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    auto light = Read()["objects"][1];
    EXPECT_EQ(light["intensityUnit"], "lm");
    EXPECT_FLOAT_EQ(ReadCanonicalLightIntensity(light), 42);
}

TEST_F(SceneDocumentTest, SourceConflictKeepsDraftAndDoesNotOverwriteExternalChanges)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    auto external = source;
    external["description"] = "External change";
    Write(external);
    EXPECT_EQ(service->Save(SaveCommand()).errorCode, EditorErrorCode::SourceChanged);
    ASSERT_EQ(service->ModifiedDocuments().size(), 1u);
    EXPECT_EQ(service->ModifiedDocuments().front().status, EditorDocumentStatus::SourceChanged);
    EXPECT_EQ(Read(), external);
}

TEST_F(SceneDocumentTest, StaleConfirmationRejectsWithoutWritingOrClearingDirty)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    auto command = SaveCommand();
    actor.position.y() = 60;
    Record(actor, EditorActorProperty::Position, 60, 1);
    EXPECT_EQ(service->Save(command).errorCode, EditorErrorCode::StaleDocumentRevision);
    EXPECT_EQ(Read(), source);
    EXPECT_FALSE(service->ModifiedDocuments().empty());
}

TEST_F(SceneDocumentTest, DraftReplaysWithNewGenerationAndSurvivesOpeningAnotherScene)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error));
    auto edits = service->ReplayEdits(path.string(), 9, error);
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits.front().worldGeneration, 9u);
    EXPECT_EQ(edits.front().identity, "pointLight|Point");
    EXPECT_EQ(edits.front().value, 55);
}

TEST_F(SceneDocumentTest, ExternallyChangedBaselineIsNeverUsedForDraftReplay)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    auto external = source;
    external["objects"][1]["position"][0] = 200;
    Write(external);
    std::string error;
    EXPECT_TRUE(service->ReplayEdits(path.string(), 9, error).empty());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(service->ModifiedDocuments().empty());
    EXPECT_EQ(service->ModifiedDocuments().front().status, EditorDocumentStatus::SourceChanged);
}

TEST_F(SceneDocumentTest, ReplayOutsideRootReportsErrorWithoutThrowing)
{
    std::string error;
    EXPECT_TRUE(service->ReplayEdits("../SC_escape.json", 9, error).empty());
    EXPECT_FALSE(error.empty());
}

TEST_F(SceneDocumentTest, RetainedEditsApplyToTheReplacementWorldNotTheObserverCamera)
{
    auto actor = Point();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    World replacement(9, path.string());
    auto point = std::make_shared<PointLight>();
    point->SetPosition(Eigen::Vector3f(0.1f, 20, 30));
    replacement.AddPointLight("Point", point);
    replacement.SetCamera(std::make_shared<Camera>());
    replacement.SetEditorViewEnabled(true);
    const auto cameraPosition = replacement.GetRenderCamera()->GetPosition();
    std::string error;
    for (const auto& edit : service->ReplayEdits(path.string(), 9, error))
        ASSERT_TRUE(ApplyEditorActorProperty(replacement, edit, error)) << error;
    EXPECT_EQ(point->GetPosition(), Eigen::Vector3f(55, 20, 30));
    EXPECT_EQ(replacement.GetRenderCamera()->GetPosition(), cameraPosition);
    EXPECT_EQ(Read(), source);
}

TEST_F(SceneDocumentTest, SpotConeSavePreservesLumensUnitAndCanonicalRuntimeStrength)
{
    auto& light = source["objects"][1];
    light["type"] = "spotLight";
    light["cone_angle_inner"] = 15;
    light["cone_angle_outer"] = 30;
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    auto actor = Point();
    actor.identity = "spotLight|Point";
    actor.type = EditorSceneActorType::SpotLight;
    actor.light.coneAngleInner = 15 * static_cast<float>(LightingPi / 180);
    actor.light.coneAngleOuter = 60 * static_cast<float>(LightingPi / 180);
    Record(actor, EditorActorProperty::OuterCone, 60);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto saved = Read()["objects"][1];
    EXPECT_EQ(saved["intensityUnit"], "lm");
    EXPECT_NEAR(saved["cone_angle_outer"].get<double>(), 60, 1.0e-5);
    EXPECT_FLOAT_EQ(ReadCanonicalLightIntensity(saved), actor.light.intensity);
}

TEST_F(SceneDocumentTest, AreaSizeSavePreservesLumensUnitAndRadiance)
{
    auto& light = source["objects"][1];
    light["type"] = "squareAreaLight";
    light["size"] = 100;
    light["normal"] = {0, -1, 0};
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    auto actor = Point();
    actor.identity = "squareAreaLight|Point";
    actor.type = EditorSceneActorType::SquareAreaLight;
    actor.light.squareSize = 200;
    Record(actor, EditorActorProperty::AreaSize, 200);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto saved = Read()["objects"][1];
    EXPECT_EQ(saved["intensityUnit"], "lm");
    EXPECT_EQ(saved["size"], 200);
    EXPECT_NEAR(saved["intensity"].get<double>(), 400, 1.0e-4);
    EXPECT_FLOAT_EQ(ReadCanonicalLightIntensity(saved), actor.light.intensity);
}

TEST_F(SceneDocumentTest, MeshTrsSavePreservesReferencesAndUnmanagedFields)
{
    source["objects"].push_back({{"type", "mesh"}, {"name", "Model"},
        {"modelPath", "Meshes/SM_model.json"}, {"position", {0, 0, 0}},
        {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"custom", "retained"}});
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    EditorSceneActorSnapshot actor;
    actor.scenePath = path.string();
    actor.identity = "mesh|Model";
    actor.type = EditorSceneActorType::Mesh;
    actor.position = Eigen::Vector3f(100, 20, 30);
    actor.rotation = Eigen::Vector3f(0, 45, 0);
    actor.scale = Eigen::Vector3f(-2, 3, 4);
    Record(actor, EditorActorProperty::Position, 100);
    Record(actor, EditorActorProperty::Rotation, 45, 1);
    Record(actor, EditorActorProperty::Scale, -2);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto mesh = Read()["objects"][2];
    EXPECT_EQ(mesh["position"], Json::array({100, 20, 30}));
    EXPECT_EQ(mesh["rotation"], Json::array({0, 45, 0}));
    EXPECT_EQ(mesh["scale"], Json::array({-2, 3, 4}));
    EXPECT_EQ(mesh["modelPath"], "Meshes/SM_model.json");
    EXPECT_EQ(mesh["custom"], "retained");
}

TEST_F(SceneDocumentTest, CsmDefaultRevertClearsDirtyAndSaveRetainsOtherShadowSettings)
{
    auto& light = source["objects"][1];
    light["type"] = "directionalLight";
    light["intensityUnit"] = "lux";
    light["shadow"] = {{"distance", 5000}};
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    auto actor = Point();
    actor.identity = "directionalLight|Point";
    actor.type = EditorSceneActorType::DirectionalLight;
    actor.castShadows = false;
    Record(actor, EditorActorProperty::CastShadows, 0);
    actor.castShadows = true;
    Record(actor, EditorActorProperty::CastShadows, 1);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
    actor.castShadows = false;
    Record(actor, EditorActorProperty::CastShadows, 0);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto shadow = Read()["objects"][1]["shadow"];
    EXPECT_EQ(shadow["castShadows"], false);
    EXPECT_EQ(shadow["distance"], 5000);
}

TEST_F(SceneDocumentTest, PreviewSceneEditsNeverBecomeSaveableDocuments)
{
    auto actor = Point();
    actor.scenePath = (root / "Maps/SC_model_preview.json").string();
    actor.position.x() = 55;
    Record(actor, EditorActorProperty::Position, 55);
    EXPECT_TRUE(service->ModifiedDocuments().empty());
}

TEST_F(SceneDocumentTest, CameraTransformRemovesOverridingLookAtButKeepsExposure)
{
    source["objects"][0]["look_at"] = {0, 0, 0};
    Write(source);
    std::string error;
    ASSERT_TRUE(service->Open(path.string(), error)) << error;
    EditorSceneActorSnapshot actor;
    actor.scenePath = path.string();
    actor.identity = "camera|Camera";
    actor.type = EditorSceneActorType::Camera;
    actor.position = Eigen::Vector3f(10, 0, 300);
    actor.rotation = Eigen::Vector3f(0, 35, 0);
    Record(actor, EditorActorProperty::Position, 10);
    ASSERT_EQ(service->Save(SaveCommand()).status, EditorCommandStatus::Succeeded);
    const auto camera = Read()["objects"][0];
    EXPECT_FALSE(camera.contains("look_at"));
    EXPECT_EQ(camera["rotation"], Json::array({0, 35, 0}));
    EXPECT_EQ(camera["fov"], 60);
    EXPECT_EQ(camera["exposure"], source["objects"][0]["exposure"]);
}

TEST_F(SceneDocumentTest, PathEscapeAndAmbiguousActorAreRejected)
{
    std::string error;
    EXPECT_FALSE(service->Open("../SC_escape.json", error));
    source["objects"].push_back(source["objects"][1]);
    Write(source);
    ASSERT_TRUE(service->Open(path.string(), error));
    EXPECT_FALSE(service->PrepareActorEdit(path.string(), "pointLight|Point", error));
}

TEST_F(SceneDocumentTest, SceneSaveProtocolRoundTripsAndRequiresRevision)
{
    auto command = SaveCommand();
    const auto wire = EditorCommandJsonCodec::Encode(command);
    EXPECT_EQ(wire["command"], "scene.save");
    const auto decoded = EditorCommandJsonCodec::DecodeCommand(wire);
    EXPECT_EQ(decoded.type, EditorCommandType::SaveSceneDocument);
    EXPECT_EQ(decoded.expectedDocumentRevision, command.expectedDocumentRevision);
    EXPECT_FALSE(ValidateEditorCommandEnvelope(command).has_value());
    command.expectedDocumentRevision.reset();
    EXPECT_EQ(ValidateEditorCommandEnvelope(command), EditorErrorCode::MissingExpectedDocumentRevision);
}
} // namespace

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "editor/command/assetJsonCommandHost.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "render/camera/cameraExposure.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
using namespace VL;
using namespace VL::Editor;
using Json = nlohmann::json;

class AssetDocumentTest : public testing::Test
{
protected:
    void SetUp() override
    {
        root = std::filesystem::temp_directory_path() / ("vl-asset-modify-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        Write("MI_test.json", {{"name", "Test"}, {"type", "materialInstance"}});
        Write("T_test.json", {{"name", "Texture"}, {"type", "texture"}, {"source", "image.png"},
            {"colorSpace", "srgb"}, {"channelsDescription", {{"r", "color"}}}});
        Write("SM_test.json", {{"name", "Mesh"}, {"type", "mesh"}, {"modelDataPath", "triangle.obj"},
            {"custom", {{"data", {1, 2, 3}}}}, {"materialSlots", Json::array({
                {{"name", "Default"}, {"materialInstancePath", "MI_test.json"}}})}});
        std::ofstream(root / "image.png") << "not decoded during document validation";
        std::ofstream(root / "triangle.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        scene = {{"name", "Scene"}, {"type", "scene"}, {"description", "Asset API test"},
            {"worldUnit", "cm"}, {"opaque", {{"data", {1, 2, 3}}}},
            {"objects", Json::array({
                {{"name", "Camera"}, {"type", "camera"}, {"position", {0, 0, 300}},
                    {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"fov", 60},
                    {"near_clip", 1}, {"far_clip", 1000}, {"exposure", DescribeCameraExposure(CameraExposure{})}},
                {{"name", "Model"}, {"type", "mesh"}, {"modelPath", "SM_test.json"},
                    {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}},
                {{"name", "Spot"}, {"type", "spotLight"}, {"position", {0, 0, 0}},
                    {"rotation", {0, 0, 0}}, {"color", {1, 1, 1}}, {"intensity", 100},
                    {"intensityUnit", "lm"}, {"cone_angle_inner", 10}, {"cone_angle_outer", 20}}
            })}};
        Write("SC_test.json", scene);
        host = std::make_unique<AssetJsonCommandHost>(root);
    }
    void TearDown() override { host.reset(); std::filesystem::remove_all(root); }
    void Write(const std::string& path, const Json& value)
    { std::ofstream(root / path) << value.dump(4) << "\n"; }
    Json Read(const std::string& path)
    { std::ifstream file(root / path); return Json::parse(file); }
    Json Request(const std::string& command, const std::string& path,
        std::optional<uint64_t> revision = {}, const Json& changes = nullptr)
    {
        Json wire{{"protocolVersion", 1}, {"commandId", ++id}, {"source", "ai"},
            {"command", command}, {"payload", {{"assetPath", path}}}};
        if (revision) wire["expectedDocumentRevision"] = *revision;
        if (!changes.is_null()) wire["payload"]["changes"] = changes;
        return wire;
    }
    Json Run(const Json& wire) { return host->Process(wire.dump()); }
    Json Get(const std::string& kind, const std::string& path)
    { return Run(Request(kind + ".get_document", path)); }
    Json Modify(const std::string& kind, const std::string& path, uint64_t revision, const Json& changes)
    { return Run(Request(kind + ".modify", path, revision, changes)); }
    std::filesystem::path root;
    Json scene;
    std::unique_ptr<AssetJsonCommandHost> host;
    uint64_t id = 0;
};

TEST_F(AssetDocumentTest, AllAssetKindsPublishCapabilitiesAndStructuredCodecRoundTrips)
{
    for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
        {"scene", "SC_test.json"}, {"mesh", "SM_test.json"}, {"texture", "T_test.json"}})
    {
        const auto result = Get(entry.first, entry.second);
        ASSERT_EQ(result["status"], "succeeded") << result;
        EXPECT_EQ(result["payload"]["documentRevision"], 1);
        EXPECT_FALSE(result["payload"]["capabilities"]["contentPreflight"].get<bool>());
        EXPECT_EQ(EditorCommandJsonCodec::EncodeResult(EditorCommandJsonCodec::DecodeResult(result)), result);
        EXPECT_EQ(Run(Request(entry.first + ".get_capabilities", entry.second))["status"], "succeeded");
    }
}

TEST_F(AssetDocumentTest, ModifyStagesOnlyAndExplicitSavePreservesOpaqueSource)
{
    ASSERT_EQ(Get("scene", "SC_test.json")["status"], "succeeded");
    const auto changed = Modify("scene", "SC_test.json", 1, Json::array({
        {{"objectIdentity", "mesh|Model"}, {"field", "position"}, {"value", {1, 2, 3}}},
        {{"field", "description"}, {"value", "Changed author description"}}}));
    ASSERT_EQ(changed["status"], "succeeded") << changed;
    EXPECT_EQ(changed["documentRevision"], 2);
    EXPECT_EQ(Read("SC_test.json"), scene);
    EXPECT_EQ(Run(Request("scene.validate", "SC_test.json"))["status"], "succeeded");
    ASSERT_EQ(Run(Request("scene.save", "SC_test.json", 2))["status"], "succeeded");
    const auto saved = Read("SC_test.json");
    EXPECT_EQ(saved["opaque"], scene["opaque"]);
    EXPECT_EQ(saved["objects"][1]["position"], Json::array({1, 2, 3}));
    EXPECT_FALSE(Get("scene", "SC_test.json")["payload"]["dirty"].get<bool>());
}

TEST_F(AssetDocumentTest, CoupledCameraFieldsValidateFinalCandidate)
{
    Get("scene", "SC_test.json");
    EXPECT_EQ(Modify("scene", "SC_test.json", 1, Json::array({
        {{"objectIdentity", "camera|Camera"}, {"field", "near_clip"}, {"value", 1.0e-50}}}))["status"], "rejected");
    EXPECT_EQ(Modify("scene", "SC_test.json", 1, Json::array({
        {{"objectIdentity", "camera|Camera"}, {"field", "near_clip"}, {"value", 1000.0}},
        {{"objectIdentity", "camera|Camera"}, {"field", "far_clip"}, {"value", 1000.000001}}}))["status"], "rejected");
    const auto result = Modify("scene", "SC_test.json", 1, Json::array({
        {{"objectIdentity", "camera|Camera"}, {"field", "near_clip"}, {"value", 2000}},
        {{"objectIdentity", "camera|Camera"}, {"field", "far_clip"}, {"value", 4000}}}));
    EXPECT_EQ(result["status"], "succeeded") << result;
}

TEST_F(AssetDocumentTest, CoupledSpotAnglesUseFinalAuthoredLumens)
{
    Get("scene", "SC_test.json");
    const auto result = Modify("scene", "SC_test.json", 1, Json::array({
        {{"objectIdentity", "spotLight|Spot"}, {"field", "cone_angle_inner"}, {"value", 30}},
        {{"objectIdentity", "spotLight|Spot"}, {"field", "cone_angle_outer"}, {"value", 40}}}));
    EXPECT_EQ(result["status"], "succeeded") << result;
    EXPECT_EQ(result["payload"]["document"]["objects"][2]["intensity"], 100);
}

TEST_F(AssetDocumentTest, InvalidFinalCandidateRollsBackEveryFieldAndRevision)
{
    Get("scene", "SC_test.json");
    const auto result = Modify("scene", "SC_test.json", 1, Json::array({
        {{"field", "description"}, {"value", "Must roll back"}},
        {{"objectIdentity", "camera|Camera"}, {"field", "near_clip"}, {"value", 2000}}}));
    EXPECT_EQ(result["status"], "rejected") << result;
    EXPECT_EQ(Get("scene", "SC_test.json")["payload"]["document"]["description"], scene["description"]);
    EXPECT_EQ(Get("scene", "SC_test.json")["documentRevision"], 1);
}

TEST_F(AssetDocumentTest, NoOpDoesNotAdvanceRevision)
{
    Get("texture", "T_test.json");
    const auto result = Modify("texture", "T_test.json", 1,
        Json::array({{{"field", "colorSpace"}, {"value", "srgb"}}}));
    EXPECT_EQ(result["status"], "succeeded");
    EXPECT_EQ(result["documentRevision"], 1);
    EXPECT_TRUE(result["payload"]["changedFields"].empty());
}

TEST_F(AssetDocumentTest, StaleRevisionCannotOverwriteNewDraft)
{
    Get("texture", "T_test.json");
    ASSERT_EQ(Modify("texture", "T_test.json", 1,
        Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}))["status"], "succeeded");
    EXPECT_EQ(Modify("texture", "T_test.json", 1,
        Json::array({{{"field", "wrapMode"}, {"value", "clamp"}}}))["errorCode"], "stale_document_revision");
}

TEST_F(AssetDocumentTest, ExternalConflictPreservesFileAndDraft)
{
    Get("texture", "T_test.json");
    Modify("texture", "T_test.json", 1, Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}));
    auto external = Read("T_test.json");
    external["name"] = "External";
    Write("T_test.json", external);
    EXPECT_EQ(Run(Request("texture.save", "T_test.json", 2))["errorCode"], "source_changed");
    EXPECT_EQ(Read("T_test.json"), external);
    EXPECT_EQ(Get("texture", "T_test.json")["payload"]["document"]["colorSpace"], "linear");
}

TEST_F(AssetDocumentTest, DuplicateCommandReturnsOriginalResultWithoutReapplying)
{
    Get("texture", "T_test.json");
    const auto request = Request("texture.modify", "T_test.json", 1,
        Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}));
    const auto first = Run(request);
    EXPECT_EQ(Run(request), first);
    EXPECT_EQ(Get("texture", "T_test.json")["documentRevision"], 2);
}

TEST_F(AssetDocumentTest, UnknownAndDuplicateFieldsAndWrongTypesAreRejected)
{
    Get("texture", "T_test.json");
    for (const auto& changes : std::vector<Json>{
        Json::array({{{"field", "flipY"}, {"value", true}}}),
        Json::array({{{"field", "mipmaps"}, {"value", 1}}}),
        Json::array({{{"field", "filter"}, {"value", "trilinear"}}}),
        Json::array({{{"field", "filter"}, {"value", "linear"}}, {{"field", "filter"}, {"value", "nearest"}}}),
        Json::array({{{"field", "mipmaps"}, {"reset", 1}}})})
        EXPECT_EQ(Modify("texture", "T_test.json", 1, changes)["errorCode"], "invalid_payload");
    EXPECT_EQ(Get("texture", "T_test.json")["documentRevision"], 1);
}

TEST_F(AssetDocumentTest, ResetRemovesOptionalFieldAndRequiredResetIsRejected)
{
    Get("texture", "T_test.json");
    EXPECT_EQ(Modify("texture", "T_test.json", 1,
        Json::array({{{"field", "channelsDescription"}, {"reset", true}}}))["status"], "succeeded");
    EXPECT_FALSE(Get("texture", "T_test.json")["payload"]["document"].contains("channelsDescription"));
    EXPECT_EQ(Modify("texture", "T_test.json", 2,
        Json::array({{{"field", "source"}, {"reset", true}}}))["errorCode"], "invalid_payload");
}

TEST_F(AssetDocumentTest, CameraRotationClearsLookAtAndConflictingIntentIsRejected)
{
    scene["objects"][0]["look_at"] = {0, 0, 0};
    Write("SC_test.json", scene);
    Get("scene", "SC_test.json");
    const auto result = Modify("scene", "SC_test.json", 1, Json::array({
        {{"objectIdentity", "camera|Camera"}, {"field", "rotation"}, {"value", {0, 45, 0}}}}));
    ASSERT_EQ(result["status"], "succeeded") << result;
    EXPECT_FALSE(result["payload"]["document"]["objects"][0].contains("look_at"));
    EXPECT_EQ(Modify("scene", "SC_test.json", 2, Json::array({
        {{"objectIdentity", "camera|Camera"}, {"field", "rotation"}, {"value", {0, 45, 0}}},
        {{"objectIdentity", "camera|Camera"}, {"field", "look_at"}, {"value", {0, 0, 0}}}}))["status"], "rejected");
}

TEST_F(AssetDocumentTest, MeshSlotSelectorGuardsOrderAndReferencedType)
{
    Get("mesh", "SM_test.json");
    EXPECT_EQ(Modify("mesh", "SM_test.json", 1, Json::array({
        {{"field", "materialInstancePath"}, {"slotIndex", 0}, {"expectedSlotName", "Old"},
            {"value", "MI_test.json"}}}))["status"], "rejected");
    EXPECT_EQ(Modify("mesh", "SM_test.json", 1, Json::array({
        {{"field", "materialInstancePath"}, {"slotIndex", 0}, {"expectedSlotName", "Default"},
            {"value", "T_test.json"}}}))["status"], "rejected");
    Write("MI_other.json", {{"type", "materialInstance"}});
    const auto result = Modify("mesh", "SM_test.json", 1, Json::array({
        {{"field", "materialInstancePath"}, {"slotIndex", 0}, {"expectedSlotName", "Default"},
            {"value", "MI_other.json"}}}));
    EXPECT_EQ(result["status"], "succeeded") << result;
}

TEST_F(AssetDocumentTest, MissingSourceAndEscapedReferenceCannotPublishDraft)
{
    Get("texture", "T_test.json");
    for (const auto& path : {"missing.png", "../escape.png", "C:/escape.png"})
        EXPECT_EQ(Modify("texture", "T_test.json", 1,
            Json::array({{{"field", "source"}, {"value", path}}}))["status"], "rejected");
    EXPECT_EQ(Get("texture", "T_test.json")["documentRevision"], 1);
}

TEST_F(AssetDocumentTest, RootEscapeAndWrongAssetKindAreRejected)
{
    EXPECT_EQ(Get("texture", "../T_test.json")["status"], "rejected");
    EXPECT_EQ(Get("texture", (root / "T_test.json").string())["status"], "rejected");
    EXPECT_EQ(Get("texture", "SM_test.json")["status"], "failed");
}

TEST_F(AssetDocumentTest, SourceContentPreflightIsNotClaimedByDocumentValidation)
{
    Get("texture", "T_test.json");
    const auto result = Run(Request("texture.validate", "T_test.json"));
    EXPECT_EQ(result["status"], "succeeded");
    EXPECT_EQ(result["payload"]["validationLevel"], "document_and_references");
    EXPECT_FALSE(result["payload"]["capabilities"]["contentPreflight"].get<bool>());
}

TEST_F(AssetDocumentTest, ReloadRequiresExplicitDiscardAndRevertUsesBaseline)
{
    Get("texture", "T_test.json");
    Modify("texture", "T_test.json", 1, Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}));
    EXPECT_EQ(Run(Request("texture.reload", "T_test.json", 2))["errorCode"], "document_dirty");
    auto request = Request("texture.reload", "T_test.json", 2);
    request["payload"]["dirtyPolicy"] = "discard_changes";
    ASSERT_EQ(Run(request)["documentRevision"], 3);
    Modify("texture", "T_test.json", 3, Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}));
    EXPECT_EQ(Run(Request("texture.revert", "T_test.json", 4))["payload"]["document"]["colorSpace"], "srgb");
}

TEST_F(AssetDocumentTest, ReadProjectionOmitsOpaqueDataButSaveRetainsIt)
{
    const auto result = Get("mesh", "SM_test.json");
    EXPECT_FALSE(result["payload"]["document"].contains("custom"));
    Modify("mesh", "SM_test.json", 1, Json::array({{{"field", "name"}, {"value", "Renamed"}}}));
    EXPECT_EQ(Run(Request("mesh.save", "SM_test.json", 2))["status"], "succeeded");
    EXPECT_EQ(Read("SM_test.json")["custom"]["data"], Json::array({1, 2, 3}));
}

TEST_F(AssetDocumentTest, SceneDraftSurvivesUiEditsAndParticipatesInWorldLoad)
{
    SceneDocumentService service(root);
    auto read = EditorCommandJsonCodec::DecodeCommand(Request("scene.get_document", "SC_test.json"));
    ASSERT_EQ(service.Execute(read).status, EditorCommandStatus::Succeeded);
    auto modify = EditorCommandJsonCodec::DecodeCommand(Request("scene.modify", "SC_test.json", 1,
        Json::array({{{"objectIdentity", "mesh|Model"}, {"field", "position"}, {"value", {1, 2, 3}}}})));
    ASSERT_EQ(service.Execute(modify).status, EditorCommandStatus::Succeeded);
    std::string error;
    EXPECT_FALSE(service.PrepareActorEdit("SC_test.json", "mesh|Model", error));
    const auto draft = service.DraftForWorldLoad((root / "SC_test.json").string(), error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(draft["objects"][1]["position"], Json::array({1, 2, 3}));
    service.NotifyWorldLoaded("SC_test.json");
    ASSERT_TRUE(service.PrepareActorEdit("SC_test.json", "mesh|Model", error));
    EditorUi::EditorSceneActorSnapshot actor;
    actor.scenePath = "SC_test.json";
    actor.identity = "mesh|Model";
    actor.type = EditorUi::EditorSceneActorType::Mesh;
    actor.position = Eigen::Vector3f(9, 2, 3);
    service.RecordActorEdit(actor, {5, actor.identity, EditorActorProperty::Position, 0, 9});
    EXPECT_EQ(service.DraftForWorldLoad("SC_test.json", error)["objects"][1]["position"], Json::array({9, 2, 3}));
    EXPECT_TRUE(service.ReplayEdits("SC_test.json", 6, error).empty());
}

TEST_F(AssetDocumentTest, ExternalSceneConflictPreventsDraftWorldLoad)
{
    SceneDocumentService service(root);
    service.Execute(EditorCommandJsonCodec::DecodeCommand(Request("scene.get_document", "SC_test.json")));
    service.Execute(EditorCommandJsonCodec::DecodeCommand(Request("scene.modify", "SC_test.json", 1,
        Json::array({{{"field", "description"}, {"value", "Draft"}}}))));
    scene["description"] = "External";
    Write("SC_test.json", scene);
    std::string error;
    EXPECT_TRUE(service.DraftForWorldLoad("SC_test.json", error).is_null());
    EXPECT_FALSE(error.empty());
}

TEST_F(AssetDocumentTest, MalformedRequestDoesNotPoisonFollowingCommands)
{
    EXPECT_EQ(host->Process("{")["status"], "rejected");
    EXPECT_EQ(Get("texture", "T_test.json")["status"], "succeeded");
}

TEST_F(AssetDocumentTest, SourcesHaveIdenticalDocumentBehavior)
{
    for (const char* source : {"rmlui", "console", "ai"})
    {
        AssetJsonCommandHost other(root);
        auto request = Request("texture.get_document", "T_test.json");
        request["source"] = source;
        EXPECT_EQ(other.Process(request.dump())["status"], "succeeded");
        request = Request("texture.modify", "T_test.json", 1,
            Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}));
        request["source"] = source;
        const auto result = other.Process(request.dump());
        EXPECT_EQ(result["payload"]["document"]["colorSpace"], "linear");
        EXPECT_EQ(result["documentRevision"], 2);
    }
}

TEST_F(AssetDocumentTest, NormalizedAliasesShareRevisionAndOneDraftOwner)
{
    Get("texture", "./T_test.json");
    ASSERT_EQ(Modify("texture", "T_test.json", 1,
        Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}))["status"], "succeeded");
    EXPECT_EQ(Get("texture", "./T_test.json")["documentRevision"], 2);
#ifdef _WIN32
    EXPECT_EQ(Get("texture", "t_TEST.JSON")["documentRevision"], 2);
#endif
}

TEST_F(AssetDocumentTest, SymlinkAliasesShareDraftAndRetargetCannotSave)
{
    std::error_code error;
    std::filesystem::create_symlink(root / "T_test.json", root / "T_link.json", error);
    if (error) GTEST_SKIP() << "Symlink permission unavailable: " << error.message();
    Get("texture", "T_link.json");
    Modify("texture", "T_test.json", 1, Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}));
    EXPECT_EQ(Get("texture", "T_link.json")["documentRevision"], 2);
    Write("T_other.json", Read("T_test.json"));
    std::filesystem::remove(root / "T_link.json");
    std::filesystem::create_symlink(root / "T_other.json", root / "T_link.json");
    EXPECT_EQ(Run(Request("texture.save", "T_link.json", 2))["status"], "failed");
    EXPECT_EQ(Read("T_other.json")["colorSpace"], "srgb");
    EXPECT_EQ(Get("texture", "T_link.json")["payload"]["document"]["colorSpace"], "linear");
}

TEST_F(AssetDocumentTest, PreviewSceneCannotBecomeStandardEditableDocument)
{
    Write("SC_model_preview.json", scene);
    EXPECT_NE(Get("scene", "SC_model_preview.json")["status"], "succeeded");
    EXPECT_NE(Get("scene", "SC_MODEL_PREVIEW.json")["status"], "succeeded");
    EXPECT_EQ(Run(Request("scene.save", "SC_model_preview.json", 1))["errorCode"], "document_not_open");
}

TEST_F(AssetDocumentTest, SavedSceneStillRequiresWorldLoadBeforeLiveActorEdits)
{
    SceneDocumentService service(root);
    service.Execute(EditorCommandJsonCodec::DecodeCommand(Request("scene.get_document", "SC_test.json")));
    service.Execute(EditorCommandJsonCodec::DecodeCommand(Request("scene.modify", "SC_test.json", 1,
        Json::array({{{"field", "description"}, {"value", "New author data"}}}))));
    ASSERT_EQ(service.Execute(EditorCommandJsonCodec::DecodeCommand(
        Request("scene.save", "SC_test.json", 2))).status, EditorCommandStatus::Succeeded);
    std::string error;
    EXPECT_FALSE(service.PrepareActorEdit("SC_test.json", "mesh|Model", error));
    EXPECT_TRUE(service.DraftForWorldLoad("SC_test.json", error).is_null());
    service.NotifyWorldLoaded("SC_test.json");
    EXPECT_TRUE(service.PrepareActorEdit("SC_test.json", "mesh|Model", error));
}

TEST_F(AssetDocumentTest, SlotProjectionDoesNotExposeOpaqueNestedData)
{
    auto mesh = Read("SM_test.json");
    mesh["materialSlots"][0]["data"] = {1, 2, 3};
    Write("SM_test.json", mesh);
    EXPECT_FALSE(Get("mesh", "SM_test.json")["payload"]["document"]["materialSlots"][0].contains("data"));
    Modify("mesh", "SM_test.json", 1, Json::array({{{"field", "name"}, {"value", "Updated"}}}));
    Run(Request("mesh.save", "SM_test.json", 2));
    EXPECT_EQ(Read("SM_test.json")["materialSlots"][0]["data"], Json::array({1, 2, 3}));
}

TEST_F(AssetDocumentTest, DuplicateJsonKeysAndExcessDepthAreRejected)
{
    EXPECT_EQ(host->Process(R"({"commandId":1,"commandId":2})")["status"], "rejected");
    EXPECT_EQ(host->Process(std::string(65, '[') + "0" + std::string(65, ']'))["status"], "rejected");
    EXPECT_EQ(Get("texture", "T_test.json")["status"], "succeeded");
}

TEST_F(AssetDocumentTest, NestedEnvironmentValidationCannotBeBypassedByUnrelatedEdit)
{
    scene["objects"].push_back({{"type", "environment"}, {"name", "Sky"},
        {"environment", {{"type", "proceduralSky"}, {"cubeSize", -1}}}});
    Write("SC_test.json", scene);
    Get("scene", "SC_test.json");
    EXPECT_EQ(Modify("scene", "SC_test.json", 1,
        Json::array({{{"field", "description"}, {"value", "Must not publish"}}}))["status"], "rejected");
    EXPECT_EQ(Get("scene", "SC_test.json")["documentRevision"], 1);
}

TEST_F(AssetDocumentTest, CandidateDiagnosticIncludesSelectorsAndUnchangedRevision)
{
    Get("scene", "SC_test.json");
    const auto result = Modify("scene", "SC_test.json", 1, Json::array({
        {{"objectIdentity", "camera|Camera"}, {"field", "near_clip"}, {"value", 2000}}}));
    ASSERT_EQ(result["status"], "rejected");
    EXPECT_EQ(result["payload"]["diagnostics"][0]["scope"], "candidate");
    EXPECT_EQ(result["payload"]["diagnostics"][0]["targets"][0]["objectIdentity"], "camera|Camera");
    EXPECT_EQ(result["documentRevision"], 1);
    EXPECT_EQ(EditorCommandJsonCodec::EncodeResult(EditorCommandJsonCodec::DecodeResult(result)), result);
}

#ifdef _WIN32
TEST_F(AssetDocumentTest, AtomicReplaceFailurePreservesDirtyDraftAndRetrySucceeds)
{
    Get("texture", "T_test.json");
    Modify("texture", "T_test.json", 1, Json::array({{{"field", "colorSpace"}, {"value", "linear"}}}));
    const HANDLE lock = CreateFileW((root / "T_test.json").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);
    const auto result = Run(Request("texture.save", "T_test.json", 2));
    CloseHandle(lock);
    EXPECT_EQ(result["errorCode"], "atomic_write_failed") << result;
    EXPECT_EQ(Read("T_test.json")["colorSpace"], "srgb");
    EXPECT_TRUE(Get("texture", "T_test.json")["payload"]["dirty"].get<bool>());
    EXPECT_EQ(Run(Request("texture.save", "T_test.json", 2))["status"], "succeeded");
}
#endif
} // namespace

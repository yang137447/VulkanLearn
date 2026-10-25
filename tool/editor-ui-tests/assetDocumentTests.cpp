#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "editor/command/assetJsonCommandHost.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "render/camera/cameraExposure.h"
#include "mesh/validation/meshAssetValidator.h"
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
        std::filesystem::create_directories(root / "Skeletons");
        Write("Skeletons/SK_test.json", {{"format", "vl.skeleton.v1"}, {"skeleton", "SK_test"},
            {"root", "root"}, {"joints", Json::array()}});
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
    void WriteRetargetInputs()
    {
        auto mesh = Read("SM_test.json");
        mesh["skinning"] = {{"skeleton", "Skeletons/SK_test.json"}, {"clips", Json::array()}};
        Write("SM_source.json", mesh);
        Write("RT_test.json", {{"format", "vl.retarget.v1"}});
        mesh["skinning"]["sourceModel"] = "SM_source.json";
        mesh["skinning"]["retarget"] = "RT_test.json";
        Write("SM_test.json", mesh);
    }
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

TEST_F(AssetDocumentTest, SkeletalMeshValidationRequiresClipReferences)
{
    auto mesh = Read("SM_test.json");
    mesh["skinning"] = {{"skeleton", "Skeletons/SK_test.json"},
        {"clips", Json::array({{{"id", "walk"}, {"path", "walk.fbx"}}})}};
    Write("SM_test.json", mesh);
    ASSERT_EQ(Get("mesh", "SM_test.json")["status"], "succeeded");
    EXPECT_NE(Run(Request("mesh.validate", "SM_test.json"))["status"], "succeeded");
    std::ofstream(root / "walk.fbx") << "Content preflight is separate";
    EXPECT_EQ(Run(Request("mesh.validate", "SM_test.json"))["status"], "succeeded");
}

TEST_F(AssetDocumentTest, RetargetMeshSavePreservesSourceAndProfileOutsideModifyCapabilities)
{
    WriteRetargetInputs();
    const auto skinning = Read("SM_test.json")["skinning"];
    ASSERT_EQ(Get("mesh", "SM_test.json")["status"], "succeeded");
    ASSERT_EQ(Modify("mesh", "SM_test.json", 1,
        Json::array({{{"field", "name"}, {"value", "Retarget target"}}}))["status"], "succeeded");
    ASSERT_EQ(Run(Request("mesh.save", "SM_test.json", 2))["status"], "succeeded");
    EXPECT_EQ(Read("SM_test.json")["skinning"], skinning);
}

TEST_F(AssetDocumentTest, RetargetMeshValidationRequiresProfileReference)
{
    WriteRetargetInputs();
    std::filesystem::remove(root / "RT_test.json");
    ASSERT_EQ(Get("mesh", "SM_test.json")["status"], "succeeded");
    EXPECT_NE(Run(Request("mesh.validate", "SM_test.json"))["status"], "succeeded");
}

TEST_F(AssetDocumentTest, RetargetMeshValidationRejectsNonSkeletalSource)
{
    WriteRetargetInputs();
    auto source = Read("SM_source.json");
    source.erase("skinning");
    Write("SM_source.json", source);
    ASSERT_EQ(Get("mesh", "SM_test.json")["status"], "succeeded");
    EXPECT_NE(Run(Request("mesh.validate", "SM_test.json"))["status"], "succeeded");
}

TEST_F(AssetDocumentTest, RetargetMeshValidationRejectsNestedSource)
{
    WriteRetargetInputs();
    auto source = Read("SM_source.json");
    source["skinning"]["sourceModel"] = "SM_other.json";
    source["skinning"]["retarget"] = "RT_test.json";
    Write("SM_source.json", source);
    ASSERT_EQ(Get("mesh", "SM_test.json")["status"], "succeeded");
    EXPECT_NE(Run(Request("mesh.validate", "SM_test.json"))["status"], "succeeded");
}

TEST_F(AssetDocumentTest, TressfxSimulationFieldsRoundTripThroughStandardModify)
{
    auto mesh = Read("SM_test.json");
    mesh["type"] = "tressfx";
    mesh["modelDataPath"] = "guides.tfx";
    mesh["strandRadius"] = 0.035;
    Write("SM_test.json", mesh);
    std::ofstream(root / "guides.tfx") << "Content preflight is separate";
    const auto opened = Get("mesh", "SM_test.json");
    ASSERT_EQ(opened["status"], "succeeded") << opened;
    bool sawDamping = false, sawGravity = false;
    for (const auto& field : opened["payload"]["capabilities"]["fields"])
    {
        if (field["field"] == "simulation.damping") sawDamping = true;
        if (field["field"] == "simulation.gravity") sawGravity = true;
    }
    EXPECT_TRUE(sawDamping);
    EXPECT_TRUE(sawGravity);

    const auto modified = Modify("mesh", "SM_test.json", 1, Json::array({
        {{"field", "simulation.damping"}, {"value", 0.02}},
        {{"field", "simulation.iterations"}, {"value", 8}},
        {{"field", "simulation.gravity"}, {"value", Json::array({0.0, -500.0, 0.0})}},
    }));
    ASSERT_EQ(modified["status"], "succeeded") << modified;
    ASSERT_EQ(Run(Request("mesh.save", "SM_test.json", 2))["status"], "succeeded");
    const auto saved = Read("SM_test.json");
    EXPECT_DOUBLE_EQ(saved["simulation"]["damping"].get<double>(), 0.02);
    EXPECT_EQ(saved["simulation"]["iterations"], 8);
    EXPECT_EQ(saved["simulation"]["gravity"], Json::array({0.0, -500.0, 0.0}));

    // 越界值必须由资产校验拒绝，不能写进文件。
    const auto invalid = Modify("mesh", "SM_test.json", 3, Json::array({
        {{"field", "simulation.damping"}, {"value", 2.0}}}));
    EXPECT_NE(invalid["status"], "succeeded");
    EXPECT_DOUBLE_EQ(Read("SM_test.json")["simulation"]["damping"].get<double>(), 0.02);
}

TEST_F(AssetDocumentTest, TressfxMeshSavePreservesRadiusOutsideModifyCapabilities)
{
    auto mesh = Read("SM_test.json");
    mesh["type"] = "tressfx";
    mesh["modelDataPath"] = "guides.tfx";
    mesh["strandRadius"] = 0.035;
    Write("SM_test.json", mesh);
    std::ofstream(root / "guides.tfx") << "Content preflight is separate";
    ASSERT_EQ(Get("mesh", "SM_test.json")["status"], "succeeded");
    ASSERT_EQ(Modify("mesh", "SM_test.json", 1,
        Json::array({{{"field", "name"}, {"value", "Renamed guides"}}}))["status"], "succeeded");
    EXPECT_EQ(Run(Request("mesh.validate", "SM_test.json"))["status"], "succeeded");
    ASSERT_EQ(Run(Request("mesh.save", "SM_test.json", 2))["status"], "succeeded");
    EXPECT_EQ(Read("SM_test.json")["strandRadius"], 0.035);
    EXPECT_EQ(Read("SM_test.json")["type"], "tressfx");
}

TEST_F(AssetDocumentTest, SceneReferencesAcceptTressfxMeshDescriptors)
{
    auto mesh = Read("SM_test.json");
    mesh["type"] = "tressfx";
    mesh["modelDataPath"] = "guides.tfx";
    mesh["strandRadius"] = 0.035;
    Write("SM_test.json", mesh);
    ASSERT_EQ(Get("scene", "SC_test.json")["status"], "succeeded");
    EXPECT_EQ(Run(Request("scene.validate", "SC_test.json"))["status"], "succeeded");
}

TEST_F(AssetDocumentTest, SkeletalMeshRejectsDuplicateClipIds)
{
    auto mesh = Read("SM_test.json");
    mesh["skinning"] = {{"skeleton", "Skeletons/SK_test.json"},
        {"clips", Json::array({{{"id", "walk"}, {"path", "a.fbx"}},
            {{"id", "walk"}, {"path", "b.fbx"}}})}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_test.json", mesh), std::runtime_error);
}

TEST_F(AssetDocumentTest, SkeletalMeshRejectsStaticSectionFiltering)
{
    auto mesh = Read("SM_test.json");
    mesh["skinning"] = {{"skeleton", "Skeletons/SK_test.json"}, {"clips", Json::array()}};
    mesh["importOptions"] = {{"includedSectionNames", {"section"}}};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_test.json", mesh), std::runtime_error);
}

Json BlueprintAsset()
{
    return {{"type", "blueprint"}, {"name", "Actor"}, {"class", "Gameplay.RotatorBlueprint"},
        {"properties", {{"DegreesPerSecond", 45}}}, {"instanceEditable", Json::array()},
        {"components", Json::array({{{"id", "pivot"}, {"type", "scene"}}})}};
}

TEST_F(AssetDocumentTest, BlueprintCommandsRoundTripAndSaveThroughOneDocumentOwner)
{
    auto asset = BlueprintAsset();
    asset["type"] = "blueprint";
    Write("BP_test.json", asset);
    const auto opened = Get("blueprint", "BP_test.json");
    ASSERT_EQ(opened["status"], "succeeded") << opened;
    const auto wire = Request("blueprint.modify", "BP_test.json", 1,
        Json::array({{{"field", "name"}, {"value", "Blueprint Draft"}}}));
    const auto command = EditorCommandJsonCodec::DecodeCommand(wire);
    EXPECT_EQ(EditorCommandJsonCodec::Encode(command), wire);
    ASSERT_EQ(Run(wire)["status"], "succeeded");
    EXPECT_EQ(Get("blueprint", "BP_test.json")["payload"]["document"]["name"], "Blueprint Draft");
    EXPECT_EQ(Read("BP_test.json"), asset);
    EXPECT_EQ(Run(Request("blueprint.validate", "BP_test.json"))["status"], "succeeded");
    ASSERT_EQ(Run(Request("blueprint.save", "BP_test.json", 2))["status"], "succeeded");
    EXPECT_EQ(Read("BP_test.json")["type"], "blueprint");
    EXPECT_EQ(Read("BP_test.json")["name"], "Blueprint Draft");
    EXPECT_FALSE(Get("blueprint", "BP_test.json")["payload"]["dirty"].get<bool>());
}

TEST_F(AssetDocumentTest, BlueprintHistoryCommandsRequireRevisionAndRoundTripWithoutChangesPayload)
{
    Write("BP_test.json", BlueprintAsset());
    const auto opened = Get("blueprint", "BP_test.json");
    EXPECT_FALSE(opened["payload"]["canUndo"].get<bool>());
    EXPECT_FALSE(opened["payload"]["canRedo"].get<bool>());
    for (const char* operation : {"undo", "redo"})
    {
        const std::string command = std::string("blueprint.") + operation;
        EXPECT_THROW(EditorCommandJsonCodec::DecodeCommand(Request(command, "BP_test.json")),
            EditorCommandJsonCodecError);
        const auto wire = Request(command, "BP_test.json", 1);
        EXPECT_EQ(EditorCommandJsonCodec::Encode(EditorCommandJsonCodec::DecodeCommand(wire)), wire);
        EXPECT_EQ(Run(wire)["status"], "rejected");
        EXPECT_EQ(Get("blueprint", "BP_test.json")["documentRevision"], 1);
    }
    for (const char* kind : {"scene", "mesh", "texture"})
        EXPECT_THROW(EditorCommandJsonCodec::DecodeCommand(
            Request(std::string(kind) + ".undo", "BP_test.json", 1)), EditorCommandJsonCodecError);
}

TEST_F(AssetDocumentTest, BlueprintUndoRestoresCompleteAcceptedEditAndRedoStagesWithoutWriting)
{
    const auto baseline = BlueprintAsset();
    Write("BP_test.json", baseline);
    Get("blueprint", "BP_test.json");
    const auto changed = Modify("blueprint", "BP_test.json", 1, Json::array({
        {{"field", "name"}, {"value", "Changed"}},
        {{"field", "instanceEditable"}, {"value", {"DegreesPerSecond"}}},
        {{"field", "components"}, {"value", Json::array({
            {{"id", "pivot"}, {"type", "scene"}},
            {{"id", "camera"}, {"parent", "pivot"}, {"type", "camera"}}})}}
    }));
    ASSERT_EQ(changed["status"], "succeeded") << changed;
    EXPECT_TRUE(changed["payload"]["canUndo"].get<bool>());
    const auto undone = Run(Request("blueprint.undo", "BP_test.json", 2));
    ASSERT_EQ(undone["status"], "succeeded") << undone;
    EXPECT_EQ(undone["documentRevision"], 3);
    EXPECT_FALSE(undone["payload"]["dirty"].get<bool>());
    EXPECT_EQ(undone["payload"]["document"], Get("blueprint", "BP_test.json")["payload"]["document"]);
    EXPECT_EQ(undone["payload"]["document"]["components"], baseline["components"]);
    EXPECT_TRUE(undone["payload"]["canRedo"].get<bool>());
    EXPECT_FALSE(undone["payload"]["canUndo"].get<bool>());
    const auto redone = Run(Request("blueprint.redo", "BP_test.json", 3));
    ASSERT_EQ(redone["status"], "succeeded") << redone;
    EXPECT_EQ(redone["payload"]["document"], changed["payload"]["document"]);
    EXPECT_EQ(EditorCommandJsonCodec::EncodeResult(EditorCommandJsonCodec::DecodeResult(redone)), redone);
    EXPECT_EQ(Read("BP_test.json"), baseline);
}

TEST_F(AssetDocumentTest, BlueprintSaveKeepsHistoryAndUndoBecomesDirtyAgainstSavedBaseline)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    Modify("blueprint", "BP_test.json", 1, Json::array({{{"field", "name"}, {"value", "Saved"}}}));
    ASSERT_EQ(Run(Request("blueprint.save", "BP_test.json", 2))["status"], "succeeded");
    const auto undo = Run(Request("blueprint.undo", "BP_test.json", 3));
    ASSERT_EQ(undo["status"], "succeeded") << undo;
    EXPECT_TRUE(undo["payload"]["dirty"].get<bool>());
    EXPECT_EQ(undo["payload"]["document"]["name"], "Actor");
    EXPECT_EQ(Read("BP_test.json")["name"], "Saved");
    const auto redo = Run(Request("blueprint.redo", "BP_test.json", 4));
    ASSERT_EQ(redo["status"], "succeeded") << redo;
    EXPECT_FALSE(redo["payload"]["dirty"].get<bool>());
}

TEST_F(AssetDocumentTest, BlueprintRejectedStaleAndNoOpEditsDoNotConsumeRedo)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    Modify("blueprint", "BP_test.json", 1, Json::array({{{"field", "name"}, {"value", "Changed"}}}));
    Run(Request("blueprint.undo", "BP_test.json", 2));
    EXPECT_EQ(Modify("blueprint", "BP_test.json", 3,
        Json::array({{{"field", "name"}, {"value", "Actor"}}}))["documentRevision"], 3);
    EXPECT_EQ(Modify("blueprint", "BP_test.json", 3,
        Json::array({{{"field", "class"}, {"value", ""}}}))["status"], "rejected");
    EXPECT_EQ(Run(Request("blueprint.redo", "BP_test.json", 2))["errorCode"], "stale_document_revision");
    EXPECT_TRUE(Get("blueprint", "BP_test.json")["payload"]["canRedo"].get<bool>());
    ASSERT_EQ(Run(Request("blueprint.redo", "BP_test.json", 3))["status"], "succeeded");
    EXPECT_EQ(Get("blueprint", "BP_test.json")["payload"]["document"]["name"], "Changed");
}

TEST_F(AssetDocumentTest, BlueprintNewEditAfterUndoDropsRedoBranch)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    Modify("blueprint", "BP_test.json", 1, Json::array({{{"field", "name"}, {"value", "Old Branch"}}}));
    Run(Request("blueprint.undo", "BP_test.json", 2));
    const auto changed = Modify("blueprint", "BP_test.json", 3,
        Json::array({{{"field", "name"}, {"value", "New Branch"}}}));
    ASSERT_EQ(changed["status"], "succeeded");
    EXPECT_FALSE(changed["payload"]["canRedo"].get<bool>());
    EXPECT_EQ(Run(Request("blueprint.redo", "BP_test.json", 4))["status"], "rejected");
    EXPECT_EQ(Get("blueprint", "BP_test.json")["documentRevision"], 4);
}

TEST_F(AssetDocumentTest, BlueprintRevertDiscardsBothHistoryBranches)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    Modify("blueprint", "BP_test.json", 1, Json::array({{{"field", "name"}, {"value", "First"}}}));
    Modify("blueprint", "BP_test.json", 2, Json::array({{{"field", "name"}, {"value", "Second"}}}));
    Run(Request("blueprint.undo", "BP_test.json", 3));
    const auto reverted = Run(Request("blueprint.revert", "BP_test.json", 4));
    ASSERT_EQ(reverted["status"], "succeeded");
    EXPECT_FALSE(reverted["payload"]["canUndo"].get<bool>());
    EXPECT_FALSE(reverted["payload"]["canRedo"].get<bool>());
    EXPECT_FALSE(reverted["payload"]["dirty"].get<bool>());
}

TEST_F(AssetDocumentTest, BlueprintExternalSaveConflictKeepsHistoryUntilExplicitReload)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    Modify("blueprint", "BP_test.json", 1, Json::array({{{"field", "name"}, {"value", "Draft"}}}));
    auto external = BlueprintAsset(); external["name"] = "External"; Write("BP_test.json", external);
    EXPECT_EQ(Run(Request("blueprint.save", "BP_test.json", 2))["errorCode"], "source_changed");
    ASSERT_EQ(Run(Request("blueprint.undo", "BP_test.json", 2))["status"], "succeeded");
    ASSERT_EQ(Run(Request("blueprint.redo", "BP_test.json", 3))["status"], "succeeded");
    EXPECT_EQ(Run(Request("blueprint.save", "BP_test.json", 4))["errorCode"], "source_changed");
    auto reload = Request("blueprint.reload", "BP_test.json", 4);
    EXPECT_EQ(Run(reload)["errorCode"], "document_dirty");
    reload = Request("blueprint.reload", "BP_test.json", 4);
    reload["payload"]["dirtyPolicy"] = "discard_changes";
    const auto loaded = Run(reload);
    ASSERT_EQ(loaded["status"], "succeeded") << loaded;
    EXPECT_EQ(loaded["payload"]["document"]["name"], "External");
    EXPECT_FALSE(loaded["payload"]["canUndo"].get<bool>());
    EXPECT_FALSE(loaded["payload"]["canRedo"].get<bool>());
    EXPECT_EQ(Read("BP_test.json"), external);
}

TEST_F(AssetDocumentTest, BlueprintHistoryRevalidatesReferencesBeforeConsumingStep)
{
    auto asset = BlueprintAsset();
    asset["components"] = Json::array({{{"id", "mesh"}, {"type", "mesh"}, {"modelPath", "SM_test.json"}}});
    Write("BP_test.json", asset);
    Get("blueprint", "BP_test.json");
    Modify("blueprint", "BP_test.json", 1, Json::array({{{"field", "components"}, {"value", Json::array()}}}));
    const auto mesh = Read("SM_test.json");
    std::filesystem::remove(root / "SM_test.json");
    const auto rejected = Run(Request("blueprint.undo", "BP_test.json", 2));
    EXPECT_EQ(rejected["status"], "rejected");
    EXPECT_EQ(rejected["documentRevision"], 2);
    EXPECT_TRUE(rejected["payload"]["document"]["components"].empty());
    EXPECT_TRUE(rejected["payload"]["canUndo"].get<bool>());
    Write("SM_test.json", mesh);
    EXPECT_EQ(Run(Request("blueprint.undo", "BP_test.json", 2))["status"], "succeeded");
}

TEST_F(AssetDocumentTest, BlueprintHistoryIsLimitedToThirtyTwoSteps)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    uint64_t revision = 1;
    for (int index = 1; index <= 40; ++index)
    {
        const auto result = Modify("blueprint", "BP_test.json", revision++,
            Json::array({{{"field", "name"}, {"value", "Edit " + std::to_string(index)}}}));
        ASSERT_EQ(result["status"], "succeeded") << result;
    }
    for (int index = 0; index < 32; ++index)
        ASSERT_EQ(Run(Request("blueprint.undo", "BP_test.json", revision++))["status"], "succeeded");
    const auto oldest = Get("blueprint", "BP_test.json");
    EXPECT_EQ(oldest["payload"]["document"]["name"], "Edit 8");
    EXPECT_FALSE(oldest["payload"]["canUndo"].get<bool>());
    EXPECT_TRUE(oldest["payload"]["canRedo"].get<bool>());
    EXPECT_EQ(Run(Request("blueprint.undo", "BP_test.json", revision))["status"], "rejected");
}

TEST_F(AssetDocumentTest, BlueprintHistorySerialisedBytesAreLimitedToSixteenMiB)
{
    auto asset = BlueprintAsset();
    asset["description"] = std::string(900000, 'x');
    asset["name"] = "Draft-00";
    Write("BP_test.json", asset);
    Get("blueprint", "BP_test.json");
    uint64_t revision = 1;
    for (int index = 1; index <= 22; ++index)
    {
        const std::string name = "Draft-" + std::string(index < 10 ? "0" : "") + std::to_string(index);
        ASSERT_EQ(Modify("blueprint", "BP_test.json", revision++,
            Json::array({{{"field", "name"}, {"value", name}}}))["status"], "succeeded");
    }
    const size_t retained = 16 * 1024 * 1024 / asset.dump().size();
    ASSERT_LT(retained, 22);
    for (size_t index = 0; index < retained; ++index)
        ASSERT_EQ(Run(Request("blueprint.undo", "BP_test.json", revision++))["status"], "succeeded");
    const auto oldest = Get("blueprint", "BP_test.json");
    EXPECT_FALSE(oldest["payload"]["canUndo"].get<bool>());
    EXPECT_TRUE(oldest["payload"]["canRedo"].get<bool>());
    EXPECT_EQ(Run(Request("blueprint.undo", "BP_test.json", revision))["status"], "rejected");
}

TEST_F(AssetDocumentTest, LegacyActorCommandsTypesAndPrefixesAreRejected)
{
    Write("BP_test.json", BlueprintAsset());
    for (const char* operation : {"get_document", "get_capabilities", "modify", "validate", "save", "revert", "reload"})
        EXPECT_THROW(EditorCommandJsonCodec::DecodeCommand(
            Request(std::string("actor.") + operation, "BP_test.json")), EditorCommandJsonCodecError);
    auto legacy = BlueprintAsset();
    legacy["type"] = "actorAsset";
    Write("BP_legacy.json", legacy);
    EXPECT_EQ(Get("blueprint", "BP_legacy.json")["status"], "failed");
    Write("ACT_test.json", BlueprintAsset());
    EXPECT_EQ(Get("blueprint", "ACT_test.json")["status"], "failed");
}

TEST_F(AssetDocumentTest, BlueprintStructuredDefaultsComponentsAndExposureSaveOnlyOnExplicitRequest)
{
    const auto baseline = BlueprintAsset();
    Write("BP_test.json", baseline);
    const auto opened = Get("blueprint", "BP_test.json");
    ASSERT_EQ(opened["status"], "succeeded") << opened;
    const auto changes = Json::array({
        {{"field", "properties"}, {"value", {{"DegreesPerSecond", 90}, {"Category", "Gameplay"}}}},
        {{"field", "instanceEditable"}, {"value", {"DegreesPerSecond"}}},
        {{"field", "components"}, {"value", Json::array({
            {{"id", "pivot"}, {"type", "scene"}},
            {{"id", "camera"}, {"parent", "pivot"}, {"type", "camera"}, {"fov", 65}, {"near_clip", 1}, {"far_clip", 5000}}
        })}},
        {{"field", "physics"}, {"value", {{"motion", "dynamic"}, {"shape", "sphere"}, {"radius", 30}}}}
    });
    const auto modified = Modify("blueprint", "BP_test.json", 1, changes);
    ASSERT_EQ(modified["status"], "succeeded") << modified;
    EXPECT_EQ(Read("BP_test.json"), baseline);
    EXPECT_EQ(Run(Request("blueprint.validate", "BP_test.json"))["status"], "succeeded");
    EXPECT_EQ(Run(Request("blueprint.save", "BP_test.json", 2))["status"], "succeeded");
    EXPECT_EQ(Read("BP_test.json")["components"][1]["parent"], "pivot");
    EXPECT_EQ(Read("BP_test.json")["instanceEditable"], Json::array({"DegreesPerSecond"}));
    EXPECT_FALSE(Get("blueprint", "BP_test.json")["payload"]["dirty"].get<bool>());
}

TEST_F(AssetDocumentTest, ActorInvalidComponentTreeRollsBackCompleteCandidate)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    const auto result = Modify("blueprint", "BP_test.json", 1, Json::array({
        {{"field", "name"}, {"value", "Rejected"}},
        {{"field", "components"}, {"value", Json::array({{{"id", "a"}, {"type", "scene"}, {"parent", "missing"}}})}}
    }));
    EXPECT_EQ(result["status"], "rejected") << result;
    EXPECT_EQ(Get("blueprint", "BP_test.json")["documentRevision"], 1);
    EXPECT_EQ(Get("blueprint", "BP_test.json")["payload"]["document"]["name"], "Actor");
}

TEST_F(AssetDocumentTest, ActorExposureRejectsDuplicateNamesAndWrongShape)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    for (const Json& value : {Json::array({"Speed", "Speed"}), Json::array({42}), Json::object()})
        EXPECT_EQ(Modify("blueprint", "BP_test.json", 1,
            Json::array({{{"field", "instanceEditable"}, {"value", value}}}))["status"], "rejected");
}

TEST_F(AssetDocumentTest, BlueprintComponentPermissionUsesSharedValidationAndSaveProjection)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    const auto changed = Modify("blueprint", "BP_test.json", 1, Json::array({
        {{"field", "componentInstanceEditable"}, {"value", {{"pivot", {"position", "rotation"}}}}}
    }));
    ASSERT_EQ(changed["status"], "succeeded") << changed;
    EXPECT_EQ(changed["payload"]["document"]["componentInstanceEditable"],
        Json({{"pivot", {"position", "rotation"}}}));
    EXPECT_FALSE(Read("BP_test.json").contains("componentInstanceEditable"));
    EXPECT_EQ(Modify("blueprint", "BP_test.json", 2, Json::array({
        {{"field", "componentInstanceEditable"}, {"value", {{"pivot", {"parent"}}}}}
    }))["status"], "rejected");
    ASSERT_EQ(Run(Request("blueprint.save", "BP_test.json", 2))["status"], "succeeded");
    EXPECT_EQ(Read("BP_test.json")["componentInstanceEditable"], Json({{"pivot", {"position", "rotation"}}}));
}
TEST_F(AssetDocumentTest, ActorDraftRevisionAndExternalConflictPreserveBothAuthors)
{
    Write("BP_test.json", BlueprintAsset());
    Get("blueprint", "BP_test.json");
    ASSERT_EQ(Modify("blueprint", "BP_test.json", 1,
        Json::array({{{"field", "name"}, {"value", "Draft"}}}))["status"], "succeeded");
    EXPECT_EQ(Modify("blueprint", "BP_test.json", 1,
        Json::array({{{"field", "name"}, {"value", "Stale"}}}))["errorCode"], "stale_document_revision");
    auto external = BlueprintAsset(); external["name"] = "External"; Write("BP_test.json", external);
    EXPECT_EQ(Run(Request("blueprint.save", "BP_test.json", 2))["errorCode"], "source_changed");
    EXPECT_EQ(Read("BP_test.json"), external);
    EXPECT_EQ(Get("blueprint", "BP_test.json")["payload"]["document"]["name"], "Draft");
}

TEST_F(AssetDocumentTest, ActorRuntimeCatalogRejectsUnknownPropertiesAndInstanceNames)
{
    Write("BP_test.json", BlueprintAsset());
    AssetDocumentService service(root, EditorAssetDocumentKind::Blueprint);
    service.SetBlueprintClasses(Json::array({{{"name", "Gameplay.RotatorBlueprint"}, {"properties", Json::array({
        {{"name", "DegreesPerSecond"}, {"type", "number"}, {"value", 45}}
    })}}}));
    auto get = EditorCommandJsonCodec::DecodeCommand(Request("blueprint.get_document", "BP_test.json"));
    ASSERT_EQ(service.Execute(get).status, EditorCommandStatus::Succeeded);
    for (const Json& change : {
        Json{{"field", "properties"}, {"value", {{"DegreesPerSecond", "bad"}}}},
        Json{{"field", "instanceEditable"}, {"value", {"Missing"}}},
        Json{{"field", "class"}, {"value", "Gameplay.Unknown"}}})
    {
        const auto command = EditorCommandJsonCodec::DecodeCommand(Request("blueprint.modify", "BP_test.json", 1, Json::array({change})));
        EXPECT_EQ(service.Execute(command).status, EditorCommandStatus::Rejected);
    }
    EXPECT_FALSE(service.DocumentSnapshots()[0].dirty);
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

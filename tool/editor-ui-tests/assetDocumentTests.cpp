#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include "editor/command/assetDocumentFields.h"
#include "editor/command/assetJsonCommandHost.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "render/camera/cameraExposure.h"
#include "mesh/validation/meshAssetValidator.h"
#include "editor/ui/tressfxHairSimulationControls.h"
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
    Write("TF_test.json", mesh);
    std::ofstream(root / "guides.tfx") << "Content preflight is separate";
    const auto opened = Get("tressfx", "TF_test.json");
    ASSERT_EQ(opened["status"], "succeeded") << opened;
    bool sawDamping = false, sawGravity = false;
    for (const auto& field : opened["payload"]["capabilities"]["fields"])
    {
        if (field["field"] == "simulationDefaults.damping") sawDamping = true;
        if (field["field"] == "simulationDefaults.gravity") sawGravity = true;
    }
    EXPECT_TRUE(sawDamping);
    EXPECT_TRUE(sawGravity);

    const auto modified = Modify("tressfx", "TF_test.json", 1, Json::array({
        {{"field", "simulationDefaults.damping"}, {"value", 0.02}},
        {{"field", "simulationDefaults.iterations"}, {"value", 8}},
        {{"field", "simulationDefaults.gravity"}, {"value", Json::array({0.0, -500.0, 0.0})}},
    }));
    ASSERT_EQ(modified["status"], "succeeded") << modified;
    ASSERT_EQ(Run(Request("tressfx.save", "TF_test.json", 2))["status"], "succeeded");
    const auto saved = Read("TF_test.json");
    EXPECT_DOUBLE_EQ(saved["simulationDefaults"]["damping"].get<double>(), 0.02);
    EXPECT_EQ(saved["simulationDefaults"]["iterations"], 8);
    EXPECT_EQ(saved["simulationDefaults"]["gravity"], Json::array({0.0, -500.0, 0.0}));
    EXPECT_FALSE(saved.contains("simulation"));

    // 越界值必须由资产校验拒绝，不能写进文件。
    const auto invalid = Modify("tressfx", "TF_test.json", 3, Json::array({
        {{"field", "simulationDefaults.damping"}, {"value", 2.0}}}));
    EXPECT_NE(invalid["status"], "succeeded");
    EXPECT_DOUBLE_EQ(Read("TF_test.json")["simulationDefaults"]["damping"].get<double>(), 0.02);
}

TEST_F(AssetDocumentTest, TressfxAuthorProjectionPreservesReadOnlySimulationGate)
{
    std::ofstream(root / "guides.tfx") << "Document-only source";
    const std::vector<std::pair<Json, bool>> cases{
        {nullptr, false},
        {Json::object(), false},
        {{{"backend", "gpu"}}, true},
        {{{"enabled", true}, {"backend", "gpu"}}, true},
        {{{"enabled", false}, {"backend", "gpu"}}, false},
        {{{"enabled", true}, {"backend", "cpu"}}, false},
        {{{"enabled", true}}, false}
    };
    for (size_t index = 0; index < cases.size(); ++index)
    {
        SCOPED_TRACE(index);
        const auto path = "TF_gate_" + std::to_string(index) + ".json";
        auto asset = Read("SM_test.json");
        asset["type"] = "tressfx";
        asset["modelDataPath"] = "guides.tfx";
        asset["strandRadius"] = 0.035;
        if (!cases[index].first.is_null()) asset["simulationDefaults"] = cases[index].first;
        Write(path, asset);
        const auto opened = Get("tressfx", path);
        ASSERT_EQ(opened["status"], "succeeded") << opened;
        const auto& projected = opened["payload"]["document"];
        EXPECT_EQ(projected.contains("simulationDefaults"), asset.contains("simulationDefaults"));
        if (asset.contains("simulationDefaults"))
            EXPECT_EQ(projected["simulationDefaults"], asset["simulationDefaults"]);
        EXPECT_EQ(VL::EditorUi::TressfxGpuSimulationDefaults(projected), cases[index].second);
        for (const auto& field : opened["payload"]["capabilities"]["fields"])
        {
            EXPECT_NE(field["field"], "simulationDefaults.enabled");
            EXPECT_NE(field["field"], "simulationDefaults.backend");
        }
        for (const auto& change : Json::array({
            {{"field", "simulationDefaults.enabled"}, {"value", true}},
            {{"field", "simulationDefaults.backend"}, {"value", "gpu"}}}))
            EXPECT_EQ(Modify("tressfx", path, 1, Json::array({change}))["status"], "rejected");
        const auto modified = Modify("tressfx", path, 1, Json::array({
            {{"field", "simulationDefaults.damping"}, {"value", 0.02}}}));
        ASSERT_EQ(modified["status"], "succeeded") << modified;
        EXPECT_EQ(VL::EditorUi::TressfxGpuSimulationDefaults(modified["payload"]["document"]),
            cases[index].second);
    }
}

TEST_F(AssetDocumentTest, LegacySmTressfxDocumentsRequireExplicitTfMigration)
{
    auto mesh = Read("SM_test.json");
    mesh["type"] = "tressfx";
    mesh["modelDataPath"] = "guides.tfx";
    mesh["strandRadius"] = 0.035;
    Write("SM_test.json", mesh);
    std::ofstream(root / "guides.tfx") << "Content preflight is separate";
    const auto rejected = Get("tressfx", "SM_test.json");
    EXPECT_EQ(rejected["status"], "failed");
    EXPECT_NE(rejected["message"].get<std::string>().find("require TF_*.json"), std::string::npos);
    EXPECT_EQ(Get("mesh", "SM_test.json")["status"], "failed");
    EXPECT_EQ(Read("SM_test.json")["strandRadius"], 0.035);
    EXPECT_EQ(Read("SM_test.json")["type"], "tressfx");
}

TEST_F(AssetDocumentTest, TressfxCommandsRoundTripAllOperationsThroughTheSharedCodec)
{
    const std::vector<std::string> operations{
        "get_document", "get_capabilities", "modify", "validate", "save", "revert", "reload"};
    for (const auto& operation : operations)
    {
        SCOPED_TRACE(operation);
        auto wire = Request("tressfx." + operation, "TF_hair.json");
        if (operation == "modify" || operation == "save" || operation == "revert" || operation == "reload")
            wire["expectedDocumentRevision"] = 7;
        if (operation == "modify")
            wire["payload"]["changes"] = Json::array({{{"field", "simulationDefaults.damping"}, {"value", 0.02}}});
        const auto decoded = EditorCommandJsonCodec::DecodeCommand(wire);
        EXPECT_EQ(GetEditorAssetDocumentKind(decoded.type), EditorAssetDocumentKind::Tressfx);
        EXPECT_EQ(EditorCommandJsonCodec::DecodeCommand(
            EditorCommandJsonCodec::Encode(decoded)).type, decoded.type);
        if (wire.contains("expectedDocumentRevision"))
        {
            wire.erase("expectedDocumentRevision");
            EXPECT_THROW(EditorCommandJsonCodec::DecodeCommand(wire), EditorCommandJsonCodecError);
        }
    }
}

TEST_F(AssetDocumentTest, TressfxAndMeshOwnersRejectEachOthersAssetTypes)
{
    auto hair = Read("SM_test.json");
    hair["type"] = "tressfx";
    hair["modelDataPath"] = "guides.tfx";
    hair["strandRadius"] = 0.035;
    Write("TF_hair.json", hair);
    Write("SM_legacy_hair.json", hair);
    EXPECT_NE(Get("mesh", "TF_hair.json")["status"], "succeeded");
    EXPECT_NE(Get("mesh", "SM_legacy_hair.json")["status"], "succeeded");
    EXPECT_NE(Get("tressfx", "SM_test.json")["status"], "succeeded");
    Write("other_hair.json", hair);
    EXPECT_NE(Get("tressfx", "other_hair.json")["status"], "succeeded");
    EXPECT_EQ(Get("tressfx", "TF_hair.json")["status"], "succeeded");
    const auto wrongPrefix = Get("tressfx", "SM_legacy_hair.json");
    EXPECT_EQ(wrongPrefix["status"], "failed");
    EXPECT_NE(wrongPrefix["message"].get<std::string>().find("require TF_*.json"), std::string::npos);
}

TEST_F(AssetDocumentTest, TressfxCapabilitiesAreIndependentAndSlotsUseStableSelectors)
{
    auto hair = Read("SM_test.json");
    hair["type"] = "tressfx";
    hair["modelDataPath"] = "guides.tfx";
    hair["strandRadius"] = 0.035;
    Write("TF_hair.json", hair);
    std::ofstream(root / "guides.tfx") << "Content preflight is separate";
    Write("MI_other.json", {{"type", "materialInstance"}});
    const auto result = Get("tressfx", "TF_hair.json");
    ASSERT_EQ(result["status"], "succeeded") << result;
    const auto fields = result["payload"]["capabilities"]["fields"].dump();
    EXPECT_NE(fields.find("simulationDefaults.damping"), std::string::npos);
    EXPECT_EQ(fields.find("importOptions"), std::string::npos);
    const auto meshFields = Get("mesh", "SM_test.json")["payload"]["capabilities"]["fields"].dump();
    EXPECT_EQ(meshFields.find("simulation."), std::string::npos);
    const auto changed = Modify("tressfx", "TF_hair.json", 1, Json::array({
        {{"field", "materialInstancePath"}, {"slotIndex", 0}, {"expectedSlotName", "Default"},
            {"value", "MI_other.json"}}}));
    ASSERT_EQ(changed["status"], "succeeded") << changed;
    for (const auto& operation : {"save", "revert", "reload"})
    {
        SCOPED_TRACE(operation);
        EXPECT_NE(Run(Request(std::string("tressfx.") + operation, "TF_hair.json", 1))["status"], "succeeded");
    }
    EXPECT_NE(Modify("tressfx", "TF_hair.json", 1, Json::array({
        {{"field", "name"}, {"value", "Stale"}}}))["status"], "succeeded");
    EXPECT_EQ(Run(Request("tressfx.revert", "TF_hair.json", 2))["status"], "succeeded");
    EXPECT_EQ(Run(Request("tressfx.reload", "TF_hair.json", 3))["status"], "succeeded");
    EXPECT_EQ(Run(Request("tressfx.get_capabilities", "TF_hair.json"))["status"], "succeeded");
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

TEST_F(AssetDocumentTest, StaticMeshAcceptsSmoothNormalsSectionsByMaterialSlot)
{
    auto mesh = Read("SM_test.json");
    mesh["smoothNormalsSections"] = {"Default"};
    const MeshAssetBuildPlan plan =
        MeshAssetValidator::BuildLoadPlan("SM_test.json", mesh);
    ASSERT_EQ(plan.smoothNormalsSections.size(), 1u);
    EXPECT_EQ(plan.smoothNormalsSections.front(), "Default");
    // 平滑配置只影响运行时 renderable key，不改变几何导入缓存身份。
    EXPECT_EQ(plan.modelCacheKey, "triangle.obj");
}

TEST_F(AssetDocumentTest, SmoothNormalsSectionsRejectUnknownMaterialSlot)
{
    auto mesh = Read("SM_test.json");
    mesh["smoothNormalsSections"] = {"Missing"};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_test.json", mesh), std::runtime_error);
}

TEST_F(AssetDocumentTest, SkeletalMeshRejectsSmoothNormalsSections)
{
    auto mesh = Read("SM_test.json");
    mesh["skinning"] = {{"skeleton", "Skeletons/SK_test.json"}, {"clips", Json::array()}};
    mesh["smoothNormalsSections"] = {"Default"};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_test.json", mesh), std::runtime_error);
}

TEST_F(AssetDocumentTest, StaticMeshAcceptsOutlineMaskSectionsByMaterialSlot)
{
    auto mesh = Read("SM_test.json");
    mesh["outlineMaskSections"] = {"Default"};
    const MeshAssetBuildPlan plan =
        MeshAssetValidator::BuildLoadPlan("SM_test.json", mesh);
    ASSERT_EQ(plan.outlineMaskSections.size(), 1u);
    EXPECT_EQ(plan.outlineMaskSections.front(), "Default");
}

TEST_F(AssetDocumentTest, OutlineMaskSectionsRejectUnknownMaterialSlot)
{
    auto mesh = Read("SM_test.json");
    mesh["outlineMaskSections"] = {"Missing"};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_test.json", mesh), std::runtime_error);
}

TEST_F(AssetDocumentTest, SkeletalMeshRejectsOutlineMaskSections)
{
    auto mesh = Read("SM_test.json");
    mesh["skinning"] = {{"skeleton", "Skeletons/SK_test.json"}, {"clips", Json::array()}};
    mesh["outlineMaskSections"] = {"Default"};
    EXPECT_THROW(MeshAssetValidator::BuildLoadPlan("SM_test.json", mesh), std::runtime_error);
}

Json BlueprintAsset()
{
    return {{"type", "blueprint"}, {"name", "Actor"}, {"class", "Gameplay.RotatorBlueprint"},
        {"properties", {{"DegreesPerSecond", 45}}}, {"instanceEditable", Json::array()},
        {"components", Json::array({{{"id", "pivot"}, {"type", "scene"}}})}};
}

Json TerrainScene()
{
    return {{"name", "Terrain Scene"}, {"type", "scene"}, {"description", "Terrain asset API test"},
        {"worldUnit", "cm"}, {"objects", Json::array({
            {{"name", "Ground"}, {"type", "terrain"},
                {"terrainPath", "Terrains/TR_ground.json"}, {"pageOrigin", {0, 0}},
                {"pageTilesPerSide", 8}, {"position", {0, 0, 0}},
                {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}},
            {{"name", "Model"}, {"type", "mesh"}, {"modelPath", "SM_test.json"},
                {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}},
            {{"name", "Camera"}, {"type", "camera"}, {"position", {0, 0, 300}},
                {"rotation", {0, 0, 0}}, {"scale", {1, 1, 1}}, {"fov", 60},
                {"near_clip", 1}, {"far_clip", 1000},
                {"exposure", DescribeCameraExposure(CameraExposure{})}}
        })}};
}

const Json* FindTerrainField(const Json& capabilities, const std::string& field)
{
    for (const auto& object : capabilities.at("objects"))
        if (object.at("objectIdentity") == "terrain|Ground")
            for (const auto& description : object.at("fields"))
                if (description.at("field") == field) return &description;
    return nullptr;
}

TEST_F(AssetDocumentTest, TerrainScopePublishesPlacementCapabilities)
{
    const Json scene = TerrainScene();
    const auto capabilities = DescribeAssetCapabilities(EditorAssetDocumentKind::Scene, scene);
    const auto* path = FindTerrainField(capabilities, "terrainPath");
    const auto* origin = FindTerrainField(capabilities, "pageOrigin");
    const auto* tiles = FindTerrainField(capabilities, "pageTilesPerSide");
    ASSERT_NE(path, nullptr);
    ASSERT_NE(origin, nullptr);
    ASSERT_NE(tiles, nullptr);
    EXPECT_EQ(path->at("type"), "string");
    EXPECT_EQ(path->at("unit"), "asset_path");
    EXPECT_FALSE(path->at("resettable").get<bool>());
    EXPECT_EQ(origin->at("type"), "vec2_int");
    EXPECT_TRUE(origin->at("resettable").get<bool>());
    EXPECT_EQ(tiles->at("type"), "integer");
    EXPECT_TRUE(tiles->at("resettable").get<bool>());
    EXPECT_EQ(FindTerrainField(capabilities, "terrainPath")->at("field"), "terrainPath");
}

TEST_F(AssetDocumentTest, TerrainVec2ChangesRoundTripThroughWireCodec)
{
    const std::vector<EditorAssetFieldChange> changes{
        {"pageOrigin", "terrain|Ground", std::nullopt, std::nullopt,
            std::nullopt, false,
            std::array<int64_t, 2>{12, 20}},
        {"pageTilesPerSide", "terrain|Ground", std::nullopt, std::nullopt,
            std::nullopt, false,
            static_cast<int64_t>(6)}};
    const Json wire = EncodeAssetChanges(changes, EditorAssetDocumentKind::Scene);
    const auto decoded = DecodeAssetChanges(wire, EditorAssetDocumentKind::Scene);
    ASSERT_EQ(decoded.size(), 2);
    EXPECT_EQ((std::get<std::array<int64_t, 2>>(decoded[0].value)),
        (std::array<int64_t, 2>{12, 20}));
    EXPECT_EQ(std::get<int64_t>(decoded[1].value), 6);
}

TEST_F(AssetDocumentTest, TerrainApplyWritesPlacementAndReferenceOnly)
{
    Json candidate = TerrainScene();
    const std::vector<EditorAssetFieldChange> changes{
        {"terrainPath", "terrain|Ground", std::nullopt, std::nullopt,
            std::nullopt, false,
            std::string("Terrains/TR_other.json")},
        {"pageOrigin", "terrain|Ground", std::nullopt, std::nullopt,
            std::nullopt, false,
            std::array<int64_t, 2>{8, 0}},
        {"pageTilesPerSide", "terrain|Ground", std::nullopt, std::nullopt,
            std::nullopt, false,
            static_cast<int64_t>(6)}};
    const auto changed = ApplyAssetChanges(candidate, EditorAssetDocumentKind::Scene, changes);
    EXPECT_EQ(changed.size(), 3);
    EXPECT_EQ(candidate["objects"][0]["terrainPath"], "Terrains/TR_other.json");
    EXPECT_EQ(candidate["objects"][0]["pageOrigin"], Json::array({8, 0}));
    EXPECT_EQ(candidate["objects"][0]["pageTilesPerSide"], 6);
    const auto projected = ProjectEditableAsset(EditorAssetDocumentKind::Scene, candidate);
    EXPECT_EQ(projected["objects"][0]["terrainPath"], "Terrains/TR_other.json");
    EXPECT_EQ(projected["objects"][0]["pageOrigin"], Json::array({8, 0}));
    EXPECT_EQ(projected["objects"][0]["pageTilesPerSide"], 6);
}

TEST_F(AssetDocumentTest, TerrainRejectsInvalidVec2AndNonTerrainScope)
{
    for (const Json& value : {Json::array({1}), Json::array({1, 2, 3}),
        Json::array({1, "x"}), Json::array({1.0, 2})})
    {
        EXPECT_THROW(DecodeAssetChanges(Json::array({{{"objectIdentity", "terrain|Ground"},
            {"field", "pageOrigin"}, {"value", value}}}), EditorAssetDocumentKind::Scene),
            std::invalid_argument);
    }
    EXPECT_THROW(EncodeAssetChanges({{"terrainPath", "mesh|Model", std::nullopt,
        std::nullopt, std::nullopt, false,
        std::string("Terrains/TR_ground.json")}},
        EditorAssetDocumentKind::Scene), std::invalid_argument);
}

TEST_F(AssetDocumentTest, TerrainServiceKeepsIntegersAndValidatesTheTerrainReference)
{
    std::filesystem::create_directories(root / "Terrains");
    Write("Terrains/TR_ground.json", {{"name", "TR_ground"}, {"type", "terrain"}});
    Write("SC_terrain.json", TerrainScene());
    const auto opened = Get("scene", "SC_terrain.json");
    ASSERT_EQ(opened["status"], "succeeded") << opened;

    const auto changed = Modify("scene", "SC_terrain.json", 1, Json::array({
        {{"objectIdentity", "terrain|Ground"}, {"field", "pageOrigin"}, {"value", {8, 0}}},
        {{"objectIdentity", "terrain|Ground"}, {"field", "pageTilesPerSide"}, {"value", 6}}}));
    ASSERT_EQ(changed["status"], "succeeded") << changed;
    const auto& document = changed["payload"]["document"];
    EXPECT_TRUE(document["objects"][0]["pageOrigin"][0].is_number_integer());
    EXPECT_TRUE(document["objects"][0]["pageOrigin"][1].is_number_integer());
    EXPECT_TRUE(document["objects"][0]["pageTilesPerSide"].is_number_integer());
    EXPECT_EQ(document["objects"][0]["pageOrigin"], Json::array({8, 0}));
    EXPECT_EQ(document["objects"][0]["pageTilesPerSide"], 6);
    // 修改只进入草稿，未 save 前不写源文件。
    EXPECT_EQ(Read("SC_terrain.json")["objects"][0]["pageOrigin"], Json::array({0, 0}));

    const auto missing = Modify("scene", "SC_terrain.json",
        Get("scene", "SC_terrain.json")["documentRevision"], Json::array({
            {{"objectIdentity", "terrain|Ground"}, {"field", "terrainPath"},
                {"value", "Terrains/TR_missing.json"}}}));
    EXPECT_EQ(missing["status"], "rejected") << missing;
    Write("Terrains/TR_wrong.json", {{"name", "TR_wrong"}, {"type", "mesh"}});
    const auto wrongType = Modify("scene", "SC_terrain.json",
        Get("scene", "SC_terrain.json")["documentRevision"], Json::array({
            {{"objectIdentity", "terrain|Ground"}, {"field", "terrainPath"},
                {"value", "Terrains/TR_wrong.json"}}}));
    EXPECT_EQ(wrongType["status"], "rejected") << wrongType;
}

TEST_F(AssetDocumentTest, TressfxBlueprintModesAndSparseOverridesSaveReloadAndReset)
{
    auto hair = Read("SM_test.json");
    hair["type"] = "tressfx";
    hair["modelDataPath"] = "guides.tfx";
    hair["simulationDefaults"] = {{"enabled", true}, {"backend", "gpu"}, {"damping", 0.035}};
    Write("TF_test.json", hair);
    std::ofstream(root / "guides.tfx") << "Document-only source";
    auto blueprint = BlueprintAsset();
    auto components = Json::array({
        {{"id", "body"}, {"type", "mesh"}, {"modelPath", "SM_test.json"}},
        {{"id", "hair"}, {"type", "tressfx"}, {"parent", "body"},
         {"modelPath", "TF_test.json"}, {"rootBone", "Head_M"}}});
    blueprint["components"] = components;
    Write("BP_test.json", blueprint);
    const auto opened = Get("blueprint", "BP_test.json");
    ASSERT_EQ(opened["status"], "succeeded") << opened;
    uint64_t revision = 1;
    for (const char* mode : {"off", "on", "inherit"})
    {
        components[1]["simulation"] = {{"mode", mode}, {"overrides", {{"damping", 0.02}}}};
        const auto changed = Modify("blueprint", "BP_test.json", revision,
            Json::array({{{"field", "components"}, {"value", components}}}));
        ASSERT_EQ(changed["status"], "succeeded") << changed;
        revision = changed["documentRevision"].get<uint64_t>();
        const auto saved = Run(Request("blueprint.save", "BP_test.json", revision));
        ASSERT_EQ(saved["status"], "succeeded") << saved;
        revision = saved["documentRevision"].get<uint64_t>();
        const auto reloaded = Run(Request("blueprint.reload", "BP_test.json", revision));
        ASSERT_EQ(reloaded["status"], "succeeded") << reloaded;
        revision = reloaded["documentRevision"].get<uint64_t>();
        EXPECT_EQ(reloaded["payload"]["document"]["components"][1]["simulation"],
            components[1]["simulation"]);
        EXPECT_EQ(Read("TF_test.json"), hair) << "component overrides never modify asset defaults";
    }
    components[1].erase("simulation");
    ASSERT_EQ(Modify("blueprint", "BP_test.json", revision,
        Json::array({{{"field", "components"}, {"value", components}}}))["status"], "succeeded");
    const auto reset = Get("blueprint", "BP_test.json");
    const auto saved = Run(Request("blueprint.save", "BP_test.json",
        reset["documentRevision"].get<uint64_t>()));
    ASSERT_EQ(saved["status"], "succeeded") << saved;
    EXPECT_FALSE(Read("BP_test.json")["components"][1].contains("simulation"));
}

TEST_F(AssetDocumentTest, TressfxPlacementProxyReferencesAreValidatedOutsideTheAsset)
{
    auto blueprint = BlueprintAsset();
    blueprint["components"] = Json::array({
        {{"id", "body"}, {"type", "mesh"}, {"modelPath", "SM_test.json"}},
        {{"id", "hair"}, {"type", "tressfx"}, {"parent", "body"}, {"rootBone", "Head_M"},
         {"modelPath", "TF_test.json"},
         {"hairCollision", {{"mesh", {{"path", "missing.obj"}, {"cellSize", 1}, {"margin", 1}}}}}}});
    auto hair = Read("SM_test.json");
    hair["type"] = "tressfx";
    hair["modelDataPath"] = "guides.tfx";
    Write("TF_test.json", hair);
    Write("BP_test.json", blueprint);
    std::ofstream(root / "guides.tfx") << "Document-only source";
    ASSERT_EQ(Get("blueprint", "BP_test.json")["status"], "succeeded");
    EXPECT_NE(Run(Request("blueprint.validate", "BP_test.json"))["status"], "succeeded");
    std::ofstream(root / "missing.obj") << "Proxy references are not imported during validation";
    EXPECT_EQ(Run(Request("blueprint.validate", "BP_test.json"))["status"], "succeeded");
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
    Write("SC_particle_preview.json", scene);
    EXPECT_NE(Get("scene", "SC_model_preview.json")["status"], "succeeded");
    EXPECT_NE(Get("scene", "SC_MODEL_PREVIEW.json")["status"], "succeeded");
    EXPECT_NE(Get("scene", "SC_particle_preview.json")["status"], "succeeded");
    EXPECT_NE(Get("scene", "SC_PARTICLE_PREVIEW.json")["status"], "succeeded");
    EXPECT_EQ(Run(Request("scene.save", "SC_model_preview.json", 1))["errorCode"], "document_not_open");
}

TEST_F(AssetDocumentTest, ParticleCapabilitiesSeparateSystemAndEmitterFields)
{
    Write("PS_test.json", Json{
        {"name", "PS_test"}, {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{{"name", "fire"}, {"maxParticles", 512}, {"spawnRate", 120.0},
                {"position", {5.0, 0.0, 0.0}}},
            Json{{"name", "smoke"}, {"maxParticles", 2048}, {"spawnRate", 30.0}}})}});
    const auto result = Get("particle", "PS_test.json");
    ASSERT_EQ(result["status"], "succeeded") << result;
    const auto& capabilities = result["payload"]["capabilities"];
    std::set<std::string> rootFields;
    for (const auto& field : capabilities["fields"])
        rootFields.insert(field.at("field").get<std::string>());
    EXPECT_EQ(rootFields, (std::set<std::string>{"name", "emitters"}));
    ASSERT_EQ(capabilities["emitters"].size(), 2u);
    for (const auto& emitter : capabilities["emitters"])
    {
        std::set<std::string> fields;
        for (const auto& field : emitter["fields"])
            fields.insert(field.at("field").get<std::string>());
        EXPECT_TRUE(fields.count("name"));
        EXPECT_TRUE(fields.count("position"));
        EXPECT_TRUE(fields.count("maxParticles"));
        EXPECT_TRUE(fields.count("spawnRate"));
        EXPECT_TRUE(fields.count("colorKeys"));
        EXPECT_FALSE(fields.count("emitters"));
    }
    EXPECT_EQ(capabilities["emitters"][0]["name"], "fire");
}

TEST_F(AssetDocumentTest, ParticleImplicitCapabilitiesKeepRootPaths)
{
    Write("PS_legacy.json", Json{
        {"name", "PS_legacy"}, {"type", "particleSystem"},
        {"maxParticles", 512}, {"spawnRate", 120.0}});
    const auto result = Get("particle", "PS_legacy.json");
    ASSERT_EQ(result["status"], "succeeded") << result;
    const auto& capabilities = result["payload"]["capabilities"];
    std::set<std::string> rootFields;
    for (const auto& field : capabilities["fields"])
        rootFields.insert(field.at("field").get<std::string>());
    EXPECT_TRUE(rootFields.count("name"));
    EXPECT_TRUE(rootFields.count("emitters"));
    EXPECT_TRUE(rootFields.count("spawnRate"));
    EXPECT_FALSE(rootFields.count("position"));
    ASSERT_EQ(capabilities["emitters"].size(), 1u);
    std::set<std::string> emitterFields;
    for (const auto& field : capabilities["emitters"][0]["fields"])
        emitterFields.insert(field.at("field").get<std::string>());
    EXPECT_TRUE(emitterFields.count("name"));
    EXPECT_TRUE(emitterFields.count("spawnRate"));
    EXPECT_FALSE(emitterFields.count("position"));
    EXPECT_FALSE(emitterFields.count("emitters"));
    EXPECT_EQ(Modify("particle", "PS_legacy.json", 1,
        Json::array({{{"field", "spawnRate"}, {"value", 150.0}}}))["status"], "succeeded");
}

TEST_F(AssetDocumentTest, ParticleExplicitRootEmitterFieldsAreRejected)
{
    Write("PS_test.json", Json{
        {"name", "PS_test"}, {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{{"name", "fire"}, {"maxParticles", 512}, {"spawnRate", 120.0}},
            Json{{"name", "smoke"}, {"maxParticles", 2048}, {"spawnRate", 30.0}}})}});
    ASSERT_EQ(Get("particle", "PS_test.json")["status"], "succeeded");
    const auto rejected = Modify("particle", "PS_test.json", 1,
        Json::array({{{"field", "spawnRate"}, {"value", 300.0}}}));
    EXPECT_EQ(rejected["status"], "rejected") << rejected;
    EXPECT_EQ(rejected["errorCode"], "validation_failed");
    EXPECT_NE(rejected["message"].get<std::string>().find(
        "requires emitters.<index>.<field>"), std::string::npos);
    EXPECT_EQ(Get("particle", "PS_test.json")["documentRevision"], 1);
    EXPECT_EQ(Modify("particle", "PS_test.json", 1,
        Json::array({{{"field", "name"}, {"value", "PS_renamed"}}}))["status"], "succeeded");
}

TEST_F(AssetDocumentTest, ParticleEmitterIdentityGuardsAndBatchRename)
{
    Write("PS_test.json", Json{
        {"name", "PS_test"}, {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{{"name", "fire"}, {"maxParticles", 512}, {"spawnRate", 120.0}},
            Json{{"name", "smoke"}, {"maxParticles", 2048}, {"spawnRate", 30.0}}})}});
    ASSERT_EQ(Get("particle", "PS_test.json")["status"], "succeeded");
    const auto wrong = Modify("particle", "PS_test.json", 1, Json::array({
        {{"field", "emitters.0.spawnRate"}, {"expectedEmitterName", "smoke"},
         {"value", 200.0}}}));
    EXPECT_EQ(wrong["status"], "rejected") << wrong;
    EXPECT_NE(wrong["message"].get<std::string>().find(
        "identity changed"), std::string::npos);
    const auto right = Modify("particle", "PS_test.json", 1, Json::array({
        {{"field", "emitters.0.spawnRate"}, {"expectedEmitterName", "fire"},
         {"value", 200.0}}}));
    ASSERT_EQ(right["status"], "succeeded") << right;
    // 同一 batch 先改名再改字段：身份仍按 batch 开始前的名字校验。
    const auto batch = Modify("particle", "PS_test.json", 2, Json::array({
        {{"field", "emitters.0.name"}, {"expectedEmitterName", "fire"},
         {"value", "flame"}},
        {{"field", "emitters.0.spawnRate"}, {"expectedEmitterName", "fire"},
         {"value", 250.0}}}));
    ASSERT_EQ(batch["status"], "succeeded") << batch;
    EXPECT_EQ(batch["payload"]["document"]["emitters"][0]["name"], "flame");
    EXPECT_EQ(batch["payload"]["document"]["emitters"][0]["spawnRate"], 250.0);
}

TEST_F(AssetDocumentTest, ParticleCloseRequiresCleanDocumentAndErasesOwner)
{
    Write("PS_test.json", Json{
        {"name", "PS_test"}, {"type", "particleSystem"},
        {"emitters", Json::array({
            Json{{"name", "fire"}, {"maxParticles", 512}, {"spawnRate", 120.0}}})}});
    ASSERT_EQ(Get("particle", "PS_test.json")["status"], "succeeded");
    ASSERT_EQ(Run(Request("particle.close", "PS_test.json"))["status"], "succeeded");
    EXPECT_EQ(Run(Request("particle.validate", "PS_test.json"))["errorCode"], "document_not_open");
    ASSERT_EQ(Get("particle", "PS_test.json")["status"], "succeeded");
    ASSERT_EQ(Modify("particle", "PS_test.json", 1, Json::array({
        {{"field", "emitters.0.spawnRate"}, {"value", 150.0}}}))["status"], "succeeded");
    const auto dirtyClose = Run(Request("particle.close", "PS_test.json"));
    EXPECT_EQ(dirtyClose["status"], "rejected") << dirtyClose;
    EXPECT_EQ(dirtyClose["errorCode"], "document_dirty");
    ASSERT_EQ(Run(Request("particle.revert", "PS_test.json", 2))["status"], "succeeded");
    ASSERT_EQ(Run(Request("particle.close", "PS_test.json"))["status"], "succeeded");
    EXPECT_EQ(Run(Request("particle.validate", "PS_test.json"))["errorCode"], "document_not_open");
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

namespace
{
Json TerrainAssetDocument()
{
    return {
        {"name", "TR_test"},
        {"type", "terrain"},
        {"description", "Terrain asset API test"},
        {"heightmapPath", "image.png"},
        {"materialInstancePath", "MI_test.json"},
        {"size", {1000.0, 800.0}},
        {"heightScale", 120.0},
        {"heightOffset", 5.0},
        {"gridResolution", 16},
        {"chunkQuads", 8},
        {"lodLevels", 2},
        {"lodDistances", {700.0}},
        {"pageOrigin", {0, 0}},
        {"pageTilesPerSide", 8},
        {"layers", Json::array({
            {{"name", "Grass"}, {"heightRange", {0.0, 1.0}}, {"slopeRange", {0.0, 90.0}}},
            {{"name", "Rock"}, {"heightRange", {0.5, 1.0}}, {"slopeRange", {0.0, 90.0}}}
        })}};
}

const Json* TerrainCapability(const Json& capabilities, const std::string& field)
{
    for (const auto& entry : capabilities.at("fields"))
        if (entry.at("field") == field) return &entry;
    return nullptr;
}
} // namespace

TEST_F(AssetDocumentTest, TerrainAssetCapabilitiesPublishTypedFieldsAndLayerIdentity)
{
    Write("TR_test.json", TerrainAssetDocument());
    const auto result = Get("terrain", "TR_test.json");
    ASSERT_EQ(result["status"], "succeeded") << result;
    const auto& capabilities = result["payload"]["capabilities"];
    ASSERT_NE(TerrainCapability(capabilities, "size"), nullptr);
    EXPECT_EQ(TerrainCapability(capabilities, "size")->at("type"), "vec2");
    EXPECT_EQ(TerrainCapability(capabilities, "pageOrigin")->at("type"), "vec2_int");
    EXPECT_EQ(TerrainCapability(capabilities, "lodDistances")->at("type"), "number_array");
    ASSERT_NE(TerrainCapability(capabilities, "paintPath"), nullptr);
    EXPECT_EQ(TerrainCapability(capabilities, "paintPath")->at("type"), "string");
    ASSERT_EQ(capabilities["layers"].size(), 2);
    EXPECT_EQ(capabilities["layers"][1]["expectedLayerName"], "Rock");
    bool hasNoise = false;
    for (const auto& field : capabilities["layers"][1]["fields"])
        hasNoise = hasNoise || field["field"] == "noise";
    EXPECT_TRUE(hasNoise);
    EXPECT_EQ(result["payload"]["document"]["layers"][1]["name"], "Rock");
    EXPECT_FALSE(result["payload"]["heightmapPreviewPath"].get<std::string>().empty());
}

TEST_F(AssetDocumentTest, TerrainLayerChangesCarryIdentityAndOnlyTouchTheNamedLayer)
{
    Write("TR_test.json", TerrainAssetDocument());
    Get("terrain", "TR_test.json");
    const auto result = Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "noise"}, {"layerIndex", 1}, {"expectedLayerName", "Rock"}, {"value", 0.25}},
        {{"field", "uvScale"}, {"layerIndex", 1}, {"expectedLayerName", "Rock"}, {"value", 3.0}}}));
    ASSERT_EQ(result["status"], "succeeded") << result;
    EXPECT_EQ(result["payload"]["document"]["layers"][1]["noise"], 0.25);
    EXPECT_EQ(result["payload"]["document"]["layers"][1]["uvScale"], 3.0);
    EXPECT_FALSE(result["payload"]["document"]["layers"][0].contains("noise"));
    EXPECT_EQ(Modify("terrain", "TR_test.json", 2, Json::array({
        {{"field", "noise"}, {"layerIndex", 1}, {"expectedLayerName", "Grass"}, {"value", 0.1}}}))["status"],
        "rejected");
    EXPECT_EQ(Modify("terrain", "TR_test.json", 2, Json::array({
        {{"field", "noise"}, {"layerIndex", 9}, {"expectedLayerName", "Rock"}, {"value", 0.1}}}))["status"],
        "rejected");
}

TEST_F(AssetDocumentTest, TerrainVec2IntegerAndNumberArrayRoundTripThroughTheWireCodec)
{
    Write("TR_test.json", TerrainAssetDocument());
    Get("terrain", "TR_test.json");
    auto request = Request("terrain.modify", "TR_test.json", 1, Json::array({
        {{"field", "pageOrigin"}, {"value", {2, 3}}},
        {{"field", "lodDistances"}, {"value", {400.0, 900.0}}},
        {{"field", "lodLevels"}, {"value", 3}}}));
    const auto command = EditorCommandJsonCodec::DecodeCommand(request);
    const auto& modify = std::get<ModifyAssetDocumentPayload>(command.payload);
    ASSERT_EQ(modify.changes.size(), 3);
    EXPECT_EQ((std::get<std::array<int64_t, 2>>(modify.changes[0].value))[0], 2);
    EXPECT_EQ(std::get<std::vector<double>>(modify.changes[1].value).size(), 2);
    EXPECT_EQ(EditorCommandJsonCodec::Encode(command), request);
    const auto result = Run(request);
    ASSERT_EQ(result["status"], "succeeded") << result;
    EXPECT_EQ(result["payload"]["document"]["pageOrigin"], Json::array({2, 3}));
    EXPECT_EQ(result["payload"]["document"]["lodDistances"], Json::array({400.0, 900.0}));
}

TEST_F(AssetDocumentTest, TerrainSaveWritesTheDraftAndReloadReadsItBack)
{
    Write("TR_test.json", TerrainAssetDocument());
    Get("terrain", "TR_test.json");
    ASSERT_EQ(Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "heightScale"}, {"value", 250.0}},
        {{"field", "layers"}, {"value", Json::array({
            {{"name", "Sand"}, {"heightRange", {0.0, 0.4}}, {"slopeRange", {0.0, 30.0}}}
        })}}}))["status"], "succeeded");
    ASSERT_EQ(Run(Request("terrain.save", "TR_test.json", 2))["status"], "succeeded");
    EXPECT_EQ(Read("TR_test.json")["heightScale"], 250.0);
    EXPECT_EQ(Read("TR_test.json")["layers"].size(), 1);
    const auto reloaded = Get("terrain", "TR_test.json");
    ASSERT_EQ(reloaded["status"], "succeeded") << reloaded;
    EXPECT_EQ(reloaded["payload"]["document"]["layers"][0]["name"], "Sand");
}

TEST_F(AssetDocumentTest, TerrainPaintPathRoundTripsWithoutRequiringTheFileToExist)
{
    Write("TR_test.json", TerrainAssetDocument());
    Get("terrain", "TR_test.json");
    const auto modified = Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "paintPath"}, {"value", "paint.vlpaint"}}}));
    ASSERT_EQ(modified["status"], "succeeded") << modified;
    EXPECT_EQ(modified["payload"]["document"]["paintPath"], "paint.vlpaint");
    ASSERT_FALSE(std::filesystem::exists(root / "paint.vlpaint"));

    ASSERT_EQ(Run(Request("terrain.save", "TR_test.json", 2))["status"], "succeeded");
    EXPECT_EQ(Read("TR_test.json")["paintPath"], "paint.vlpaint");
    const auto reloaded = Get("terrain", "TR_test.json");
    ASSERT_EQ(reloaded["status"], "succeeded") << reloaded;
    EXPECT_EQ(reloaded["payload"]["document"]["paintPath"], "paint.vlpaint");
}

TEST_F(AssetDocumentTest, TerrainValidationRejectsWrongReferencesAndLayerRanges)
{
    Write("MI_bad.json", {{"name", "Bad"}, {"type", "texture"}});
    Write("TR_test.json", TerrainAssetDocument());
    Get("terrain", "TR_test.json");
    EXPECT_EQ(Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "materialInstancePath"}, {"value", "MI_bad.json"}}}))["status"], "rejected");
    EXPECT_EQ(Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "heightRange"}, {"layerIndex", 1}, {"expectedLayerName", "Rock"},
            {"value", {0.8, 0.2}}}}))["status"], "rejected");
    EXPECT_EQ(Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "heightmapPath"}, {"value", "missing.png"}}}))["status"], "rejected");
    EXPECT_EQ(Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "paintPath"}, {"value", "../paint.vlpaint"}}}))["status"], "rejected");
    EXPECT_EQ(Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "paintPath"}, {"value", "C:/paint.vlpaint"}}}))["status"], "rejected");
    EXPECT_EQ(Modify("terrain", "TR_test.json", 1, Json::array({
        {{"field", "paintPath"}, {"value", "paint.bin"}}}))["status"], "rejected");
}

TEST_F(AssetDocumentTest, TerrainOpenResolvesReferencesInsteadOfPublishingACleanMissingAsset)
{
    auto missing = TerrainAssetDocument();
    missing["heightmapPath"] = "missing.png";
    Write("TR_missing.json", missing);
    EXPECT_NE(Get("terrain", "TR_missing.json")["status"], "succeeded");

    Write("MI_bad.json", {{"name", "Bad"}, {"type", "texture"}});
    auto wrongMi = TerrainAssetDocument();
    wrongMi["materialInstancePath"] = "MI_bad.json";
    Write("TR_wrong_mi.json", wrongMi);
    EXPECT_NE(Get("terrain", "TR_wrong_mi.json")["status"], "succeeded");
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

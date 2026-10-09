#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include "core/groomPfm.h"
#include "render/offline/offlineColorImage.h"
#include "render/offline/offlineSceneAdapter.h"

namespace
{
VL::OfflineWorldAssets MakeAssets()
{
    VL::OfflineWorldAssets assets;
    assets.worldGeneration = 7;
    assets.scenePath = "Maps/SC_offline/SC_offline.json";
    auto imported = std::make_shared<MeshAssetImportResult>();
    imported->buildPlan.assetType = "mesh";
    imported->buildPlan.meshAssetPath = "Maps/SC_offline/Meshes/SM_cloth.json";
    MeshSection section;
    section.sectionName = "cloth";
    section.vertices.emplace_back(Eigen::Vector3f(0, 0, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(0, 0));
    section.vertices.emplace_back(Eigen::Vector3f(1, 0, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(1, 0));
    section.vertices.emplace_back(Eigen::Vector3f(0, 1, 0),
                                  Eigen::Vector3f(0, 0, 1),
                                  Eigen::Vector4f::Ones(), Eigen::Vector2f(0, 1));
    section.indices = {0, 1, 2};
    imported->modelResource.sections.push_back(std::move(section));
    assets.meshImports.emplace(imported->buildPlan.meshAssetPath, imported);
    assets.sections.emplace("mesh|0|cloth",
        VL::OfflineWorldAssets::Section{imported, 0});
    assets.effectiveMaterialInstances.emplace(
        "Maps/SC_offline/Materials/MI_cloth.json",
        nlohmann::json{
            {"offlineModel", "Lambert"},
            {"renderStates", {{"renderMode", "Opaque"}, {"cullMode", "None"}}},
            {"macros", nlohmann::json::object()},
            {"textures", nlohmann::json::object()},
            {"parameters", {{"u_baseColor", {0.2, 0.4, 0.6}}}}});
    return assets;
}

VL::WorldSnapshot MakeSnapshot()
{
    VL::WorldSnapshot snapshot;
    snapshot.worldGeneration = 7;
    snapshot.environment.type = VL::EnvironmentType::None;
    snapshot.camera.position = Eigen::Vector3f(0, 0, 2);
    snapshot.camera.forward = Eigen::Vector3f(0, 0, -1);
    snapshot.meshDraws.emplace_back();
    snapshot.meshDraws.back().mesh.key = "mesh|0|cloth";
    snapshot.meshDraws.back().materialInstance.key =
        "Maps/SC_offline/Materials/MI_cloth.json";
    snapshot.meshDraws.back().debugName = "cloth::section";
    return snapshot;
}

VL::OfflineRenderSettings Settings()
{
    return {8, 6, 2, 0, 1, 3};
}

std::string GroomPfm(float firstX, float secondX)
{
    std::string bytes = "PF\n2 1\n-1.0\n";
    for (float x : {firstX, secondX})
        for (float channel : {x, 0.0f, 1.0f})
            bytes.append(reinterpret_cast<const char*>(&channel), sizeof(channel));
    return bytes;
}

TEST(OfflineSceneAdapter, FreezesImportedSectionAndEffectiveLambert)
{
    const auto assets = MakeAssets();
    const auto snapshot = MakeSnapshot();
    auto result = VL::BuildOfflineRenderInput(snapshot, assets, Settings());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const auto& input = result.Value();
    ASSERT_EQ(input.scene.instances.size(), 1u);
    ASSERT_EQ(input.instanceMeshes.size(), 1u);
    EXPECT_EQ(input.scene.instances[0].mesh, input.instanceMeshes[0].get());
    EXPECT_DOUBLE_EQ(input.scene.instances[0].baseColor.y, 0.4);
    EXPECT_DOUBLE_EQ(input.cameraClipNear, snapshot.camera.clipNear);
    EXPECT_DOUBLE_EQ(input.cameraClipFar, snapshot.camera.clipFar);
    EXPECT_EQ(input.scene.seed, 3u);
}

TEST(OfflineSceneAdapter, CarriesFrozenCameraClipThroughPreflight)
{
    auto snapshot = MakeSnapshot();
    snapshot.camera.clipNear = 0.75f;
    snapshot.camera.clipFar = 12.0f;
    const auto assets = MakeAssets();
    ASSERT_TRUE(VL::PreflightOfflineRenderInput(snapshot, assets, Settings()).IsSuccess());
    const auto result = VL::BuildOfflineRenderInput(snapshot, assets, Settings());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    EXPECT_DOUBLE_EQ(result.Value().cameraClipNear, 0.75);
    EXPECT_DOUBLE_EQ(result.Value().cameraClipFar, 12.0);
}

TEST(OfflineSceneAdapter, RejectsInvalidCameraClipBeforeBuildingGeometry)
{
    const auto assets = MakeAssets();
    for (const auto [nearClip, farClip] :
         {std::pair{0.0f, 2.0f}, std::pair{2.0f, 1.0f},
          std::pair{1.0f, 1.0f},
          std::pair{1.0f, std::numeric_limits<float>::infinity()},
          std::pair{std::numeric_limits<float>::quiet_NaN(), 2.0f}})
    {
        auto snapshot = MakeSnapshot();
        snapshot.camera.clipNear = nearClip;
        snapshot.camera.clipFar = farClip;
        const auto result = VL::PreflightOfflineRenderInput(snapshot, assets, Settings());
        ASSERT_TRUE(result.IsFailure());
        EXPECT_EQ(result.Error().code, "Offline.InvalidCamera");
    }
}

TEST(OfflineSceneAdapter, PreflightDoesNotBuildGeometry)
{
    const auto assets = MakeAssets();
    const auto snapshot = MakeSnapshot();
    auto result = VL::PreflightOfflineRenderInput(snapshot, assets, Settings());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    auto prepared = VL::BuildOfflineRenderInput(snapshot, assets, Settings());
    ASSERT_TRUE(prepared.IsSuccess()) << prepared.Error().message;
    EXPECT_FALSE(prepared.Value().instanceMeshes[0]->nodes.empty());
}

TEST(OfflineSceneAdapter, PreflightRejectsBadIndicesWithObjectIdentity)
{
    auto assets = MakeAssets();
    auto imported = std::make_shared<MeshAssetImportResult>(
        *assets.meshImports.begin()->second);
    imported->modelResource.sections[0].indices[2] = 99;
    assets.sections.begin()->second.imported = imported;
    auto result = VL::PreflightOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().sourceNode, "cloth::section");
}

TEST(OfflineSceneAdapter, UnknownMaterialReportsObjectAndAsset)
{
    auto assets = MakeAssets();
    assets.effectiveMaterialInstances.begin()->second.erase("offlineModel");
    auto result = VL::BuildOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().sourcePath,
              "Maps/SC_offline/Materials/MI_cloth.json");
    EXPECT_EQ(result.Error().sourceNode, "cloth::section");
}

TEST(OfflineSceneAdapter, AshikhminVelvetFreezesProfileAndRejectsInvalidSigma)
{
    auto assets = MakeAssets();
    auto& material = assets.effectiveMaterialInstances.begin()->second;
    material["offlineModel"] = "AshikhminVelvet";
    material["parameters"] = {
        {"u_sigma", 0.5}, {"u_f0", 0.05},
        {"u_baseColor", {0.35, 0.015, 0.025, 1.0}}};
    ASSERT_TRUE(VL::PreflightOfflineRenderInput(
        MakeSnapshot(), assets, Settings()).IsSuccess());
    auto result = VL::BuildOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    ASSERT_EQ(result.Value().instanceVelvets.size(), 1u);
    EXPECT_EQ(result.Value().scene.instances[0].velvet,
              result.Value().instanceVelvets[0].get());
    material["parameters"]["u_sigma"] = 0.0;
    result = VL::BuildOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Offline.PreflightFailed");
}

TEST(OfflineSceneAdapter, ActiveEnvironmentIsRejectedBeforeGeometry)
{
    auto snapshot = MakeSnapshot();
    snapshot.environment.type = VL::EnvironmentType::ProceduralSky;
    auto result = VL::BuildOfflineRenderInput(snapshot, MakeAssets(), Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Offline.EnvironmentUnsupported");
}

VL::OfflineWorldAssets MakeSatinAssets()
{
    auto assets = MakeAssets();
    auto& material = assets.effectiveMaterialInstances.begin()->second;
    material["offlineModel"] = "AshikhminSatin";
    material["parameters"] = {
        {"u_sigmaFlat", 0.1}, {"u_sigmaEnds", 0.3}, {"u_flatWeight", 0.7},
        {"u_f0", 0.05}, {"u_baseColor", {0.002, 0.002, 0.002, 1.0}},
        {"u_fiberRotationDegrees", 30.0}};
    return assets;
}

TEST(OfflineSceneAdapter, AshikhminSatinFreezesMaterialAndFiberDirection)
{
    auto assets = MakeSatinAssets();
    ASSERT_TRUE(VL::PreflightOfflineRenderInput(
        MakeSnapshot(), assets, Settings()).IsSuccess());
    const auto result = VL::BuildOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const auto& input = result.Value();
    ASSERT_EQ(input.instanceSatins.size(), 1u);
    EXPECT_EQ(input.scene.instances[0].model, VL::Offline::SurfaceModel::AshikhminSatin);
    EXPECT_EQ(input.scene.instances[0].satin, input.instanceSatins[0].get());
    EXPECT_DOUBLE_EQ(input.scene.instances[0].fiberRotationDegrees, 30.0);
    assets.effectiveMaterialInstances.begin()->second["parameters"]["u_flatWeight"] = 0.2;
    EXPECT_DOUBLE_EQ(input.instanceSatins[0]->flatWeight, 0.7);
}

TEST(OfflineSceneAdapter, AshikhminSatinRejectsInvalidParametersDuringPreflight)
{
    for (const auto& [name, invalid] :
         {std::pair{"u_sigmaFlat", 0.01}, std::pair{"u_sigmaEnds", 1.1},
          std::pair{"u_flatWeight", -0.1}, std::pair{"u_f0", 1.1},
          std::pair{"u_fiberRotationDegrees", 361.0}})
    {
        auto assets = MakeSatinAssets();
        assets.effectiveMaterialInstances.begin()->second["parameters"][name] = invalid;
        const auto result = VL::PreflightOfflineRenderInput(
            MakeSnapshot(), assets, Settings());
        ASSERT_TRUE(result.IsFailure()) << name;
        EXPECT_EQ(result.Error().code, "Offline.PreflightFailed");
    }
}

TEST(OfflineSceneAdapter, AshikhminSatinRejectsDegenerateUvBeforePublication)
{
    auto assets = MakeSatinAssets();
    auto imported = std::make_shared<MeshAssetImportResult>(
        *assets.meshImports.begin()->second);
    for (Vertex& vertex : imported->modelResource.sections[0].vertices)
        vertex.texCoord = Eigen::Vector2f::Zero();
    assets.sections.begin()->second.imported = imported;
    const auto result = VL::PreflightOfflineRenderInput(
        MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_NE(result.Error().message.find("non-degenerate UV"), std::string::npos);
}

VL::OfflineWorldAssets MakeSlantedVelvetAssets()
{
    auto assets = MakeAssets();
    auto& material = assets.effectiveMaterialInstances.begin()->second;
    material["offlineModel"] = "AshikhminSlantedVelvet";
    material["parameters"] = {
        {"u_sigma", 0.5}, {"u_slantDegrees", 40}, {"u_f0", 0.05},
        {"u_baseColor", {0.3, 0.015, 0.06, 1}}, {"u_fiberRotationDegrees", 180}};
    return assets;
}

TEST(OfflineSceneAdapter, SlantedVelvetFreezesMaterialAndSignedFiberDirection)
{
    auto assets = MakeSlantedVelvetAssets();
    ASSERT_TRUE(VL::PreflightOfflineRenderInput(
        MakeSnapshot(), assets, Settings()).IsSuccess());
    const auto result = VL::BuildOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const auto& input = result.Value();
    ASSERT_EQ(input.instanceSlantedVelvets.size(), 1u);
    EXPECT_EQ(input.scene.instances[0].model, VL::Offline::SurfaceModel::AshikhminSlantedVelvet);
    EXPECT_EQ(input.scene.instances[0].slantedVelvet, input.instanceSlantedVelvets[0].get());
    EXPECT_DOUBLE_EQ(input.scene.instances[0].fiberRotationDegrees, 180);
    assets.effectiveMaterialInstances.begin()->second["parameters"]["u_slantDegrees"] = 20;
    EXPECT_DOUBLE_EQ(input.instanceSlantedVelvets[0]->slantDegrees, 40);
}

TEST(OfflineSceneAdapter, SlantedVelvetRejectsInvalidParametersInPreflight)
{
    for (const auto& [name, value] : {
         std::pair{"u_sigma", 0.2}, std::pair{"u_slantDegrees", 20.0},
         std::pair{"u_f0", 1.1}, std::pair{"u_fiberRotationDegrees", 361.0}})
    {
        auto assets = MakeSlantedVelvetAssets();
        assets.effectiveMaterialInstances.begin()->second["parameters"][name] = value;
        EXPECT_TRUE(VL::PreflightOfflineRenderInput(
            MakeSnapshot(), assets, Settings()).IsFailure()) << name;
    }
}

TEST(OfflineSceneAdapter, SlantedVelvetPreflightRejectsUnknownParameters)
{
    auto assets = MakeSlantedVelvetAssets();
    assets.effectiveMaterialInstances.begin()->second["parameters"]["u_roughness"] = 0.5;
    const auto result = VL::PreflightOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_NE(result.Error().message.find("unsupported input"), std::string::npos);
}

TEST(OfflineSceneAdapter, SlantedVelvetRejectsDegenerateUvBeforePublication)
{
    auto assets = MakeSlantedVelvetAssets();
    auto imported = std::make_shared<MeshAssetImportResult>(
        *assets.meshImports.begin()->second);
    for (Vertex& vertex : imported->modelResource.sections[0].vertices)
        vertex.texCoord = Eigen::Vector2f::Zero();
    assets.sections.begin()->second.imported = imported;
    const auto result = VL::PreflightOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_NE(result.Error().message.find("non-degenerate UV"), std::string::npos);
}

TEST(OfflineSceneAdapter, SquareEmitterKeepsWorldNormalAndRadiance)
{
    auto snapshot = MakeSnapshot();
    VL::LightSnapshot light;
    light.type = VL::LightSnapshotType::SquareArea;
    light.position = Eigen::Vector3f(0, 2, 1);
    light.direction = Eigen::Vector3f(0, -1, 0);
    light.squareSize = 2.5f;
    light.color = Eigen::Vector3f(0.8f, 0.4f, 0.2f);
    light.intensity = 8.0f;
    snapshot.lights.push_back(light);
    auto result = VL::BuildOfflineRenderInput(snapshot, MakeAssets(), Settings());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    const auto& scene = result.Value().scene;
    EXPECT_TRUE(scene.hasAreaEmitter);
    EXPECT_FALSE(scene.directionalLight);
    EXPECT_DOUBLE_EQ(scene.lightWidth, 2.5);
    EXPECT_DOUBLE_EQ(scene.areaRadiance, 8.0);
    const VL::Offline::Vec3 normal =
        VL::Offline::cross(scene.areaU, scene.areaV);
    EXPECT_NEAR(normal.y, -1.0, 1.0e-6);
}

TEST(OfflineSceneAdapter, PointLightIsNeverSilentlyConverted)
{
    auto snapshot = MakeSnapshot();
    snapshot.lights.emplace_back();
    snapshot.lights.back().type = VL::LightSnapshotType::Point;
    auto result = VL::BuildOfflineRenderInput(snapshot, MakeAssets(), Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().code, "Offline.LightUnsupported");
}

TEST(OfflineSceneAdapter, ClothProfilePreservesAuthoredCurveSegments)
{
    const nlohmann::json yarn = {
        {"coverage", 0.45}, {"dye", {0.8, 0.5, 0.3}}, {"kd", 0.2},
        {"gammaS", 20.0}, {"gammaV", 15.0},
        {"offsets", {-25.0, 25.0}}, {"lengths", {1.33}}};
    nlohmann::json profile = {
        {"type", "offlineModelProfile"}, {"version", 1},
        {"model", "ClothFiber"}, {"eta", 1.5},
        {"yarns", {yarn, yarn}}};
    const auto fabric = VL::ParseOfflineClothProfile(profile);
    EXPECT_DOUBLE_EQ(fabric.yarns[0].lengths[0], 1.33);
    EXPECT_EQ(fabric.yarns[1].sampleCount, VL::Offline::kTangentSamples);
    profile["yarns"][1]["lengths"] = nlohmann::json::array();
    EXPECT_THROW(VL::ParseOfflineClothProfile(profile), std::runtime_error);
}

TEST(OfflineSceneAdapter, ClothProfileRejectsNegativeSegmentLength)
{
    const nlohmann::json yarn = {
        {"coverage", 0.4}, {"dye", {0.8, 0.5, 0.3}}, {"kd", 0.2},
        {"gammaS", 20.0}, {"gammaV", 15.0},
        {"offsets", {-25.0, 25.0}}, {"lengths", {-1.0}}};
    const nlohmann::json profile = {
        {"type", "offlineModelProfile"}, {"version", 1},
        {"model", "ClothFiber"}, {"eta", 1.5},
        {"yarns", {yarn, yarn}}};
    EXPECT_THROW(VL::ParseOfflineClothProfile(profile), std::runtime_error);
}

TEST(OfflineSceneAdapter, ClothProfileRejectsNegativeDye)
{
    const nlohmann::json yarn = {
        {"coverage", 0.4}, {"dye", {-0.1, 0.5, 0.3}}, {"kd", 0.2},
        {"gammaS", 20.0}, {"gammaV", 15.0},
        {"offsets", {-25.0, 25.0}}, {"lengths", {1.0}}};
    const nlohmann::json profile = {
        {"type", "offlineModelProfile"}, {"version", 1},
        {"model", "ClothFiber"}, {"eta", 1.5},
        {"yarns", {yarn, yarn}}};
    EXPECT_THROW(VL::ParseOfflineClothProfile(profile), std::runtime_error);
}

TEST(OfflineSceneAdapter, FrozenGroomUsesSignedUvAndCoreRepeatFiltering)
{
    auto assets = MakeAssets();
    const nlohmann::json yarn = {
        {"coverage", 0.4}, {"dye", {0.8, 0.5, 0.3}}, {"kd", 0.2},
        {"gammaS", 20.0}, {"gammaV", 15.0},
        {"offsets", {-25.0, 25.0}}, {"lengths", {1.0}}};
    const std::string profilePath = "Profiles/cloth.json";
    const std::string groomPath = "Textures/T_groom.json";
    assets.modelProfiles.emplace(profilePath, nlohmann::json{
        {"type", "offlineModelProfile"}, {"version", 1},
        {"model", "ClothFiber"}, {"eta", 1.5}, {"yarns", {yarn, yarn}}});
    auto& material = assets.effectiveMaterialInstances.begin()->second;
    material["offlineModel"] = "ClothFiber";
    material["parameters"] = nlohmann::json::object();
    material["modelProfile"] = profilePath;
    material["textures"] = {{"groomMap", groomPath}};
    auto bytes = std::make_shared<const std::string>(GroomPfm(1.0f, -1.0f));
    assets.groomAssets.emplace(groomPath, VL::OfflineWorldAssets::GroomAsset{
        {}, {}, "", "", bytes});
    auto result = VL::BuildOfflineRenderInput(MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsSuccess()) << result.Error().message;
    ASSERT_EQ(result.Value().instanceGrooms.size(), 1u);
    EXPECT_EQ(result.Value().scene.instances[0].groom,
        result.Value().instanceGrooms[0].get());
    EXPECT_NEAR(VL::Offline::groomDirection(
        *result.Value().instanceGrooms[0], {0.5, 0.5}).x, -1.0, 1e-6);
    EXPECT_NEAR(VL::Offline::groomDirection(
        *result.Value().instanceGrooms[0], {-0.5, 0.5}).x, -1.0, 1e-6);
}

TEST(OfflineSceneAdapter, InvalidGroomIsNeverReplacedWithDefaultAxes)
{
    EXPECT_THROW(VL::Offline::ParseGroomPfm(GroomPfm(0.0f, 1.0f),
        "Textures/T_groom.pfm"), std::runtime_error);
}

TEST(OfflineSceneAdapter, FrozenColorImageRequiresDecodableBytes)
{
    auto assets = MakeAssets();
    const std::string reference = "Textures/T_albedo.json";
    assets.effectiveMaterialInstances.begin()->second["textures"] =
        {{"baseColorMap", reference}};
    auto bytes = std::make_shared<const std::string>("not a PNG");
    assets.colorAssets.emplace(reference, VL::OfflineWorldAssets::ColorAsset{
        {}, "Textures/albedo.png", "", "", bytes, ".png", true, true, true});
    const auto preflight = VL::PreflightOfflineRenderInput(
        MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(preflight.IsFailure());
    EXPECT_EQ(preflight.Error().sourcePath,
        "Maps/SC_offline/Materials/MI_cloth.json");
    EXPECT_EQ(preflight.Error().sourceNode, "cloth::section");
    const auto prepared = VL::BuildOfflineRenderInput(
        MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(prepared.IsFailure());
    EXPECT_EQ(prepared.Error().sourceNode, "cloth::section");
}

TEST(OfflineSceneAdapter, PreflightRejectsInvalidFrozenGroom)
{
    auto assets = MakeAssets();
    const nlohmann::json yarn = {
        {"coverage", 0.4}, {"dye", {0.8, 0.5, 0.3}}, {"kd", 0.2},
        {"gammaS", 20.0}, {"gammaV", 15.0},
        {"offsets", {-25.0, 25.0}}, {"lengths", {1.0}}};
    const std::string profilePath = "Profiles/cloth.json";
    const std::string groomPath = "Textures/T_groom.json";
    assets.modelProfiles.emplace(profilePath, nlohmann::json{
        {"type", "offlineModelProfile"}, {"version", 1},
        {"model", "ClothFiber"}, {"eta", 1.5}, {"yarns", {yarn, yarn}}});
    auto& material = assets.effectiveMaterialInstances.begin()->second;
    material["offlineModel"] = "ClothFiber";
    material["parameters"] = nlohmann::json::object();
    material["modelProfile"] = profilePath;
    material["textures"] = {{"groomMap", groomPath}};
    assets.groomAssets.emplace(groomPath, VL::OfflineWorldAssets::GroomAsset{
        {}, {}, "", "", std::make_shared<const std::string>("not PFM")});
    const auto result = VL::PreflightOfflineRenderInput(
        MakeSnapshot(), assets, Settings());
    ASSERT_TRUE(result.IsFailure());
    EXPECT_EQ(result.Error().sourcePath,
        "Maps/SC_offline/Materials/MI_cloth.json");
    EXPECT_EQ(result.Error().sourceNode, "cloth::section");
}

TEST(OfflineSceneAdapter, ColorRowsMatchRealtimeMaterialTextureOrientation)
{
    // 未压缩 1x2 TGA，top-left origin：第一行红、第二行蓝。
    std::string tga(18, '\0');
    tga[2] = 2;
    tga[12] = 1;
    tga[14] = 2;
    tga[16] = 24;
    tga[17] = 0x20;
    tga.append("\0\0\xff\xff\0\0", 6);
    VL::OfflineWorldAssets::ColorAsset asset{
        {}, "color.tga", "", "",
        std::make_shared<const std::string>(std::move(tga)),
        ".tga", false, false, false};
    const auto map = VL::DecodeOfflineColorImage(asset);
    ASSERT_EQ(map->pixels.size(), 2u);
    EXPECT_NEAR(VL::Offline::sampleColorMap(*map, {0.5, 0.25}).z, 1.0, 1e-6);
    EXPECT_NEAR(VL::Offline::sampleColorMap(*map, {0.5, 0.75}).x, 1.0, 1e-6);
}
} // namespace

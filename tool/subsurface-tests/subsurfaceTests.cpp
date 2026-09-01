#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "render/subsurface/subsurfaceAssets.h"
#include "render/subsurface/subsurfaceGBufferCodec.h"

namespace
{

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireNear(
    float actual,
    float expected,
    float tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
    {
        throw std::runtime_error(
            message + ": actual=" + std::to_string(actual) +
            ", expected=" + std::to_string(expected));
    }
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open test source: " + path.string());
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

nlohmann::json MakeProfileJson(
    uint32_t profileId,
    const std::string& unit,
    float distance,
    float worldScale)
{
    return {
        {"name", "Test Profile"},
        {"type", "subsurfaceProfile"},
        {"schemaVersion", 1},
        {"profileId", profileId},
        {"meanFreePathColor", {1.0f, 0.5f, 0.25f}},
        {"meanFreePathDistance", distance},
        {"distanceUnit", unit},
        {"worldUnitScale", worldScale},
        {"kernelVersion", 1},
    };
}

nlohmann::json MakeSkinJson(
    uint32_t skinLutId,
    const std::string& outputMode)
{
    return {
        {"name", "Test Skin LUT"},
        {"type", "preintegratedSkinLut"},
        {"schemaVersion", 1},
        {"skinLutId", skinLutId},
        {"lutVersion", 1},
        {"width", 128},
        {"height", 64},
        {"thicknessMax", 8.0f},
        {"thicknessUnit", "millimeter"},
        {"worldUnitScale", 0.001f},
        {"outputMode", outputMode},
        {"scatterColor", {1.0f, 0.5f, 0.3f}},
        {"transmissionColor", {1.0f, 0.2f, 0.1f}},
        {"sourceIdentity", "test.compute.v1"},
    };
}

void TestProfileSchemaAndUnits()
{
    const VL::SubsurfaceProfileAsset millimeter =
        VL::ParseSubsurfaceProfileAsset(
            MakeProfileJson(1, "millimeter", 1.2f, 0.001f),
            "SSP_mm.json");
    const VL::SubsurfaceProfileAsset centimeter =
        VL::ParseSubsurfaceProfileAsset(
            MakeProfileJson(2, "centimeter", 0.12f, 0.01f),
            "SSP_cm.json");
    const VL::SubsurfaceProfileAsset meter =
        VL::ParseSubsurfaceProfileAsset(
            MakeProfileJson(3, "meter", 0.0012f, 1.0f),
            "SSP_m.json");

    RequireNear(
        millimeter.meanFreePathDistance * millimeter.worldUnitScale,
        0.0012f,
        1.0e-7f,
        "Millimeter conversion mismatch");
    RequireNear(
        centimeter.meanFreePathDistance * centimeter.worldUnitScale,
        0.0012f,
        1.0e-7f,
        "Centimeter conversion mismatch");
    RequireNear(
        meter.meanFreePathDistance * meter.worldUnitScale,
        0.0012f,
        1.0e-7f,
        "Meter conversion mismatch");
}

void TestSchemaRejection()
{
    nlohmann::json profile =
        MakeProfileJson(1, "millimeter", 1.2f, 0.001f);
    profile["schemaVersion"] = 2;
    bool rejected = false;
    try
    {
        VL::ParseSubsurfaceProfileAsset(profile, "invalid-profile.json");
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    Require(rejected, "Unsupported profile schema was accepted");

    nlohmann::json skin = MakeSkinJson(1, "finalDiffuseResponse");
    skin["width"] = 64;
    rejected = false;
    try
    {
        VL::ParsePreintegratedSkinLutAsset(skin, "invalid-skin.json");
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    Require(rejected, "Invalid skin LUT resolution was accepted");
}

void TestSkinModesAndStableIds()
{
    const VL::PreintegratedSkinLutAsset finalDiffuse =
        VL::ParsePreintegratedSkinLutAsset(
            MakeSkinJson(1, "finalDiffuseResponse"),
            "PSL_final.json");
    const VL::PreintegratedSkinLutAsset multiplier =
        VL::ParsePreintegratedSkinLutAsset(
            MakeSkinJson(15, "scatteringMultiplier"),
            "PSL_multiplier.json");
    Require(
        finalDiffuse.outputMode ==
            VL::PreintegratedSkinOutputMode::FinalDiffuseResponse,
        "Final diffuse output mode mismatch");
    Require(
        multiplier.outputMode ==
            VL::PreintegratedSkinOutputMode::ScatteringMultiplier,
        "Scattering multiplier output mode mismatch");
    Require(
        multiplier.skinLutId == VL::PreintegratedSkinMaximumId,
        "Maximum stable skin LUT ID was not preserved");
}

void TestSubsurfaceRoundTrip()
{
    VL::SubsurfaceMaterialGBufferInputs input;
    input.subsurfaceColor = {0.9f, 0.4f, 0.2f};
    input.wrapWidth = 0.45f;
    input.backscatterPower = 4.0f;
    input.backscatterWeight = 0.35f;
    input.subsurfaceWeight = 0.8f;
    input.thickness = 0.006f;
    input.transmissionWeight = 0.12f;
    const VL::SubsurfaceMaterialGBufferInputs decoded =
        VL::DecodeSubsurfaceGBuffer(
            VL::EncodeSubsurfaceGBuffer(input));
    RequireNear(decoded.subsurfaceColor[1], input.subsurfaceColor[1], 0.0f, "Subsurface color round-trip mismatch");
    RequireNear(decoded.wrapWidth, input.wrapWidth, 0.0f, "Subsurface wrap round-trip mismatch");
    RequireNear(decoded.transmissionWeight, input.transmissionWeight, 0.0f, "Subsurface transmission round-trip mismatch");
}

void TestPreintegratedSkinRoundTrip()
{
    VL::PreintegratedSkinMaterialGBufferInputs input;
    input.skinLutId = 15;
    input.thickness = 0.006f;
    input.thicknessScale = 1.0f;
    input.subsurfaceWeight = 0.9f;
    input.curvature = 0.0f;
    input.transmissionWeight = 0.1f;
    input.characterLighting = {1.8f, 1.37f, 1.0f, 0.75f};
    const VL::PreintegratedSkinMaterialGBufferInputs decoded =
        VL::DecodePreintegratedSkinGBuffer(
            VL::EncodePreintegratedSkinGBuffer(input));
    Require(decoded.skinLutId == input.skinLutId, "Skin LUT ID round-trip mismatch");
    RequireNear(decoded.thickness, input.thickness, 0.0f, "Skin thickness round-trip mismatch");
    RequireNear(decoded.transmissionWeight, input.transmissionWeight, 0.0f, "Skin transmission round-trip mismatch");
    RequireNear(decoded.characterLighting[0], input.characterLighting[0], 0.0f, "Skin environment multiplier round-trip mismatch");
    RequireNear(decoded.characterLighting[1], input.characterLighting[1], 0.0f, "Skin directional multiplier round-trip mismatch");
    RequireNear(decoded.characterLighting[2], input.characterLighting[2], 0.0f, "Skin GI multiplier round-trip mismatch");
    RequireNear(decoded.characterLighting[3], input.characterLighting[3], 0.0f, "Skin virtual-light multiplier round-trip mismatch");
}

void TestSubsurfaceProfileRoundTrip()
{
    VL::SubsurfaceProfileMaterialGBufferInputs input;
    input.profileId = 255;
    input.subsurfaceWeight = 0.75f;
    input.thickness = 0.005f;
    input.transmissionWeight = 0.08f;
    const VL::SubsurfaceProfileMaterialGBufferInputs decoded =
        VL::DecodeSubsurfaceProfileGBuffer(
            VL::EncodeSubsurfaceProfileGBuffer(input));
    Require(decoded.profileId == input.profileId, "Profile ID round-trip mismatch");
    RequireNear(decoded.subsurfaceWeight, input.subsurfaceWeight, 0.0f, "Profile weight round-trip mismatch");
    RequireNear(decoded.thickness, input.thickness, 0.0f, "Profile thickness round-trip mismatch");
}

void TestDefaultLitComponentRecomposition()
{
    const std::array<float, 3> directDiffuse = {0.7f, 0.3f, 0.1f};
    const std::array<float, 3> directSpecular = {0.2f, 0.25f, 0.3f};
    const std::array<float, 3> indirectDiffuse = {0.1f, 0.12f, 0.14f};
    const std::array<float, 3> indirectSpecular = {0.08f, 0.07f, 0.06f};
    const std::array<float, 3> emissive = {0.02f, 0.01f, 0.0f};
    const float ambientOcclusion = 0.65f;
    for (size_t channel = 0; channel < 3; ++channel)
    {
        const float legacy =
            emissive[channel] +
            directDiffuse[channel] + directSpecular[channel] +
            (indirectDiffuse[channel] + indirectSpecular[channel]) *
                ambientOcclusion;
        const float components =
            directDiffuse[channel] +
            indirectDiffuse[channel] * ambientOcclusion +
            emissive[channel] +
            directSpecular[channel] +
            indirectSpecular[channel] * ambientOcclusion;
        RequireNear(
            components,
            legacy,
            1.0e-6f,
            "DefaultLit component recomposition mismatch");
    }
}

void TestComputeOnlyGenerationContract()
{
    const std::filesystem::path root(VULKANLEARN_SOURCE_DIR);
    const std::string assetsSource = ReadText(
        root / "source/render/subsurface/subsurfaceAssets.cpp");
    Require(
        assetsSource.find("BuildSubsurfaceProfileTableImage") ==
            std::string::npos,
        "CPU profile table generation remains in the asset loader");
    Require(
        assetsSource.find("BuildPreintegratedSkinLutTableImage") ==
            std::string::npos,
        "CPU skin LUT generation remains in the asset loader");
    Require(
        assetsSource.find("EvaluateBurleyRadial") == std::string::npos,
        "CPU radial profile evaluation remains in runtime code");

    const std::string computeShader = ReadText(
        root / "shader/glsl/generator/subsurfaceLookupTables.comp");
    Require(
        computeShader.find("GenerateProfileTexel") != std::string::npos &&
            computeShader.find("GenerateSkinTexel") != std::string::npos &&
            computeShader.find("weights / weightSum") != std::string::npos,
            "Compute lookup generation contract is incomplete");
}

void TestDiscreteGBufferSamplingContract()
{
    const std::filesystem::path root(VULKANLEARN_SOURCE_DIR);
    const std::string filterSource = ReadText(
        root / "shader/glsl/engine/subsurfaceProfileFilter.glsl");
    Require(
        filterSource.find("GetNearestSubsurfaceProfileTexel") !=
            std::string::npos &&
            filterSource.find("texelFetch") != std::string::npos,
        "Subsurface profile filter has no discrete texel sampling path");
    Require(
        filterSource.find("texture(gbufferBTexture") == std::string::npos &&
            filterSource.find("texture(gbufferDTexture") == std::string::npos &&
            filterSource.find("texture(sceneDepthTexture") == std::string::npos &&
            filterSource.find("texture(sourceLighting") == std::string::npos,
        "Subsurface profile filter still linearly samples discrete GBuffer data");
}


void TestCompositionDiscreteGBufferSamplingContract()
{
    const std::filesystem::path root(VULKANLEARN_SOURCE_DIR);
    const std::string compositionSource = ReadText(
        root / "shader/glsl/pass/sssComposition.frag");
    // composition 也要保持离散 GBuffer 语义，不能只修 profile filter 的邻域读取。
    Require(
        compositionSource.find("GetNearestSubsurfaceProfileTexel") !=
            std::string::npos &&
            compositionSource.find("texelFetch") != std::string::npos,
        "SSS composition has no discrete GBuffer sampling path");
    Require(
        compositionSource.find("texture(gbufferB") == std::string::npos &&
            compositionSource.find("texture(gbufferD") == std::string::npos,
        "SSS composition still linearly samples discrete GBuffer data");
}

void TestPreintegratedSkinBottomNormalContract()
{
    const std::filesystem::path root(VULKANLEARN_SOURCE_DIR);
    const std::string inputs = ReadText(
        root / "shader/glsl/engine/materialInputs.glsl");
    const std::string surface = ReadText(
        root / "shader/glsl/engine/materialSurface.glsl");
    const std::string codec = ReadText(
        root / "shader/glsl/engine/gbufferCodec.glsl");
    const std::string skinLighting = ReadText(
        root / "shader/glsl/engine/preintegratedSkinLighting.glsl");
    Require(
        inputs.find("vec3 bottomNormal") != std::string::npos &&
            surface.find("preintegratedSkinBottomNormal") != std::string::npos,
        "PreintegratedSkin bottom normal is missing from the material contract");
    Require(
        codec.find("gbufferF.zw") != std::string::npos &&
            codec.find("OctahedronToUnitVector") != std::string::npos,
        "Skin bottom normal does not have an explicit GBuffer F.zw codec");
    Require(
        skinLighting.find("preintegratedSkin.bottomNormal") !=
            std::string::npos,
        "PreintegratedSkin lighting does not consume bottom normal");
}
void TestVirtualLightModuleContract()
{
    const std::filesystem::path root(VULKANLEARN_SOURCE_DIR);
    const std::string virtualLight = ReadText(
        root / "shader/glsl/engine/virtualLight.glsl");
    const std::string hairLighting = ReadText(
        root / "shader/glsl/engine/hairLighting.glsl");
    const std::string skinLighting = ReadText(
        root / "shader/glsl/engine/preintegratedSkinLighting.glsl");
    Require(
        virtualLight.find("struct VirtualLight") != std::string::npos &&
            virtualLight.find("CreateCameraVirtualLight") != std::string::npos &&
            virtualLight.find("EvaluateVirtualLightVisibility") !=
                std::string::npos,
        "Virtual Light module does not expose the shared direction/radiance/visibility contract");
    Require(
        hairLighting.find("CreateCameraVirtualLight") != std::string::npos &&
            skinLighting.find("CreateCameraVirtualLight") != std::string::npos,
        "Hair and Skin do not consume the shared Virtual Light module");
    Require(
        skinLighting.find("transmissionMultiplier") != std::string::npos,
        "Skin Virtual Light cannot disable transmission independently");
}
void TestDebugView16Contract()
{
    const std::filesystem::path root(VULKANLEARN_SOURCE_DIR);
    const std::string contract = ReadText(
        root / "documents/rendering/subsurface-shading-models.md");
    const std::string deferredLighting = ReadText(
        root / "shader/glsl/engine/deferredLighting.glsl");
    Require(
        contract.find("检查 ID 2/3 当前像素 response") !=
            std::string::npos,
        "Debug View 16 contract no longer names ID 2/3 response");
    const size_t profileFunctionStart = deferredLighting.find(
        "DeferredLightingResult ShadeSubsurfaceProfileDeferredSurfaceDetailed");
    const size_t nextFunctionStart = deferredLighting.find(
        "DeferredLightingResult ShadeClearCoatDeferredSurfaceDetailed",
        profileFunctionStart);
    Require(
        profileFunctionStart != std::string::npos &&
            nextFunctionStart != std::string::npos,
        "Subsurface profile deferred lighting function boundaries are missing");
    const std::string profileFunction = deferredLighting.substr(
        profileFunctionStart,
        nextFunctionStart - profileFunctionStart);
    Require(
        profileFunction.find(
            "result.localSubsurfaceLighting = vec3(0.0);") !=
            std::string::npos,
        "ID 5 still publishes diffuse lighting as Debug View 16 response");
}

} // namespace

int main()
{
    try
    {
        TestProfileSchemaAndUnits();
        TestSchemaRejection();
        TestSkinModesAndStableIds();
        TestSubsurfaceRoundTrip();
        TestPreintegratedSkinRoundTrip();
        TestSubsurfaceProfileRoundTrip();
        TestDefaultLitComponentRecomposition();
        TestComputeOnlyGenerationContract();
        TestDiscreteGBufferSamplingContract();
        TestCompositionDiscreteGBufferSamplingContract();
        TestPreintegratedSkinBottomNormalContract();
        TestVirtualLightModuleContract();
        TestDebugView16Contract();
        std::cout << "Subsurface contract tests passed." << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Subsurface contract tests failed: "
                  << exception.what() << std::endl;
        return 1;
    }
}

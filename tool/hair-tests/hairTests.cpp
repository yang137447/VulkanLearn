#include <cmath>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <functional>
#include <vector>

#include <nlohmann/json.hpp>

#include "render/hair/hairAssets.h"
#include "support/hairGBufferCodec.h"
#include "render/hair/hairLutCoordinates.h"
#include "render/hair/hairMaterialContract.h"
#include "support/hairReference.h"

namespace
{

using VL::Hair::HairPath;
using VL::Hair::HairReferenceParameters;
using VL::Hair::HairVec3;

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

void RequireThrows(
    const std::function<void()>& callback,
    const std::string& message)
{
    try
    {
        callback();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error(message);
}

void TestHairBaseColorAbsorptionConvention()
{
    const float fiberRadius = 0.00005f;
    const float referencePathLength = 4.0f * fiberRadius;
    const float darkColor = 0.05f;
    const float lightColor = 0.5f;
    const float darkAbsorption = -std::log(darkColor) / referencePathLength;
    const float lightAbsorption = -std::log(lightColor) / referencePathLength;
    Require(
        darkAbsorption > lightAbsorption,
        "darker Hair BaseColor must produce greater absorption");
    RequireNear(
        std::exp(-darkAbsorption * referencePathLength),
        darkColor,
        1.0e-6f,
        "Hair absorption reference path did not restore author color");
}

void TestHairAngleConvention()
{
    const VL::Hair::HairTangentFrame frame =
        VL::Hair::BuildTangentFrame(
            {0.0f, 0.0f, 1.0f},
            {1.0f, 0.0f, 0.0f},
            -1.0f);
    const VL::Hair::HairAngles angles = VL::Hair::ComputeHairAngles(
        frame,
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f});
    RequireNear(angles.thetaI, 0.0f, 1.0e-6f, "thetaI convention mismatch");
    RequireNear(angles.thetaO, 0.0f, 1.0e-6f, "thetaO convention mismatch");
    RequireNear(angles.thetaH, 0.0f, 1.0e-6f, "thetaH convention mismatch");
    RequireNear(angles.thetaD, 0.0f, 1.0e-6f, "thetaD convention mismatch");
    RequireNear(
        std::abs(angles.deltaPhi),
        0.0f,
        1.0e-6f,
        "deltaPhi zero point mismatch");
    RequireNear(frame.handedness, -1.0f, 1.0e-6f, "tangent handedness mismatch");
}

void TestHairAzimuthalRoots()
{
    const std::vector<VL::Hair::HairAzimuthalRoot> reflectionRoots =
        VL::Hair::FindAzimuthalRoots(HairPath::R, 0.0f, 0.0f, 1.55f);
    Require(!reflectionRoots.empty(), "R path root search returned no root");
    RequireNear(
        reflectionRoots.front().height,
        0.0f,
        0.02f,
        "R path root is not centered");
    Require(
        reflectionRoots.front().jacobian > 0.0f,
        "R path Jacobian must be positive");

    const std::vector<VL::Hair::HairAzimuthalRoot> transmissionRoots =
        VL::Hair::FindAzimuthalRoots(HairPath::TT, 0.15f, 0.2f, 1.55f);
    Require(!transmissionRoots.empty(), "TT path root search returned no root");
    for (const VL::Hair::HairAzimuthalRoot& root : transmissionRoots)
    {
        Require(root.height >= -1.0f && root.height <= 1.0f, "root height out of range");
        Require(std::isfinite(root.jacobian), "root Jacobian is not finite");
    }
}

void TestHairPathWeights()
{
    HairReferenceParameters parameters;
    parameters.absorption = {0.2f, 0.6f, 1.2f};
    const VL::Hair::HairAngles angles{
        0.1f,
        -0.05f,
        0.025f,
        -0.075f,
        0.2f,
        0.35f,
        0.15f};
    const VL::Hair::HairPathResponse reflection =
        VL::Hair::EvaluateHairPath(HairPath::R, angles, parameters);
    const VL::Hair::HairPathResponse transmission =
        VL::Hair::EvaluateHairPath(HairPath::TT, angles, parameters);
    const VL::Hair::HairPathResponse internalReflection =
        VL::Hair::EvaluateHairPath(HairPath::TRT, angles, parameters);
    Require(reflection.pathLength <= 1.0e-6f, "R path must not enter the fiber");
    Require(transmission.pathLength > 0.0f, "TT path must have internal length");
    Require(internalReflection.pathLength >= transmission.pathLength, "TRT path must be longer");
    Require(
        transmission.transmittance[2] <= transmission.transmittance[0],
        "absorption must color and attenuate TT");
    Require(
        internalReflection.transmittance[2] <= internalReflection.transmittance[0],
        "absorption must color and attenuate TRT");
    Require(
        reflection.contribution[2] <= reflection.contribution[0] + 1.0e-5f,
        "R must not be tinted by absorption");
}

void TestHairWhiteFurnaceAndParameterIsolation()
{
    const VL::Hair::HairTangentFrame frame =
        VL::Hair::BuildTangentFrame(
            {0.0f, 0.0f, 1.0f},
            {1.0f, 0.0f, 0.0f},
            1.0f);
    HairReferenceParameters smooth;
    smooth.longitudinalRoughness = 0.1f;
    HairReferenceParameters rough = smooth;
    rough.longitudinalRoughness = 0.4f;
    const VL::Hair::HairScatteringResponse smoothResponse =
        VL::Hair::EvaluateHairScattering(
            frame,
            {0.0f, 0.3f, 0.9539392f},
            {0.0f, -0.1f, 0.9949874f},
            smooth);
    const VL::Hair::HairScatteringResponse roughResponse =
        VL::Hair::EvaluateHairScattering(
            frame,
            {0.0f, 0.3f, 0.9539392f},
            {0.0f, -0.1f, 0.9949874f},
            rough);
    Require(
        std::isfinite(smoothResponse.paths[0].contribution[0]) &&
            std::isfinite(roughResponse.paths[0].contribution[0]),
        "roughness sweep produced a non-finite response");
    Require(
        VL::Hair::EvaluateMultipleScatteringBudget(smooth, 0.25f, 0.5f) <= 0.25f,
        "multiple scattering exceeded remaining single-scattering energy");
    smooth.scatter = 1.0f;
    Require(
        VL::Hair::EvaluateMultipleScatteringBudget(smooth, 0.25f, 0.5f) > 0.0f,
        "scatter did not affect the multiple-scattering budget");
}

void TestHairLutMetadataContract()
{
    VL::HairAzimuthalLutMetadata metadata;
    metadata.sourceIdentity = "fixture-hair-author-v1";
    const nlohmann::json serialized =
        VL::SerializeHairAzimuthalLutMetadata(metadata);
    const VL::HairAzimuthalLutMetadata restored =
        VL::ParseHairAzimuthalLutMetadata(serialized, "hair-fixture");
    Require(
        restored.sourceIdentity == metadata.sourceIdentity,
        "Hair LUT metadata source identity round-trip mismatch");
    Require(
        restored.width == VL::HairAzimuthalLutWidth &&
            restored.height == VL::HairAzimuthalLutHeight,
        "Hair LUT metadata dimensions round-trip mismatch");

    nlohmann::json unknownField = serialized;
    unknownField["unknown"] = true;
    RequireThrows(
        [&unknownField]()
        {
            VL::ParseHairAzimuthalLutMetadata(unknownField, "unknown-field");
        },
        "Hair LUT metadata accepted an unknown field");

    nlohmann::json wrongVersion = serialized;
    wrongVersion["lutVersion"] = 99;
    RequireThrows(
        [&wrongVersion]()
        {
            VL::ParseHairAzimuthalLutMetadata(wrongVersion, "wrong-version");
        },
        "Hair LUT metadata accepted an incompatible version");

    nlohmann::json wrongDimensions = serialized;
    wrongDimensions["width"] = 64;
    RequireThrows(
        [&wrongDimensions]()
        {
            VL::ParseHairAzimuthalLutMetadata(wrongDimensions, "wrong-dimensions");
        },
        "Hair LUT metadata accepted incompatible dimensions");

    nlohmann::json wrongChannels = serialized;
    wrongChannels["channels"] = 3;
    RequireThrows(
        [&wrongChannels]()
        {
            VL::ParseHairAzimuthalLutMetadata(wrongChannels, "wrong-channels");
        },
        "Hair LUT metadata accepted incompatible channels");

    nlohmann::json wrongIor = serialized;
    wrongIor["ior"] = 1.0;
    RequireThrows(
        [&wrongIor]()
        {
            VL::ParseHairAzimuthalLutMetadata(wrongIor, "wrong-ior");
        },
        "Hair LUT metadata accepted invalid IOR");

    nlohmann::json wrongRadius = serialized;
    wrongRadius["fiberRadius"] = 0.0;
    RequireThrows(
        [&wrongRadius]()
        {
            VL::ParseHairAzimuthalLutMetadata(wrongRadius, "wrong-radius");
        },
        "Hair LUT metadata accepted invalid fiber radius");

    nlohmann::json wrongConvention = serialized;
    wrongConvention["wrap"] = "clamp-all";
    RequireThrows(
        [&wrongConvention]()
        {
            VL::ParseHairAzimuthalLutMetadata(wrongConvention, "wrong-convention");
        },
        "Hair LUT metadata accepted an incompatible coordinate convention");
}
void TestHairGBufferRoundTrip()
{
    VL::Hair::HairGBufferInputs expected;
    expected.scatter = 0.35f;
    expected.backlit = 0.72f;
    expected.cuticleTilt = -0.08f;
    expected.multipleScatteringWeight = 0.21f;
    expected.absorption = {0.8f, 1.4f, 2.1f};
    expected.opacity = 0.73f;
    expected.tangent = {0.6f, -0.2f, 0.7745967f};
    expected.tangentHandedness = -1.0f;
    expected.specular = 0.3f;
    expected.roughness = 0.42f;
    expected.ambientOcclusion = 0.66f;
    expected.characterLighting = {0.9f, 0.8f, 0.55f, 0.70588237f};
    expected.precomputedShadowFactor = 0.74f;
    const VL::Hair::HairGBufferInputs actual =
        VL::Hair::DecodeHairGBuffer(VL::Hair::EncodeHairGBuffer(expected));
    RequireNear(actual.absorption[0], expected.absorption[0], 1.0e-6f, "Hair absorption R GBuffer mismatch");
    RequireNear(actual.absorption[1], expected.absorption[1], 1.0e-6f, "Hair absorption G GBuffer mismatch");
    RequireNear(actual.absorption[2], expected.absorption[2], 1.0e-6f, "Hair absorption B GBuffer mismatch");
    RequireNear(actual.opacity, expected.opacity, 1.0e-6f, "Hair opacity GBuffer mismatch");
    RequireNear(actual.scatter, expected.scatter, 1.0e-6f, "Hair scatter GBuffer mismatch");
    RequireNear(actual.backlit, expected.backlit, 1.0e-6f, "Hair backlit GBuffer mismatch");
    RequireNear(actual.cuticleTilt, expected.cuticleTilt, 1.0e-6f, "Hair tilt GBuffer mismatch");
    RequireNear(
        actual.multipleScatteringWeight,
        expected.multipleScatteringWeight,
        1.0e-6f,
        "Hair MS GBuffer mismatch");
    RequireNear(actual.tangentHandedness, -1.0f, 1.0e-6f, "Hair tangent sign mismatch");
    RequireNear(actual.specular, expected.specular, 1.0e-6f, "Hair specular GBuffer mismatch");
    RequireNear(actual.roughness, expected.roughness, 1.0e-6f, "Hair roughness GBuffer mismatch");
    RequireNear(actual.ambientOcclusion, expected.ambientOcclusion, 1.0e-6f, "Hair AO GBuffer mismatch");
    for (size_t index = 0; index < expected.characterLighting.size(); ++index)
    {
        RequireNear(
            actual.characterLighting[index],
            expected.characterLighting[index],
            1.0e-6f,
            "Hair character lighting GBuffer mismatch");
    }
    RequireNear(
        actual.precomputedShadowFactor,
        expected.precomputedShadowFactor,
        1.0e-6f,
        "Hair precomputed shadow GBuffer mismatch");
}

void TestHairLutCoordinateGolden()
{
    const VL::Hair::HairLutUv center =
        VL::Hair::EncodeHairAzimuthalLutUv(0.0f, 0.0f, 3.0f / 7.0f);
    RequireNear(center.u, 0.5f, 1.0e-6f, "Hair LUT deltaPhi center mismatch");
    RequireNear(center.v, 224.0f / 512.0f, 1.0e-6f, "Hair LUT atlas center mismatch");

    const VL::Hair::HairLutDecodedUv decoded =
        VL::Hair::DecodeHairAzimuthalLutUv(center);
    RequireNear(decoded.deltaPhi, 0.0f, 1.0e-6f, "Hair LUT deltaPhi round-trip mismatch");
    RequireNear(decoded.thetaD, 0.0f, 1.0e-6f, "Hair LUT thetaD round-trip mismatch");
    RequireNear(
        decoded.roughnessSlice,
        3.0f / 7.0f,
        1.0e-6f,
        "Hair LUT roughness slice round-trip mismatch");
    RequireNear(decoded.thetaDSample, 0.5f, 1.0e-6f, "Hair LUT thetaD sample mismatch");

    const VL::Hair::HairLutUv wrapped =
        VL::Hair::EncodeHairAzimuthalLutUv(
            2.0f * VL::Hair::HairPi,
            VL::Hair::HairHalfPi,
            1.0f);
    RequireNear(wrapped.u, 0.5f, 1.0e-6f, "Hair LUT angular wrapping mismatch");
    Require(wrapped.v < 1.0f, "Hair LUT upper atlas coordinate overflowed");
}

void TestHairMaterialAuthoringContract()
{
    VL::HairAzimuthalLutMetadata metadata;
    metadata.sourceIdentity = "fixture-hair-author-v1";
    const nlohmann::json material = {
        {"shadingModel", "Hair"},
        {"parameters", {
            {"u_hairOptical", {1.0f, metadata.ior, metadata.fiberRadius, 0.04f}},
            {"u_hairScattering", {0.25f, 0.35f, 0.22f, 0.25f}},
            {"u_hairCoverage", {0.8f, 0.35f, 1.0f, 0.0f}},
            {"u_hairCharacterLighting", {1.0f, 1.0f, 0.55f, 0.70588237f}}}}};

    VL::ValidateHairMaterialAuthoringContract(
        material,
        metadata,
        "hair-contract-fixture");

    nlohmann::json wrongRadius = material;
    wrongRadius["parameters"]["u_hairOptical"][2] = metadata.fiberRadius * 2.0f;
    RequireThrows(
        [&wrongRadius, &metadata]()
        {
            VL::ValidateHairMaterialAuthoringContract(
                wrongRadius,
                metadata,
                "wrong-radius");
        },
        "Hair material contract accepted a mismatched fiber radius");

    nlohmann::json wrongCharacterLighting = material;
    wrongCharacterLighting["parameters"]["u_hairCharacterLighting"][2] = 4.1f;
    RequireThrows(
        [&wrongCharacterLighting, &metadata]()
        {
            VL::ValidateHairMaterialAuthoringContract(
                wrongCharacterLighting,
                metadata,
                "wrong-character-lighting");
        },
        "Hair material contract accepted invalid character lighting");

    const std::filesystem::path missingResourceRoot =
        std::filesystem::path(VULKANLEARN_SOURCE_DIR) /
        "tool" / "hair-tests" / "missing-authoring-resource";
    RequireThrows(
        [&missingResourceRoot]()
        {
            VL::LoadHairAzimuthalLutAsset(missingResourceRoot);
        },
        "Hair loader accepted a resource without authoring metadata");
}

void TestHairValidationSceneManifest()
{
    const std::filesystem::path manifestPath =
        std::filesystem::path(VULKANLEARN_SOURCE_DIR) /
        "tool" / "hair-tests" / "fixtures_hair_validation_scenes.json";
    std::ifstream file(manifestPath);
    Require(file.is_open(), "Hair validation scene manifest is missing");
    const nlohmann::json manifest = nlohmann::json::parse(file);
    Require(manifest.at("schemaVersion") == 1, "Hair validation scene schema mismatch");
    const nlohmann::json& scenes = manifest.at("scenes");
    Require(scenes.is_array() && scenes.size() == 5, "Hair validation scene count mismatch");
    for (const nlohmann::json& scene : scenes)
    {
        Require(scene.at("material").get<std::string>() == "MI_hair", "Hair scene material mismatch");
        Require(scene.at("camera").contains("position"), "Hair scene camera position missing");
        Require(scene.at("lighting").contains("mode"), "Hair scene lighting mode missing");
        Require(scene.at("geometry").contains("coverageMip"), "Hair scene coverage contract missing");
    }
}

} // namespace

TEST(Hair, AngleConvention)
{
    TestHairAngleConvention();
}

TEST(Hair, BaseColorAbsorptionConvention)
{
    TestHairBaseColorAbsorptionConvention();
}

TEST(Hair, AzimuthalRoots)
{
    TestHairAzimuthalRoots();
}

TEST(Hair, PathWeights)
{
    TestHairPathWeights();
}

TEST(Hair, WhiteFurnaceAndParameterIsolation)
{
    TestHairWhiteFurnaceAndParameterIsolation();
}

TEST(Hair, GBufferRoundTrip)
{
    TestHairGBufferRoundTrip();
}

TEST(Hair, LutMetadataContract)
{
    TestHairLutMetadataContract();
}

TEST(Hair, LutCoordinateGolden)
{
    TestHairLutCoordinateGolden();
}

TEST(Hair, MaterialAuthoringContract)
{
    TestHairMaterialAuthoringContract();
}

TEST(Hair, ValidationSceneManifest)
{
    TestHairValidationSceneManifest();
}

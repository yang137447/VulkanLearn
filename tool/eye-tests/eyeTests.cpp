#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "render/eye/eyeAssets.h"
#include "render/eye/eyeGBufferCodec.h"
#include "render/eye/eyeLodContract.h"
#include "render/eye/eyePerformanceBudget.h"
#include "render/eye/eyeResourceSet.h"

namespace
{

using Json = nlohmann::json;
namespace fs = std::filesystem;

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

Json MakeProfile(
    uint32_t profileId,
    const std::string& distanceUnit,
    float worldUnitScale,
    float pupilRadius = 2.0f)
{
    const float authoredScale = distanceUnit == "millimeter"
        ? 1.0f
        : distanceUnit == "centimeter"
            ? 0.1f
            : 0.001f;
    return {
        {"name", "Eye Contract Test"},
        {"type", "eyeProfile"},
        {"schemaVersion", 1},
        {"profileVersion", 1},
        {"profileId", profileId},
        {"sourceIdentity", "test.eye.compute.v1"},
        {"ior", 1.376f},
        {"eyeRadius", 12.0f * authoredScale},
        {"corneaRadius", 7.8f * authoredScale},
        {"irisDistance", 3.0f * authoredScale},
        {"irisRadius", 6.0f * authoredScale},
        {"pupilRadiusRange", {1.5f * authoredScale, 4.0f * authoredScale}},
        {"pupilRadius", pupilRadius * authoredScale},
        {"limbusWidth", 0.5f * authoredScale},
        {"causticStrength", 0.15f},
        {"distanceUnit", distanceUnit},
        {"worldUnitScale", worldUnitScale},
        {"causticLutVersion", 1},
        {"kernelVersion", 1}
    };
}

void TestProfileUnitsAndDefaults()
{
    const VL::EyeProfileAsset millimeter = VL::ParseEyeProfileAsset(
        MakeProfile(1, "millimeter", 0.001f),
        "eyeProfiles/EP_mm.json");
    const VL::EyeProfileAsset centimeter = VL::ParseEyeProfileAsset(
        MakeProfile(2, "centimeter", 0.01f),
        "eyeProfiles/EP_cm.json");
    const VL::EyeProfileAsset meter = VL::ParseEyeProfileAsset(
        MakeProfile(3, "meter", 1.0f, 2.0f),
        "eyeProfiles/EP_m.json");

    for (const VL::EyeProfileAsset* profile :
         {&millimeter, &centimeter, &meter})
    {
        RequireNear(profile->eyeRadius, 0.012f, 1.0e-7f, "eyeRadius unit conversion mismatch");
        RequireNear(profile->corneaRadius, 0.0078f, 1.0e-7f, "corneaRadius unit conversion mismatch");
        RequireNear(profile->irisDistance, 0.003f, 1.0e-7f, "irisDistance unit conversion mismatch");
        RequireNear(profile->irisRadius, 0.006f, 1.0e-7f, "irisRadius unit conversion mismatch");
        RequireNear(profile->pupilRadiusMin, 0.0015f, 1.0e-7f, "pupil minimum conversion mismatch");
        RequireNear(profile->pupilRadiusMax, 0.004f, 1.0e-7f, "pupil maximum conversion mismatch");
        Require(profile->unit == "meter", "runtime Eye profile unit must be meter");
    }
    Require(millimeter.distanceUnit == "millimeter", "authoring unit was not retained");
    RequireNear(meter.pupilRadius, 0.002f, 1.0e-7f, "meter pupil radius mismatch");
}

void TestVersionAndRangeRejection()
{
    Json wrongSchema = MakeProfile(1, "millimeter", 0.001f);
    wrongSchema["schemaVersion"] = 99;
    RequireThrows(
        [&]() { VL::ParseEyeProfileAsset(wrongSchema, "wrong-schema"); },
        "incompatible Eye schema was accepted");

    Json wrongKernel = MakeProfile(1, "millimeter", 0.001f);
    wrongKernel["kernelVersion"] = 99;
    RequireThrows(
        [&]() { VL::ParseEyeProfileAsset(wrongKernel, "wrong-kernel"); },
        "incompatible Eye kernel was accepted");

    Json wrongLut = MakeProfile(1, "millimeter", 0.001f);
    wrongLut["causticLutVersion"] = 99;
    RequireThrows(
        [&]() { VL::ParseEyeProfileAsset(wrongLut, "wrong-lut"); },
        "incompatible Eye LUT version was accepted");

    Json reservedId = MakeProfile(0, "millimeter", 0.001f);
    RequireThrows(
        [&]() { VL::ParseEyeProfileAsset(reservedId, "reserved-id"); },
        "Eye profile ID 0 was accepted");

    Json outOfRangeId = MakeProfile(16, "millimeter", 0.001f);
    RequireThrows(
        [&]() { VL::ParseEyeProfileAsset(outOfRangeId, "out-of-range-id"); },
        "Eye profile ID above 15 was accepted");

    Json invalidPupil = MakeProfile(1, "millimeter", 0.001f, 5.0f);
    RequireThrows(
        [&]() { VL::ParseEyeProfileAsset(invalidPupil, "invalid-pupil"); },
        "pupil radius outside the profile range was accepted");

    Json invalidScale = MakeProfile(1, "millimeter", 0.01f);
    RequireThrows(
        [&]() { VL::ParseEyeProfileAsset(invalidScale, "invalid-scale"); },
        "inconsistent worldUnitScale was accepted");
}

void TestDuplicateIdsAreRejectedAtLoad()
{
    const fs::path root = fs::temp_directory_path() /
        "vulkanlearn-eye-contract-duplicate";
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
    fs::create_directories(root / "eyeProfiles");
    {
        std::ofstream first(root / "eyeProfiles" / "EP_a.json");
        first << MakeProfile(1, "millimeter", 0.001f).dump(2);
        std::ofstream second(root / "eyeProfiles" / "EP_b.json");
        second << MakeProfile(1, "millimeter", 0.001f).dump(2);
    }
    RequireThrows(
        [&]() { VL::LoadEyeProfileAssets(root); },
        "duplicate Eye profile IDs were accepted by asset loading");
    fs::remove_all(root, cleanupError);
}

void TestResourcePathNormalization()
{
    VL::EyeResourceSet resources;
    VL::EyeProfileAsset profile = VL::ParseEyeProfileAsset(
        MakeProfile(1, "millimeter", 0.001f),
        "eyeProfiles/EP_human_default.json");
    resources.profiles.push_back(profile);
    resources.profileIdsByAssetPath.emplace(profile.assetPath, profile.profileId);
    resources.profileIndicesById.emplace(profile.profileId, 0);

    Require(
        resources.ResolveProfileId("eyeProfiles\\EP_human_default.json") == 1,
        "Eye profile path separator normalization failed");
    Require(
        resources.GetProfile(1).assetPath == "eyeProfiles/EP_human_default.json",
        "Eye profile reverse lookup failed");
    RequireThrows(
        [&]() { resources.ResolveProfileId("eyeProfiles/EP_missing.json"); },
        "inactive Eye profile path was accepted");
}

void TestCausticGainNormalization()
{
    constexpr float strength = 0.15f;
    float weightedMean = 0.0f;
    constexpr int ringCount = 256;
    for (int index = 0; index < ringCount; ++index)
    {
        const float radius = (static_cast<float>(index) + 0.5f) /
            static_cast<float>(ringCount);
        const float gain = VL::EvaluateEyeCausticGain(
            radius * radius,
            0.5f,
            strength);
        Require(std::isfinite(gain), "Eye caustic gain is non-finite");
        Require(gain > 0.0f, "Eye caustic gain is not positive");
        weightedMean += 2.0f * radius * gain /
            static_cast<float>(ringCount);
    }
    RequireNear(
        weightedMean,
        1.0f,
        2.0e-3f,
        "Eye caustic unit-disk average is not normalized");

    RequireNear(
        VL::EvaluateEyeCausticGain(0.0f, 0.5f, 0.0f),
        1.0f,
        1.0e-6f,
        "neutral Eye caustic gain is not one");
}

struct Vec3
{
    float x;
    float y;
    float z;
};

float Dot(Vec3 first, Vec3 second)
{
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

float Length(Vec3 value)
{
    return std::sqrt(Dot(value, value));
}

Vec3 Normalize(Vec3 value)
{
    const float length = Length(value);
    return {value.x / length, value.y / length, value.z / length};
}

Vec3 Refract(Vec3 incident, Vec3 normal, float eta)
{
    const float cosine = -Dot(normal, incident);
    const float discriminant = 1.0f - eta * eta * (1.0f - cosine * cosine);
    if (discriminant < 0.0f)
    {
        return {0.0f, 0.0f, 0.0f};
    }
    return {
        eta * incident.x + (eta * cosine - std::sqrt(discriminant)) * normal.x,
        eta * incident.y + (eta * cosine - std::sqrt(discriminant)) * normal.y,
        eta * incident.z + (eta * cosine - std::sqrt(discriminant)) * normal.z};
}

float ExactFresnel(float cosine, float ior)
{
    const float sinThetaSquared = 1.0f - cosine * cosine;
    const float etaI = 1.0f;
    const float etaT = ior;
    const float eta = etaI / etaT;
    const float transmittedCosineSquared =
        1.0f - eta * eta * sinThetaSquared;
    if (transmittedCosineSquared <= 0.0f)
    {
        return 1.0f;
    }
    const float transmittedCosine = std::sqrt(transmittedCosineSquared);
    const float parallel =
        (etaT * cosine - etaI * transmittedCosine) /
        (etaT * cosine + etaI * transmittedCosine);
    const float perpendicular =
        (etaI * cosine - etaT * transmittedCosine) /
        (etaI * cosine + etaT * transmittedCosine);
    return 0.5f * (parallel * parallel + perpendicular * perpendicular);
}

float SchlickFresnel(float cosine, float ior)
{
    const float ratio = (1.0f - ior) / (1.0f + ior);
    const float f0 = ratio * ratio;
    return f0 + (1.0f - f0) *
        std::pow(std::max(0.0f, 1.0f - cosine), 5.0f);
}

void TestPlaneSnellAndFresnelReference()
{
    const float ior = 1.376f;
    const Vec3 incident = {0.0f, 0.0f, -1.0f};
    const Vec3 normal = {0.0f, 0.0f, 1.0f};
    const Vec3 transmitted = Refract(incident, normal, 1.0f / ior);
    RequireNear(transmitted.x, 0.0f, 1.0e-6f, "normal-incidence Snell X mismatch");
    RequireNear(transmitted.y, 0.0f, 1.0e-6f, "normal-incidence Snell Y mismatch");
    RequireNear(transmitted.z, -1.0f, 1.0e-6f, "normal-incidence Snell Z mismatch");

    const float f0 = std::pow((1.0f - ior) / (1.0f + ior), 2.0f);
    RequireNear(
        ExactFresnel(1.0f, ior),
        f0,
        1.0e-6f,
        "exact Fresnel normal incidence mismatch");
    RequireNear(
        SchlickFresnel(1.0f, ior),
        f0,
        1.0e-6f,
        "Schlick Fresnel normal incidence mismatch");
    Require(
        ExactFresnel(0.1f, ior) > ExactFresnel(1.0f, ior),
        "grazing Fresnel did not increase");
    Require(
        std::isfinite(Length(Normalize(transmitted))),
        "normalized Snell direction is non-finite");
}

void TestMaterialSchemaAndMetadata()
{
    const fs::path materialPath = fs::path(VULKANLEARN_SOURCE_DIR) /
        "shader" / "glsl" / "M_eye.json";
    std::ifstream materialFile(materialPath);
    Require(materialFile.is_open(), "M_eye.json is missing");
    Json material;
    materialFile >> material;
    Require(material.at("shadingModel") == "Eye", "M_eye shading model mismatch");
    Require(
        material.at("renderStates").at("renderMode") == "ForwardOpaque",
        "M_eye must use ForwardOpaque");
    for (const char* parameter : {
             "u_eyeSurface", "u_eyeGeometry", "u_eyeIrisColor",
             "u_eyeScleraColor", "u_eyeProfileId", "u_eyeCorneaIor"})
    {
        Require(
            material.at("parameters").contains(parameter),
            std::string("M_eye is missing parameter ") + parameter);
    }

    VL::EyeCausticLutMetadata metadata;
    metadata.computeArtifactGenerationKey = "artifact-42";
    metadata.sourceDigest = "source-42";
    const Json serialized = VL::SerializeEyeCausticLutMetadata(metadata);
    Require(serialized.at("width") == VL::EyeCausticLutWidth, "LUT width round-trip mismatch");
    Require(serialized.at("height") == VL::EyeCausticLutHeight, "LUT height round-trip mismatch");
    Require(serialized.at("layers") == VL::EyeCausticLutLayerCount, "LUT layer round-trip mismatch");
    Require(serialized.at("computeArtifactGenerationKey") == "artifact-42", "artifact identity round-trip mismatch");
    Require(serialized.at("sourceDigest") == "source-42", "source digest round-trip mismatch");
}

void TestEyeGBufferRoundTripAndRejection()
{
    VL::EyeMaterialGBufferInputs inputs;
    inputs.irisColor = {0.21f, 0.42f, 0.63f};
    inputs.irisUv = {0.37f, 0.81f};
    inputs.scleraColor = {0.72f, 0.66f, 0.58f};
    inputs.irisNormal = {0.0f, 0.0f, 1.0f};
    inputs.irisMask = 0.85f;
    inputs.validIrisHit = 1.0f;
    inputs.irisRadius = 0.006f;
    inputs.irisDistance = 0.003f;
    inputs.pupilRadius = 0.0024f;
    inputs.limbusWidth = 0.0006f;
    inputs.corneaIor = 1.376f;
    inputs.causticStrength = 0.15f;
    inputs.causticProfileId = 3.0f;
    inputs.scleraProfileId = 5.0f;
    inputs.roughness = 0.18f;
    inputs.ambientOcclusion = 0.91f;
    inputs.opacity = 1.0f;

    const VL::EyeGBufferPayload payload = VL::EncodeEyeGBuffer(inputs);
    const VL::EyeMaterialGBufferInputs decoded = VL::DecodeEyeGBuffer(payload);
    RequireNear(decoded.irisUv[0], inputs.irisUv[0], 1.0e-6f, "Eye GBuffer iris U mismatch");
    RequireNear(decoded.irisUv[1], inputs.irisUv[1], 1.0e-6f, "Eye GBuffer iris V mismatch");
    RequireNear(decoded.irisMask, inputs.irisMask, 1.0e-6f, "Eye GBuffer iris mask mismatch");
    RequireNear(decoded.pupilRadius / decoded.irisRadius, inputs.pupilRadius / inputs.irisRadius, 1.0e-6f, "Eye GBuffer pupil ratio mismatch");
    RequireNear(decoded.limbusWidth / decoded.irisRadius, inputs.limbusWidth / inputs.irisRadius, 1.0e-6f, "Eye GBuffer limbus ratio mismatch");
    Require(decoded.causticProfileId == inputs.causticProfileId, "Eye GBuffer caustic profile mismatch");
    Require(decoded.scleraProfileId == inputs.scleraProfileId, "Eye GBuffer sclera profile mismatch");
    Require(decoded.validIrisHit > 0.5f, "Eye GBuffer valid hit mismatch");

    VL::EyeGBufferPayload invalidPayload = payload;
    invalidPayload.gbufferD[2] = static_cast<float>(
        VL::PackEyeProfileAndValidity(3, 5, false));
    invalidPayload.gbufferD[0] = 0.03f;
    invalidPayload.gbufferD[1] = 0.97f;
    invalidPayload.gbufferD[3] = 1.0f;
    const VL::EyeMaterialGBufferInputs invalid =
        VL::DecodeEyeGBuffer(invalidPayload);
    Require(invalid.validIrisHit < 0.5f, "invalid Eye hit was accepted");
    RequireNear(invalid.irisUv[0], 0.5f, 1.0e-6f, "invalid Eye hit consumed U");
    RequireNear(invalid.irisUv[1], 0.5f, 1.0e-6f, "invalid Eye hit consumed V");
    RequireNear(invalid.irisMask, 0.0f, 1.0e-6f, "invalid Eye hit consumed mask");

    VL::EyeGBufferPayload wrongVersion = payload;
    wrongVersion.gbufferD[2] = static_cast<float>(
        VL::PackEyeProfileAndValidity(3, 5, true) + (1u << 9u));
    RequireThrows(
        [&]() { VL::DecodeEyeGBuffer(wrongVersion); },
        "Eye GBuffer version mismatch was silently accepted");
}

void TestEyeLodGazeAndPupilContract()
{
    VL::EyeLodContract contract = VL::MakeDefaultEyeLodContract(3, 1, 1);
    contract.side = VL::EyeSide::Left;
    contract.uvHandedness = -1;
    contract.pupilDilationMin = 0.1f;
    contract.pupilDilationMax = 0.9f;
    VL::ValidateEyeLodContract(contract, "lod-contract-test");

    const VL::EyeLodSelection far = VL::ResolveEyeLod(contract, 0.002f);
    const VL::EyeLodSelection mid = VL::ResolveEyeLod(contract, 0.012f);
    const VL::EyeLodSelection near = VL::ResolveEyeLod(contract, 0.05f);
    Require(far.tier == VL::EyeLodTier::Far, "far Eye LOD selection mismatch");
    Require(mid.tier == VL::EyeLodTier::Mid, "mid Eye LOD selection mismatch");
    Require(near.tier == VL::EyeLodTier::Near, "near Eye LOD selection mismatch");
    Require(
        far.profileId == mid.profileId && mid.profileId == near.profileId,
        "LOD changed profile identity");
    RequireNear(
        VL::ResolvePupilRadius(0.0015f, 0.004f, contract, 0.5f),
        0.00275f,
        1.0e-6f,
        "pupil dilation interpolation mismatch");
    RequireNear(
        VL::ApplyEyeUvHandedness(0.25f, contract),
        0.75f,
        1.0e-6f,
        "left-eye UV handedness mismatch");

    VL::EyeFrameContract frame;
    std::string frameError;
    Require(
        VL::ValidateEyeFrameContract(frame, &frameError),
        "default Eye frame is invalid: " + frameError);
    const VL::EyeGazeContract gaze{{1.0f, 0.0f, 0.0f}, 1.0f};
    const auto direction = VL::ResolveEyeGazeDirection(frame, gaze);
    RequireNear(direction[0], 1.0f, 1.0e-6f, "gaze frame X mismatch");
    RequireNear(direction[1], 0.0f, 1.0e-6f, "gaze frame Y mismatch");
    RequireNear(direction[2], 0.0f, 1.0e-6f, "gaze frame Z mismatch");

    nlohmann::json authored = {
        {"lodContract", {
            {"schemaVersion", 1},
            {"eyeSide", "left"},
            {"uvHandedness", -1},
            {"pupilDilationRange", {0.1f, 0.9f}},
            {"lods", {
                {{"tier", "far"}, {"minimumScreenRadius", 0.0f}},
                {{"tier", "mid"}, {"minimumScreenRadius", 0.01f}},
                {{"tier", "near"}, {"minimumScreenRadius", 0.04f}}}}}}};
    const VL::EyeLodContract parsed = VL::ParseEyeLodContract(
        authored,
        "lod-contract-json",
        3,
        1,
        1);
    Require(parsed.side == VL::EyeSide::Left, "JSON Eye side mismatch");
    Require(parsed.uvHandedness == -1, "JSON Eye handedness mismatch");
    RequireThrows(
        [&]() {
            VL::EyeLodContract invalid = parsed;
            invalid.tiers[1].profileVersion = 99;
            VL::ValidateEyeLodContract(invalid, "lod-contract-invalid");
        },
        "LOD profile version mismatch was accepted");
}

void TestEyePerformanceBudget()
{
    const VL::EyePerformanceBudget budget;
    Require(
        VL::CalculateEyeLutMemoryBytes(budget) ==
            static_cast<size_t>(64u * 64u * 256u * 4u * 2u),
        "Eye LUT memory budget mismatch");
    VL::EyePerformanceFrameStats stats;
    stats.eyeDrawCount = 2;
    stats.eyeDescriptorBindCount = 4;
    stats.eyeLutSampleCount = 4;
    std::string error;
    Require(
        VL::ValidateEyePerformanceFrame(budget, stats, &error),
        "valid Eye performance frame rejected: " + error);
    stats.eyeLutSampleCount = 5;
    Require(
        !VL::ValidateEyePerformanceFrame(budget, stats, &error),
        "Eye LUT sample budget overflow was accepted");
}

} // namespace

TEST(Eye, ProfileUnitsAndDefaults)
{
    TestProfileUnitsAndDefaults();
}

TEST(Eye, VersionAndRangeRejection)
{
    TestVersionAndRangeRejection();
}

TEST(Eye, DuplicateIdsAreRejectedAtLoad)
{
    TestDuplicateIdsAreRejectedAtLoad();
}

TEST(Eye, ResourcePathNormalization)
{
    TestResourcePathNormalization();
}

TEST(Eye, CausticGainNormalization)
{
    TestCausticGainNormalization();
}

TEST(Eye, PlaneSnellAndFresnelReference)
{
    TestPlaneSnellAndFresnelReference();
}

TEST(Eye, MaterialSchemaAndMetadata)
{
    TestMaterialSchemaAndMetadata();
}

TEST(Eye, GBufferRoundTripAndRejection)
{
    TestEyeGBufferRoundTripAndRejection();
}

TEST(Eye, LodGazeAndPupilContract)
{
    TestEyeLodGazeAndPupilContract();
}

TEST(Eye, PerformanceBudget)
{
    TestEyePerformanceBudget();
}

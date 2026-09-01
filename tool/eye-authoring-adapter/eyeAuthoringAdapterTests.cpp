#include "eyeAuthoringAdapter.h"

#include <cstdio>
#include <stdexcept>

#include <gtest/gtest.h>

namespace
{

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

} // namespace

TEST(EyeAuthoringAdapter, ConvertsLegacyAuthoring)
{
    const nlohmann::json legacy = {
        {"authoringModel", "Legacy"},
        {"eyeProfile", "eyeProfiles/EP_human_default.json"},
        {"irisColor", {0.1f, 0.2f, 0.3f}},
        {"scleraColor", {0.8f, 0.85f, 0.9f}},
        {"layer", "inner"},
        {"eyeSide", "left"},
        {"pupilDilation", 0.75f}};
    const VL::EyeAuthoringAdapterResult legacyResult =
        VL::ConvertEyeAuthoring(legacy);
    Require(
        legacyResult.materialInstance.at("material") ==
            "shader/glsl/M_eyeInner.json",
        "Legacy layer mapping mismatch");
    Require(
        legacyResult.materialInstance.at("parameters").at("u_eyeUvHandedness") ==
            -1.0f,
        "left-eye handedness mapping mismatch");
    Require(
        !legacyResult.report.at("sourceIdentity").get<std::string>().empty(),
        "adapter source identity is empty");
}

TEST(EyeAuthoringAdapter, ConvertsSubstrateAuthoring)
{
    const nlohmann::json substrate = {
        {"model", "Substrate"},
        {"profilePath", "eyeProfiles/EP_human_default.json"},
        {"layer", "cornea"},
        {"unsupportedParameters", {"clearCoatAnisotropy"}}};
    const VL::EyeAuthoringAdapterResult substrateResult =
        VL::ConvertEyeAuthoring(substrate);
    Require(
        substrateResult.materialInstance.at("material") ==
            "shader/glsl/M_eyeCornea.json",
        "Substrate layer mapping mismatch");
    Require(
        substrateResult.report.at("unsupportedFields").size() == 1,
        "unsupported field report mismatch");
}

TEST(EyeAuthoringAdapter, StrictModeRejectsUnsupportedFields)
{
    const nlohmann::json substrate = {
        {"model", "Substrate"},
        {"profilePath", "eyeProfiles/EP_human_default.json"},
        {"layer", "cornea"},
        {"unsupportedParameters", {"clearCoatAnisotropy"}}};
    VL::EyeAuthoringAdapterOptions strict;
    strict.strict = true;
    bool rejected = false;
    try
    {
        VL::ConvertEyeAuthoring(substrate, strict);
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    Require(rejected, "strict adapter accepted unsupported field");
}

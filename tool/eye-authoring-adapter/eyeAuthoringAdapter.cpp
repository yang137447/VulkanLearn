#include "eyeAuthoringAdapter.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "shader/build/contentHash.h"

namespace VL
{
namespace
{

using Json = nlohmann::json;

const Json* FindField(
    const Json& source,
    std::initializer_list<const char*> names)
{
    const Json* candidates[] = {
        &source,
        source.contains("parameters") ? &source.at("parameters") : nullptr,
        source.contains("legacy") ? &source.at("legacy") : nullptr,
        source.contains("substrate") ? &source.at("substrate") : nullptr};
    for (const Json* candidate : candidates)
    {
        if (candidate == nullptr || !candidate->is_object())
        {
            continue;
        }
        for (const char* name : names)
        {
            if (candidate->contains(name))
            {
                return &candidate->at(name);
            }
        }
    }
    return nullptr;
}

float ReadFloat(
    const Json& source,
    std::initializer_list<const char*> names,
    float defaultValue)
{
    const Json* value = FindField(source, names);
    if (value == nullptr)
    {
        return defaultValue;
    }
    if (!value->is_number())
    {
        throw std::runtime_error("Eye authoring value is not numeric");
    }
    return value->get<float>();
}

std::array<float, 3> ReadVector3(
    const Json& source,
    std::initializer_list<const char*> names,
    const std::array<float, 3>& defaultValue)
{
    const Json* value = FindField(source, names);
    if (value == nullptr)
    {
        return defaultValue;
    }
    if (!value->is_array() || value->size() < 3)
    {
        throw std::runtime_error(
            "Eye authoring vector must contain three values");
    }
    return {
        value->at(0).get<float>(),
        value->at(1).get<float>(),
        value->at(2).get<float>()};
}

std::string ReadString(
    const Json& source,
    std::initializer_list<const char*> names,
    std::string defaultValue = {})
{
    const Json* value = FindField(source, names);
    if (value == nullptr)
    {
        return defaultValue;
    }
    if (!value->is_string())
    {
        throw std::runtime_error("Eye authoring value is not a string");
    }
    return value->get<std::string>();
}

float ReadVisibility(
    const Json& source,
    std::initializer_list<const char*> names)
{
    return std::clamp(ReadFloat(source, names, 1.0f), 0.0f, 1.0f);
}

float ReadLayer(const Json& source)
{
    const Json* value = FindField(source, {"eyeLayer", "layer"});
    if (value == nullptr)
    {
        return 0.0f;
    }
    if (value->is_number())
    {
        return value->get<float>();
    }
    const std::string layer = value->get<std::string>();
    if (layer == "inner" || layer == "iris" || layer == "sclera")
    {
        return 1.0f;
    }
    if (layer == "cornea" || layer == "outer")
    {
        return 2.0f;
    }
    if (layer == "single")
    {
        return 0.0f;
    }
    throw std::runtime_error("Unsupported Eye authoring layer: " + layer);
}

std::string MaterialForLayer(float layer)
{
    if (layer >= 1.5f)
    {
        return "shader/glsl/M_eyeCornea.json";
    }
    if (layer >= 0.5f)
    {
        return "shader/glsl/M_eyeInner.json";
    }
    return "shader/glsl/M_eye.json";
}

} // namespace

EyeAuthoringAdapterResult ConvertEyeAuthoring(
    const nlohmann::json& source,
    const EyeAuthoringAdapterOptions& options)
{
    if (!source.is_object())
    {
        throw std::runtime_error("Eye authoring input must be an object");
    }
    const std::string authoringModel = ReadString(
        source,
        {"authoringModel", "model", "type"},
        "Legacy");
    const bool isSubstrate =
        authoringModel == "Substrate" || authoringModel == "substrate";
    if (authoringModel != "Legacy" && authoringModel != "legacy" &&
        !isSubstrate)
    {
        throw std::runtime_error(
            "Eye authoring model must be Legacy or Substrate");
    }

    const std::string profile = ReadString(
        source,
        {"eyeProfile", "profile", "profilePath"});
    if (profile.empty())
    {
        throw std::runtime_error(
            "Eye authoring input requires eyeProfile/profile");
    }
    const float layer = ReadLayer(source);
    const auto irisColor = ReadVector3(
        source,
        {"irisColor", "irisAlbedo", "baseColor"},
        {0.08f, 0.22f, 0.06f});
    const auto scleraColor = ReadVector3(
        source,
        {"scleraColor", "scleraAlbedo"},
        {0.88f, 0.90f, 0.94f});
    const float roughness = std::clamp(
        ReadFloat(source, {"roughness", "corneaRoughness"}, 0.08f),
        0.001f,
        1.0f);
    const float irisDistance = ReadFloat(
        source,
        {"irisDistance", "irisDepth"},
        0.003f);
    const float irisRadius = ReadFloat(source, {"irisRadius"}, 0.006f);
    const float pupilRadius = ReadFloat(
        source,
        {"pupilRadius", "pupilRadiusCurrent"},
        0.002f);
    const float limbusWidth = ReadFloat(source, {"limbusWidth"}, 0.0005f);
    const float ior = ReadFloat(source, {"corneaIor", "ior"}, 1.376f);
    const std::string side = ReadString(
        source,
        {"eyeSide", "side"},
        "right");
    const float defaultHandedness = side == "left" ? -1.0f : 1.0f;
    const float uvHandedness = ReadFloat(
        source,
        {"uvHandedness"},
        defaultHandedness);
    const auto gaze = ReadVector3(
        source,
        {"gazeDirection", "gaze"},
        {0.0f, 0.0f, 1.0f});
    const float gazeWeight = ReadVisibility(source, {"gazeWeight"});
    const float contactVisibility = ReadVisibility(
        source,
        {"contactVisibility", "eyelidContactVisibility"});
    const float ciliaVisibility = ReadVisibility(
        source,
        {"ciliaVisibility", "lashVisibility"});
    const float pupilDilation = std::clamp(
        ReadFloat(source, {"pupilDilation", "dilation"}, 0.5f),
        0.0f,
        1.0f);

    Json material = {
        {"name", source.value("name", "Migrated Eye Material")},
        {"type", "materialInstance"},
        {"material", MaterialForLayer(layer)},
        {"eyeProfile", profile},
        {"parameters", {
            {"u_eyeSurface", {roughness, 1.0f, 1.0f, 0.0f}},
            {"u_eyeGeometry", {irisDistance, irisRadius, pupilRadius, limbusWidth}},
            {"u_eyeIrisColor", {irisColor[0], irisColor[1], irisColor[2], 1.0f}},
            {"u_eyeScleraColor", {scleraColor[0], scleraColor[1], scleraColor[2], 1.0f}},
            {"u_eyeCorneaIor", ior},
            {"u_eyeLayer", layer},
            {"u_eyeContactVisibility", contactVisibility},
            {"u_eyeCiliaVisibility", ciliaVisibility},
            {"u_eyeUvHandedness", uvHandedness < 0.0f ? -1.0f : 1.0f},
            {"u_eyePupilDilation", pupilDilation},
            {"u_eyeGaze", {gaze[0], gaze[1], gaze[2], gazeWeight}}}}};

    std::vector<std::string> unsupported;
    if (source.contains("unsupportedParameters"))
    {
        for (const Json& value : source.at("unsupportedParameters"))
        {
            unsupported.push_back(value.get<std::string>());
        }
    }
    if (!unsupported.empty() && options.strict)
    {
        throw std::runtime_error(
            "Eye authoring input contains unsupported parameters: " +
            unsupported.front());
    }

    CanonicalFieldHasher hasher("vulkanlearn.eye-authoring-migration.v1");
    hasher.AddString("authoringModel", isSubstrate ? "Substrate" : "Legacy");
    hasher.AddString("profile", profile);
    hasher.AddString("mappedMaterial", material.dump());
    const std::string sourceIdentity = hasher.Finalize().ToHex();
    Json report = {
        {"migrationVersion", 1},
        {"sourceAuthoringModel", isSubstrate ? "Substrate" : "Legacy"},
        {"sourceIdentity", sourceIdentity},
        {"targetMaterial", material.at("material")},
        {"mappedFields", {
            "eyeProfile", "irisDistance", "irisRadius", "pupilRadius",
            "limbusWidth", "corneaIor", "roughness", "irisColor",
            "scleraColor", "eyeLayer", "contactVisibility", "ciliaVisibility",
            "uvHandedness", "pupilDilation", "gazeDirection"}},
        {"unsupportedFields", unsupported},
        {"warnings", unsupported}};
    return {std::move(material), std::move(report)};
}

EyeAuthoringAdapterResult ConvertEyeAuthoringFile(
    const std::filesystem::path& sourcePath,
    const EyeAuthoringAdapterOptions& options)
{
    std::ifstream input(sourcePath);
    if (!input.is_open())
    {
        throw std::runtime_error(
            "Failed to open Eye authoring file: " + sourcePath.string());
    }
    nlohmann::json source;
    input >> source;
    return ConvertEyeAuthoring(source, options);
}

} // namespace VL

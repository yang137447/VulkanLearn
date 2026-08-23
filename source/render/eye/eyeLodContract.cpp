#include "render/eye/eyeLodContract.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace VL
{
namespace
{

bool IsFinite(float value) noexcept
{
    return std::isfinite(value) != 0;
}

float Dot(
    const std::array<float, 3>& first,
    const std::array<float, 3>& second) noexcept
{
    return first[0] * second[0] +
        first[1] * second[1] +
        first[2] * second[2];
}

std::array<float, 3> Cross(
    const std::array<float, 3>& first,
    const std::array<float, 3>& second) noexcept
{
    return {
        first[1] * second[2] - first[2] * second[1],
        first[2] * second[0] - first[0] * second[2],
        first[0] * second[1] - first[1] * second[0]};
}

float Length(const std::array<float, 3>& value) noexcept
{
    return std::sqrt(Dot(value, value));
}

std::array<float, 3> Normalize(
    const std::array<float, 3>& value) noexcept
{
    const float length = Length(value);
    if (!IsFinite(length) || length <= 1.0e-6f)
    {
        return {0.0f, 0.0f, 1.0f};
    }
    return {value[0] / length, value[1] / length, value[2] / length};
}

std::string RequireString(
    const nlohmann::json& json,
    const char* field,
    std::string_view assetPath)
{
    if (!json.contains(field) || !json.at(field).is_string())
    {
        throw std::runtime_error(
            "Eye LOD field must be a string \"" + std::string(field) +
            "\": " + std::string(assetPath));
    }
    return json.at(field).get<std::string>();
}

float RequireFloat(
    const nlohmann::json& json,
    const char* field,
    std::string_view assetPath,
    float defaultValue)
{
    if (!json.contains(field))
    {
        return defaultValue;
    }
    if (!json.at(field).is_number())
    {
        throw std::runtime_error(
            "Eye LOD field must be numeric \"" + std::string(field) +
            "\": " + std::string(assetPath));
    }
    const float value = json.at(field).get<float>();
    if (!IsFinite(value))
    {
        throw std::runtime_error(
            "Eye LOD field is non-finite \"" + std::string(field) +
            "\": " + std::string(assetPath));
    }
    return value;
}

EyeLodTier ParseTier(
    std::string_view value,
    std::string_view assetPath)
{
    if (value == "near")
    {
        return EyeLodTier::Near;
    }
    if (value == "mid")
    {
        return EyeLodTier::Mid;
    }
    if (value == "far")
    {
        return EyeLodTier::Far;
    }
    throw std::runtime_error(
        "Unknown Eye LOD tier: " + std::string(assetPath));
}

std::string TierName(EyeLodTier tier)
{
    switch (tier)
    {
    case EyeLodTier::Near:
        return "near";
    case EyeLodTier::Mid:
        return "mid";
    case EyeLodTier::Far:
        return "far";
    }
    return "near";
}

} // namespace

EyeLodContract MakeDefaultEyeLodContract(
    uint32_t profileId,
    uint32_t profileVersion,
    uint32_t lutVersion)
{
    EyeLodContract contract;
    contract.profileId = profileId;
    contract.profileVersion = profileVersion;
    contract.lutVersion = lutVersion;
    contract.tiers = {
        {EyeLodTier::Far, 0.0f, 1.0f, 1.0f, profileVersion, lutVersion},
        {EyeLodTier::Mid, 0.008f, 1.0f, 1.0f, profileVersion, lutVersion},
        {EyeLodTier::Near, 0.03f, 1.0f, 1.0f, profileVersion, lutVersion}};
    return contract;
}

EyeLodContract ParseEyeLodContract(
    const nlohmann::json& json,
    std::string_view assetPath,
    uint32_t profileId,
    uint32_t profileVersion,
    uint32_t lutVersion)
{
    EyeLodContract contract = MakeDefaultEyeLodContract(
        profileId,
        profileVersion,
        lutVersion);
    if (!json.contains("lodContract"))
    {
        ValidateEyeLodContract(contract, assetPath);
        return contract;
    }
    const nlohmann::json& value = json.at("lodContract");
    if (!value.is_object())
    {
        throw std::runtime_error(
            "Eye lodContract must be an object: " + std::string(assetPath));
    }
    contract.schemaVersion = value.value(
        "schemaVersion",
        EyeLodContractSchemaVersion);
    const std::string side = value.value("eyeSide", std::string("right"));
    if (side == "left")
    {
        contract.side = EyeSide::Left;
    }
    else if (side == "right")
    {
        contract.side = EyeSide::Right;
    }
    else
    {
        throw std::runtime_error(
            "Eye lodContract eyeSide must be left or right: " +
            std::string(assetPath));
    }
    contract.uvHandedness = value.value("uvHandedness", 1);
    if (value.contains("pupilDilationRange"))
    {
        const nlohmann::json& range = value.at("pupilDilationRange");
        if (!range.is_array() || range.size() != 2 ||
            !range[0].is_number() || !range[1].is_number())
        {
            throw std::runtime_error(
                "Eye pupilDilationRange must contain two numbers: " +
                std::string(assetPath));
        }
        contract.pupilDilationMin = range[0].get<float>();
        contract.pupilDilationMax = range[1].get<float>();
    }
    if (value.contains("lods"))
    {
        if (!value.at("lods").is_array() || value.at("lods").empty())
        {
            throw std::runtime_error(
                "Eye lodContract lods must be a non-empty array: " +
                std::string(assetPath));
        }
        contract.tiers.clear();
        for (const nlohmann::json& item : value.at("lods"))
        {
            if (!item.is_object())
            {
                throw std::runtime_error(
                    "Eye LOD entry must be an object: " +
                    std::string(assetPath));
            }
            EyeLodTierContract tier;
            tier.tier = ParseTier(
                RequireString(item, "tier", assetPath),
                assetPath);
            tier.minimumScreenRadius = RequireFloat(
                item,
                "minimumScreenRadius",
                assetPath,
                0.0f);
            tier.irisDistanceScale = RequireFloat(
                item,
                "irisDistanceScale",
                assetPath,
                1.0f);
            tier.irisRadiusScale = RequireFloat(
                item,
                "irisRadiusScale",
                assetPath,
                1.0f);
            tier.profileVersion = item.value("profileVersion", profileVersion);
            tier.lutVersion = item.value("lutVersion", lutVersion);
            contract.tiers.push_back(tier);
        }
    }
    ValidateEyeLodContract(contract, assetPath);
    return contract;
}

void ValidateEyeLodContract(
    const EyeLodContract& contract,
    std::string_view assetPath)
{
    if (contract.schemaVersion != EyeLodContractSchemaVersion ||
        (contract.uvHandedness != -1 && contract.uvHandedness != 1) ||
        !IsFinite(contract.pupilDilationMin) ||
        !IsFinite(contract.pupilDilationMax) ||
        contract.pupilDilationMin < 0.0f ||
        contract.pupilDilationMax > 1.0f ||
        contract.pupilDilationMin > contract.pupilDilationMax ||
        contract.tiers.empty())
    {
        throw std::runtime_error(
            "Eye LOD contract has invalid schema/range: " +
            std::string(assetPath));
    }
    std::set<EyeLodTier> tierIds;
    float previousThreshold = -1.0f;
    for (const EyeLodTierContract& tier : contract.tiers)
    {
        if (!tierIds.insert(tier.tier).second ||
            !IsFinite(tier.minimumScreenRadius) ||
            !IsFinite(tier.irisDistanceScale) ||
            !IsFinite(tier.irisRadiusScale) ||
            tier.minimumScreenRadius < 0.0f ||
            tier.irisDistanceScale <= 0.0f ||
            tier.irisRadiusScale <= 0.0f ||
            tier.profileVersion != contract.profileVersion ||
            tier.lutVersion != contract.lutVersion ||
            tier.minimumScreenRadius < previousThreshold)
        {
            throw std::runtime_error(
                "Eye LOD tier is invalid or changes profile identity: " +
                std::string(assetPath));
        }
        previousThreshold = tier.minimumScreenRadius;
    }
}

nlohmann::json SerializeEyeLodContract(
    const EyeLodContract& contract)
{
    nlohmann::json lods = nlohmann::json::array();
    for (const EyeLodTierContract& tier : contract.tiers)
    {
        lods.push_back({
            {"tier", TierName(tier.tier)},
            {"minimumScreenRadius", tier.minimumScreenRadius},
            {"irisDistanceScale", tier.irisDistanceScale},
            {"irisRadiusScale", tier.irisRadiusScale},
            {"profileVersion", tier.profileVersion},
            {"lutVersion", tier.lutVersion}});
    }
    return {
        {"schemaVersion", contract.schemaVersion},
        {"eyeSide", contract.side == EyeSide::Left ? "left" : "right"},
        {"uvHandedness", contract.uvHandedness},
        {"pupilDilationRange", {
            contract.pupilDilationMin,
            contract.pupilDilationMax}},
        {"lods", std::move(lods)}};
}

EyeLodSelection ResolveEyeLod(
    const EyeLodContract& contract,
    float screenRadius)
{
    ValidateEyeLodContract(contract, "ResolveEyeLod");
    if (!IsFinite(screenRadius) || screenRadius < 0.0f)
    {
        throw std::runtime_error("Eye screen radius is invalid");
    }
    const EyeLodTierContract* selected = &contract.tiers.front();
    for (const EyeLodTierContract& tier : contract.tiers)
    {
        if (screenRadius >= tier.minimumScreenRadius)
        {
            selected = &tier;
        }
    }
    return {
        selected->tier,
        selected->irisDistanceScale,
        selected->irisRadiusScale,
        contract.profileId,
        contract.profileVersion,
        contract.lutVersion};
}

float ResolvePupilRadius(
    float minimumRadius,
    float maximumRadius,
    const EyeLodContract& contract,
    float dilation)
{
    ValidateEyeLodContract(contract, "ResolvePupilRadius");
    if (!IsFinite(minimumRadius) || !IsFinite(maximumRadius) ||
        minimumRadius <= 0.0f || maximumRadius < minimumRadius ||
        !IsFinite(dilation))
    {
        throw std::runtime_error("Eye pupil radius input is invalid");
    }
    const float span = contract.pupilDilationMax -
        contract.pupilDilationMin;
    const float normalized = span > 0.0f
        ? (dilation - contract.pupilDilationMin) / span
        : 0.0f;
    const float clamped = std::clamp(normalized, 0.0f, 1.0f);
    return minimumRadius + (maximumRadius - minimumRadius) * clamped;
}

float ApplyEyeUvHandedness(
    float u,
    const EyeLodContract& contract) noexcept
{
    return contract.uvHandedness < 0 ? 1.0f - u : u;
}

bool ValidateEyeFrameContract(
    const EyeFrameContract& frame,
    std::string* error) noexcept
{
    const float forwardLength = Length(frame.forward);
    const float upLength = Length(frame.up);
    const float rightLength = Length(frame.right);
    const float handedness = Dot(Cross(frame.right, frame.up), frame.forward);
    const bool valid = IsFinite(frame.handedness) &&
        std::abs(std::abs(frame.handedness) - 1.0f) <= 1.0e-4f &&
        std::abs(forwardLength - 1.0f) <= 1.0e-3f &&
        std::abs(upLength - 1.0f) <= 1.0e-3f &&
        std::abs(rightLength - 1.0f) <= 1.0e-3f &&
        std::abs(Dot(frame.forward, frame.up)) <= 1.0e-3f &&
        std::abs(Dot(frame.forward, frame.right)) <= 1.0e-3f &&
        std::abs(Dot(frame.up, frame.right)) <= 1.0e-3f &&
        std::abs(handedness - frame.handedness) <= 1.0e-3f;
    if (!valid && error != nullptr)
    {
        *error = "Eye frame axes are not an orthonormal handed basis";
    }
    return valid;
}

std::array<float, 3> ResolveEyeGazeDirection(
    const EyeFrameContract& frame,
    const EyeGazeContract& gaze) noexcept
{
    const std::array<float, 3> local = Normalize(gaze.direction);
    return Normalize({
        frame.right[0] * local[0] + frame.up[0] * local[1] + frame.forward[0] * local[2],
        frame.right[1] * local[0] + frame.up[1] * local[1] + frame.forward[1] * local[2],
        frame.right[2] * local[0] + frame.up[2] * local[1] + frame.forward[2] * local[2]});
}

} // namespace VL

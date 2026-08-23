#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace VL
{

inline constexpr uint32_t EyeLodContractSchemaVersion = 1;

enum class EyeSide
{
    Left,
    Right
};

enum class EyeLodTier
{
    Near,
    Mid,
    Far
};

struct EyeLodTierContract
{
    EyeLodTier tier = EyeLodTier::Near;
    float minimumScreenRadius = 0.0f;
    float irisDistanceScale = 1.0f;
    float irisRadiusScale = 1.0f;
    uint32_t profileVersion = 1;
    uint32_t lutVersion = 1;
};

struct EyeLodContract
{
    uint32_t schemaVersion = EyeLodContractSchemaVersion;
    EyeSide side = EyeSide::Right;
    int32_t uvHandedness = 1;
    uint32_t profileId = 0;
    uint32_t profileVersion = 1;
    uint32_t lutVersion = 1;
    float pupilDilationMin = 0.0f;
    float pupilDilationMax = 1.0f;
    std::vector<EyeLodTierContract> tiers;
};

struct EyeLodSelection
{
    EyeLodTier tier = EyeLodTier::Near;
    float irisDistanceScale = 1.0f;
    float irisRadiusScale = 1.0f;
    uint32_t profileId = 0;
    uint32_t profileVersion = 1;
    uint32_t lutVersion = 1;
};

struct EyeFrameContract
{
    std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    std::array<float, 3> right{1.0f, 0.0f, 0.0f};
    float handedness = 1.0f;
};

struct EyeGazeContract
{
    std::array<float, 3> direction{0.0f, 0.0f, 1.0f};
    float weight = 1.0f;
};

EyeLodContract MakeDefaultEyeLodContract(
    uint32_t profileId,
    uint32_t profileVersion,
    uint32_t lutVersion);

EyeLodContract ParseEyeLodContract(
    const nlohmann::json& json,
    std::string_view assetPath,
    uint32_t profileId,
    uint32_t profileVersion,
    uint32_t lutVersion);

void ValidateEyeLodContract(
    const EyeLodContract& contract,
    std::string_view assetPath);

nlohmann::json SerializeEyeLodContract(
    const EyeLodContract& contract);

EyeLodSelection ResolveEyeLod(
    const EyeLodContract& contract,
    float screenRadius);

float ResolvePupilRadius(
    float minimumRadius,
    float maximumRadius,
    const EyeLodContract& contract,
    float dilation);

float ApplyEyeUvHandedness(
    float u,
    const EyeLodContract& contract) noexcept;

bool ValidateEyeFrameContract(
    const EyeFrameContract& frame,
    std::string* error = nullptr) noexcept;

std::array<float, 3> ResolveEyeGazeDirection(
    const EyeFrameContract& frame,
    const EyeGazeContract& gaze) noexcept;

} // namespace VL

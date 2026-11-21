#pragma once

#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "world/worldWind.h"

namespace VL
{
struct SceneWindCommandRequest
{
    std::string operation;
    float strength = 0.0f;
};

inline bool ParseSceneWindCommand(std::string_view arguments,
    SceneWindCommandRequest& request, std::string& error)
{
    std::istringstream input{std::string(arguments)};
    SceneWindCommandRequest candidate;
    std::string token;
    std::string extra;
    error.clear();
    if (!(input >> candidate.operation) ||
        (candidate.operation != "get" && candidate.operation != "strength"))
    {
        error = "Usage: wind get | wind strength <cm/s>";
        return false;
    }
    if (candidate.operation == "strength")
    {
        if (!(input >> token))
        {
            error = "wind strength requires one finite non-negative cm/s value";
            return false;
        }
        try
        {
            size_t consumed = 0;
            candidate.strength = std::stof(token, &consumed);
            if (consumed != token.size() || !std::isfinite(candidate.strength) ||
                candidate.strength < 0.0f)
                throw std::invalid_argument("strength");
        }
        catch (const std::exception&)
        {
            error = "wind strength must be finite and non-negative (cm/s)";
            return false;
        }
    }
    if (input >> extra)
    {
        error = "Usage: wind get | wind strength <cm/s>";
        return false;
    }
    request = std::move(candidate);
    return true;
}

inline bool ApplySceneWindCommand(WorldWind& wind, const SceneWindCommandRequest& request,
    std::string& error)
{
    error.clear();
    if (!wind.authored)
        error = "active World has no authored wind object";
    else if (request.operation != "get" && request.operation != "strength")
        error = "unknown scene wind operation";
    else if (request.operation == "strength" &&
        (!std::isfinite(request.strength) || request.strength < 0.0f))
        error = "wind strength must be finite and non-negative (cm/s)";
    if (!error.empty())
        return false;
    if (request.operation == "strength")
        wind.strength = request.strength;
    return true;
}

inline nlohmann::json MakeSceneWindCommandReceipt(
    const WorldWind& wind, const SceneWindCommandRequest& request,
    float previousStrength, uint64_t worldGeneration, double steadySeconds)
{
    return {
        {"command", "wind"}, {"operation", request.operation}, {"status", "applied"},
        {"worldGeneration", worldGeneration}, {"steadyTimeSeconds", steadySeconds},
        {"runtimeOnly", true}, {"strengthOverrideApplied", request.operation == "strength"},
        {"windName", wind.name}, {"previousTargetStrengthCmPerSecond", previousStrength},
        {"targetStrengthCmPerSecond", wind.strength},
        {"direction", {wind.direction.x(), wind.direction.y(), wind.direction.z()}},
        {"gust", {{"enabled", wind.gust.enabled}, {"amplitude", wind.gust.amplitude},
            {"frequency", wind.gust.frequency}, {"turbulence", wind.gust.turbulence}}}
    };
}
}

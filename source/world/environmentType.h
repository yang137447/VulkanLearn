#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace VL
{
enum class EnvironmentType
{
    None,
    Hdri,
    ProceduralSky
};

inline EnvironmentType ParseEnvironmentType(std::string_view value)
{
    if (value == "none")
    {
        return EnvironmentType::None;
    }
    if (value == "hdri")
    {
        return EnvironmentType::Hdri;
    }
    if (value == "proceduralSky")
    {
        return EnvironmentType::ProceduralSky;
    }

    throw std::runtime_error("Unknown environment type: " + std::string(value));
}
} // namespace VL

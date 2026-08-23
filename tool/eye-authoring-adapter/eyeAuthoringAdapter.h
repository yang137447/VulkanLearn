#pragma once

#include <filesystem>

#include <nlohmann/json.hpp>

namespace VL
{

struct EyeAuthoringAdapterOptions
{
    bool strict = false;
};

struct EyeAuthoringAdapterResult
{
    nlohmann::json materialInstance;
    nlohmann::json report;
};

EyeAuthoringAdapterResult ConvertEyeAuthoring(
    const nlohmann::json& source,
    const EyeAuthoringAdapterOptions& options = {});

EyeAuthoringAdapterResult ConvertEyeAuthoringFile(
    const std::filesystem::path& sourcePath,
    const EyeAuthoringAdapterOptions& options = {});

} // namespace VL

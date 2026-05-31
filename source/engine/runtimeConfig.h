#pragma once

#include <string>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "platform/fileSystem.h"

namespace VL
{

// RuntimeConfig is the engine-facing view of config/config.json and related
// runtime files. FileSystem handles physical paths; RuntimeConfig handles the
// semantic fields engine startup needs.
class RuntimeConfig
{
public:
    RuntimeResult<void> Load();

    const nlohmann::json& GetConfigJson() const;
    const nlohmann::json& GetRenderGraphJson() const;

    const Eigen::Vector2f& GetWindowSize() const;
    float GetWindowAspectRatio() const;
    const std::string& GetInitialSceneRelativePath() const;
    const std::string& GetProjectPath() const;
    const std::string& GetResourcePath() const;

    std::string ResolvePath(const std::string& path) const;

private:
    void EnsureLoaded() const;
    RuntimeResult<void> LoadJsonFiles();
    RuntimeResult<void> LoadConfigFields();

    FileSystem fileSystem;
    bool loaded = false;
    nlohmann::json configJson;
    nlohmann::json renderGraphJson;
    Eigen::Vector2f windowSize = Eigen::Vector2f::Zero();
    std::string initialSceneRelativePath;
    std::string projectPath;
    std::string resourcePath;
};

} // namespace VL

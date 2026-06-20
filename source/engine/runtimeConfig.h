#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "platform/fileSystem.h"

namespace VL
{

struct CsmSettings
{
    bool enabled = false;
    uint32_t cascadeCount = 1;
    float shadowDistance = 10.0f;
    float splitLambda = 0.65f;
    bool lightSpaceCasterBounds = true;
    std::array<Eigen::Vector4f, 4> bias{};
};

// RuntimeConfig is the engine-facing view of config/config.json and related
// runtime files. FileSystem handles physical paths; RuntimeConfig handles the
// semantic fields engine startup needs.
class RuntimeConfig
{
public:
    RuntimeResult<void> Load();

    const nlohmann::json& GetRenderGraphJson() const;

    const Eigen::Vector2f& GetWindowSize() const;
    float GetWindowAspectRatio() const;
    const std::string& GetInitialSceneRelativePath() const;
    const std::string& GetResourcePath() const;
    bool ShouldUseRenderThread() const;
    const CsmSettings& GetCsmSettings() const;

    std::string ResolvePath(const std::string& path) const;

private:
    void EnsureLoaded() const;
    RuntimeResult<void> LoadJsonFiles();
    RuntimeResult<void> LoadConfigFields();
    RuntimeResult<void> LoadCsmSettings();
    RuntimeResult<void> ValidateCsmAgainstRenderGraph() const;

    FileSystem fileSystem;
    bool loaded = false;
    nlohmann::json configJson;
    nlohmann::json renderGraphJson;
    Eigen::Vector2f windowSize = Eigen::Vector2f::Zero();
    std::string initialSceneRelativePath;
    std::string projectPath;
    std::string resourcePath;
    int workerThreadCount = 1;
    CsmSettings csmSettings;
};

} // namespace VL

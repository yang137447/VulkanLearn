#include "engine/runtimeConfig.h"

#include <stdexcept>

namespace VL
{

namespace
{

RuntimeResult<Eigen::Vector2f> ReadVector2Field(
    const nlohmann::json& json,
    const char* fieldName)
{
    if (!json.contains(fieldName) || !json[fieldName].is_array() || json[fieldName].size() != 2)
    {
        return RuntimeResult<Eigen::Vector2f>::Failure(MakeRuntimeError(
            "RuntimeConfig.InvalidVector2",
            "Expected a two-element numeric array.",
            "config/config.json",
            fieldName));
    }

    Eigen::Vector2f value;
    value.x() = json[fieldName][0].get<float>();
    value.y() = json[fieldName][1].get<float>();
    return RuntimeResult<Eigen::Vector2f>::Success(value);
}

RuntimeResult<std::string> ReadStringField(
    const nlohmann::json& json,
    const char* fieldName)
{
    if (!json.contains(fieldName) || !json[fieldName].is_string())
    {
        return RuntimeResult<std::string>::Failure(MakeRuntimeError(
            "RuntimeConfig.InvalidString",
            "Expected a string field.",
            "config/config.json",
            fieldName));
    }

    return RuntimeResult<std::string>::Success(json[fieldName].get<std::string>());
}

RuntimeResult<int> ReadWorkerThreadCount(const nlohmann::json& json)
{
    if (json.contains("renderThread"))
    {
        return RuntimeResult<int>::Failure(MakeRuntimeError(
            "RuntimeConfig.DeprecatedRenderThreadConfig",
            "renderThread.enabled has been replaced by top-level workerThreadCount. Use 1 for synchronous GT-only mode or 2 for GT + RT mode.",
            "config/config.json",
            "renderThread"));
    }

    if (!json.contains("workerThreadCount"))
    {
        return RuntimeResult<int>::Success(1);
    }

    const nlohmann::json& workerThreadCountJson = json["workerThreadCount"];
    if (!workerThreadCountJson.is_number_integer() &&
        !workerThreadCountJson.is_number_unsigned())
    {
        return RuntimeResult<int>::Failure(MakeRuntimeError(
            "RuntimeConfig.InvalidWorkerThreadCount",
            "workerThreadCount must be an integer. V1 supports 1 for synchronous GT-only mode or 2 for GT + RT mode.",
            "config/config.json",
            "workerThreadCount"));
    }

    if (workerThreadCountJson.is_number_unsigned())
    {
        const uint64_t count = workerThreadCountJson.get<uint64_t>();
        if (count < 1)
        {
            return RuntimeResult<int>::Failure(MakeRuntimeError(
                "RuntimeConfig.InvalidWorkerThreadCount",
                "workerThreadCount must be at least 1. Use 1 for synchronous GT-only mode or 2 for GT + RT mode.",
                "config/config.json",
                "workerThreadCount"));
        }

        if (count > 2)
        {
            return RuntimeResult<int>::Failure(MakeRuntimeError(
                "RuntimeConfig.UnsupportedWorkerThreadCount",
                "workerThreadCount > 2 is reserved for future JobSystem/TaskGraph support. V1 supports only 1 or 2.",
                "config/config.json",
                "workerThreadCount"));
        }

        return RuntimeResult<int>::Success(static_cast<int>(count));
    }

    const int64_t count = workerThreadCountJson.get<int64_t>();
    if (count < 1)
    {
        return RuntimeResult<int>::Failure(MakeRuntimeError(
            "RuntimeConfig.InvalidWorkerThreadCount",
            "workerThreadCount must be at least 1. Use 1 for synchronous GT-only mode or 2 for GT + RT mode.",
            "config/config.json",
            "workerThreadCount"));
    }

    if (count > 2)
    {
        return RuntimeResult<int>::Failure(MakeRuntimeError(
            "RuntimeConfig.UnsupportedWorkerThreadCount",
            "workerThreadCount > 2 is reserved for future JobSystem/TaskGraph support. V1 supports only 1 or 2.",
            "config/config.json",
            "workerThreadCount"));
    }

    return RuntimeResult<int>::Success(static_cast<int>(count));
}

} // namespace

RuntimeResult<void> RuntimeConfig::Load()
{
    auto fileSystemResult = fileSystem.Initialize();
    if (fileSystemResult.IsFailure())
    {
        loaded = false;
        return fileSystemResult;
    }

    auto jsonResult = LoadJsonFiles();
    if (jsonResult.IsFailure())
    {
        loaded = false;
        return jsonResult;
    }

    auto fieldResult = LoadConfigFields();
    if (fieldResult.IsFailure())
    {
        loaded = false;
        return fieldResult;
    }

    if (windowSize.x() <= 0.0f || windowSize.y() <= 0.0f)
    {
        loaded = false;
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "RuntimeConfig.InvalidWindowSize",
            "config/config.json must define a positive windowSize."));
    }

    if (initialSceneRelativePath.empty())
    {
        loaded = false;
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "RuntimeConfig.EmptyInitialScene",
            "config/config.json must define a non-empty initScene."));
    }

    loaded = true;
    return RuntimeResult<void>::Success();
}

const nlohmann::json& RuntimeConfig::GetRenderGraphJson() const
{
    EnsureLoaded();
    return renderGraphJson;
}

const Eigen::Vector2f& RuntimeConfig::GetWindowSize() const
{
    EnsureLoaded();
    return windowSize;
}

float RuntimeConfig::GetWindowAspectRatio() const
{
    EnsureLoaded();
    return windowSize.x() / windowSize.y();
}

const std::string& RuntimeConfig::GetInitialSceneRelativePath() const
{
    EnsureLoaded();
    return initialSceneRelativePath;
}

const std::string& RuntimeConfig::GetResourcePath() const
{
    EnsureLoaded();
    return resourcePath;
}

bool RuntimeConfig::ShouldUseRenderThread() const
{
    EnsureLoaded();
    return workerThreadCount == 2;
}

std::string RuntimeConfig::ResolvePath(const std::string& path) const
{
    EnsureLoaded();
    auto resolvedPath = fileSystem.ResolveRuntimePath(path, resourcePath, projectPath);
    if (resolvedPath.IsFailure())
    {
        const RuntimeError& error = resolvedPath.Error();
        throw std::runtime_error(error.code + ": " + error.message + " [" + error.sourcePath + "]");
    }

    return resolvedPath.Value().string();
}

void RuntimeConfig::EnsureLoaded() const
{
    if (!loaded)
    {
        throw std::logic_error("RuntimeConfig must be loaded before use");
    }
}

RuntimeResult<void> RuntimeConfig::LoadJsonFiles()
{
    auto configResult = fileSystem.ReadJsonFile(fileSystem.GetConfigDirectory() / "config.json");
    if (configResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(configResult.Error());
    }
    configJson = std::move(configResult.Value());

    auto renderGraphResult = fileSystem.ReadJsonFile(fileSystem.GetConfigDirectory() / "renderGraphConfig.json");
    if (renderGraphResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(renderGraphResult.Error());
    }
    renderGraphJson = std::move(renderGraphResult.Value());

    return RuntimeResult<void>::Success();
}

RuntimeResult<void> RuntimeConfig::LoadConfigFields()
{
    auto windowSizeResult = ReadVector2Field(configJson, "windowSize");
    if (windowSizeResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(windowSizeResult.Error());
    }
    windowSize = windowSizeResult.Value();

    auto initialSceneResult = ReadStringField(configJson, "initScene");
    if (initialSceneResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(initialSceneResult.Error());
    }
    initialSceneRelativePath = std::move(initialSceneResult.Value());

    auto resourcePathResult = ReadStringField(configJson, "resourcePath");
    if (resourcePathResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(resourcePathResult.Error());
    }
    resourcePath = std::move(resourcePathResult.Value());

    if (configJson.contains("projectPath") && configJson["projectPath"].is_string())
    {
        projectPath = configJson["projectPath"].get<std::string>();
    }
    else
    {
        projectPath = fileSystem.GetProjectRoot().string();
    }

    auto workerThreadCountResult = ReadWorkerThreadCount(configJson);
    if (workerThreadCountResult.IsFailure())
    {
        return RuntimeResult<void>::Failure(workerThreadCountResult.Error());
    }
    workerThreadCount = workerThreadCountResult.Value();

    return RuntimeResult<void>::Success();
}

} // namespace VL

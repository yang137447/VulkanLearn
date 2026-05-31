#include "platform/fileSystem.h"

#include <fstream>
#include <stdexcept>

namespace VL
{

namespace
{

std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    if (std::filesystem::exists(path))
    {
        return std::filesystem::weakly_canonical(path);
    }

    return std::filesystem::absolute(path).lexically_normal();
}

RuntimeResult<std::filesystem::path> FindProjectRoot(const std::filesystem::path& searchStart)
{
    std::filesystem::path current = NormalizePath(searchStart);
    if (std::filesystem::is_regular_file(current))
    {
        current = current.parent_path();
    }

    while (!current.empty())
    {
        const std::filesystem::path configPath = current / "config" / "config.json";
        if (std::filesystem::exists(configPath))
        {
            return RuntimeResult<std::filesystem::path>::Success(current);
        }

        const std::filesystem::path parent = current.parent_path();
        if (parent == current)
        {
            break;
        }
        current = parent;
    }

    return RuntimeResult<std::filesystem::path>::Failure(MakeRuntimeError(
        "FileSystem.ProjectRootNotFound",
        "Could not find config/config.json from the current working directory.",
        searchStart.string()));
}

} // namespace

RuntimeResult<void> FileSystem::Initialize(const std::filesystem::path& searchStart)
{
    auto rootResult = FindProjectRoot(searchStart);
    if (rootResult.IsFailure())
    {
        initialized = false;
        return RuntimeResult<void>::Failure(rootResult.Error());
    }

    projectRoot = rootResult.Value();
    initialized = true;
    return RuntimeResult<void>::Success();
}

const std::filesystem::path& FileSystem::GetProjectRoot() const
{
    EnsureInitialized();
    return projectRoot;
}

std::filesystem::path FileSystem::GetConfigDirectory() const
{
    EnsureInitialized();
    return projectRoot / "config";
}

RuntimeResult<nlohmann::json> FileSystem::ReadJsonFile(const std::filesystem::path& absolutePath) const
{
    EnsureInitialized();

    std::ifstream file(absolutePath);
    if (!file.is_open())
    {
        return RuntimeResult<nlohmann::json>::Failure(MakeRuntimeError(
            "FileSystem.JsonOpenFailed",
            "Could not open JSON file.",
            absolutePath.string()));
    }

    try
    {
        nlohmann::json json;
        file >> json;
        return RuntimeResult<nlohmann::json>::Success(std::move(json));
    }
    catch (const std::exception& exception)
    {
        return RuntimeResult<nlohmann::json>::Failure(MakeRuntimeError(
            "FileSystem.JsonParseFailed",
            exception.what(),
            absolutePath.string()));
    }
}

RuntimeResult<std::filesystem::path> FileSystem::ResolveRuntimePath(
    const std::string& path,
    const std::filesystem::path& resourceRoot,
    const std::filesystem::path& projectRoot) const
{
    EnsureInitialized();

    const std::filesystem::path requestedPath(path);
    if (requestedPath.is_absolute() && std::filesystem::exists(requestedPath))
    {
        return RuntimeResult<std::filesystem::path>::Success(NormalizePath(requestedPath));
    }

    const std::filesystem::path resourcePath = resourceRoot / requestedPath;
    if (std::filesystem::exists(resourcePath))
    {
        return RuntimeResult<std::filesystem::path>::Success(NormalizePath(resourcePath));
    }

    const std::filesystem::path shaderSpvPath = projectRoot / "shader" / "spv" / requestedPath;
    if (std::filesystem::exists(shaderSpvPath))
    {
        return RuntimeResult<std::filesystem::path>::Success(NormalizePath(shaderSpvPath));
    }

    const std::filesystem::path projectPath = projectRoot / requestedPath;
    if (std::filesystem::exists(projectPath))
    {
        return RuntimeResult<std::filesystem::path>::Success(NormalizePath(projectPath));
    }

    return RuntimeResult<std::filesystem::path>::Failure(MakeRuntimeError(
        "FileSystem.PathNotFound",
        "Could not resolve runtime path.",
        path));
}

void FileSystem::EnsureInitialized() const
{
    if (!initialized)
    {
        throw std::logic_error("FileSystem must be initialized before use");
    }
}

} // namespace VL

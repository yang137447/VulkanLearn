#include "platform/fileSystem.h"
#include "core/runtimeFileTrace.h"

#include <fstream>
#include <cerrno>
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

std::filesystem::path NormalizeResolvedRuntimePath(const std::filesystem::path& path)
{
    try
    {
        const auto resolved = NormalizePath(path);
        RuntimeFileTrace::Record(RuntimeFileTrace::Event::ResolveResult,
            path, true, "FileSystem.ResolveRuntimePath", &resolved);
        return resolved;
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        RuntimeFileTrace::Record(RuntimeFileTrace::Event::ResolveResult,
            path, false, "FileSystem.ResolveRuntimePath", nullptr, std::nullopt,
            RuntimeFileTrace::Access::Read, error.code().value(), error.code().category().name());
        throw;
    }
    catch (...)
    {
        RuntimeFileTrace::Record(RuntimeFileTrace::Event::ResolveResult,
            path, false, "FileSystem.ResolveRuntimePath");
        throw;
    }
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
    RuntimeFileTrace::SetRoot(RuntimeFileTrace::RootScope::Project, projectRoot);
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

    const int previousErrno = errno;
    std::ifstream file(absolutePath);
    const auto openError = !file.is_open() && errno != 0 && errno != previousErrno ?
        std::optional<int64_t>(errno) : std::nullopt;
    RuntimeFileTrace::Record(RuntimeFileTrace::Event::StreamReadOpen,
        absolutePath, file.is_open(), "FileSystem.ReadJsonFile", nullptr, std::nullopt,
        RuntimeFileTrace::Access::Read, openError, openError ? "errno" : nullptr);
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
    const auto exists = [](const std::filesystem::path& candidate) {
        bool found;
        try
        {
            found = std::filesystem::exists(candidate);
        }
        catch (const std::filesystem::filesystem_error& error)
        {
            RuntimeFileTrace::Record(RuntimeFileTrace::Event::Resolve,
                candidate, false, "FileSystem.ResolveRuntimePath", nullptr, std::nullopt,
                RuntimeFileTrace::Access::Read, error.code().value(), error.code().category().name());
            throw;
        }
        catch (...)
        {
            RuntimeFileTrace::Record(RuntimeFileTrace::Event::Resolve,
                candidate, false, "FileSystem.ResolveRuntimePath");
            throw;
        }
        RuntimeFileTrace::Record(RuntimeFileTrace::Event::Resolve,
            candidate, found, "FileSystem.ResolveRuntimePath");
        return found;
    };
    if (requestedPath.is_absolute() && exists(requestedPath))
    {
        return RuntimeResult<std::filesystem::path>::Success(NormalizeResolvedRuntimePath(requestedPath));
    }

    const std::filesystem::path resourcePath = resourceRoot / requestedPath;
    if (exists(resourcePath))
    {
        return RuntimeResult<std::filesystem::path>::Success(NormalizeResolvedRuntimePath(resourcePath));
    }

    const std::filesystem::path shaderSpvPath = projectRoot / "shader" / "spv" / requestedPath;
    if (exists(shaderSpvPath))
    {
        return RuntimeResult<std::filesystem::path>::Success(NormalizeResolvedRuntimePath(shaderSpvPath));
    }

    const std::filesystem::path projectPath = projectRoot / requestedPath;
    if (exists(projectPath))
    {
        return RuntimeResult<std::filesystem::path>::Success(NormalizeResolvedRuntimePath(projectPath));
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

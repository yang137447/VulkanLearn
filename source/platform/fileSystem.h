#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"

namespace VL
{

// FileSystem owns project-root discovery and basic runtime file reads. Keeping
// path lookup here gives config/asset code one platform-facing boundary instead
// of scattering current-working-directory assumptions through engine systems.
class FileSystem
{
public:
    RuntimeResult<void> Initialize(
        const std::filesystem::path& searchStart = std::filesystem::current_path());

    const std::filesystem::path& GetProjectRoot() const;
    std::filesystem::path GetConfigDirectory() const;

    RuntimeResult<nlohmann::json> ReadJsonFile(const std::filesystem::path& absolutePath) const;
    RuntimeResult<std::filesystem::path> ResolveRuntimePath(
        const std::string& path,
        const std::filesystem::path& resourceRoot,
        const std::filesystem::path& projectRoot) const;

private:
    void EnsureInitialized() const;

    bool initialized = false;
    std::filesystem::path projectRoot;
};

} // namespace VL

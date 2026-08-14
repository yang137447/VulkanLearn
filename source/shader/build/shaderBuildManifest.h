#pragma once

// File responsibility: Defines the versioned shader build-cache manifest and
// its reverse dependency index. It stores only successfully committed artifact
// generations and never performs shader compilation or Vulkan work.

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace VL
{

inline constexpr uint32_t ShaderBuildCacheSchemaVersion = 1;
inline constexpr const char* ShaderBuildHashAlgorithm = "BLAKE3-256";
inline constexpr const char* ShaderBuildTargetEnvironment = "Vulkan1.4";

struct ShaderBuildSourceRecord
{
    std::string identity;
    std::string digest;
};

struct ShaderDependencyRecord
{
    std::string path;
    std::string digest;
    std::vector<std::string> requestingSources;
    uint32_t minimumIncludeDepth = 0;
};

struct ShaderBuildOutputRecord
{
    std::string path;
    std::string digest;
};

struct ShaderBuildArtifactRecord
{
    std::string logicalBuildId;
    std::string kind;
    std::string normalizedKey;
    std::string sourceFingerprint;
    std::vector<ShaderBuildSourceRecord> primarySources;
    std::vector<ShaderDependencyRecord> dependencies;
    std::map<std::string, ShaderBuildOutputRecord> outputs;
    std::string abiFingerprint;
};

struct ShaderBuildManifestSnapshot
{
    uint32_t schemaVersion = ShaderBuildCacheSchemaVersion;
    std::string hashAlgorithm = ShaderBuildHashAlgorithm;
    std::string compilePolicy;
    std::string targetEnvironment = ShaderBuildTargetEnvironment;
    std::unordered_map<std::string, ShaderBuildArtifactRecord> artifacts;
};

class ShaderBuildManifestStore
{
public:
    void Initialize(
        const std::filesystem::path& shaderRoot,
        std::string compilePolicy);

    std::optional<ShaderBuildArtifactRecord> FindArtifact(
        const std::string& logicalBuildId) const;
    void CommitArtifact(const ShaderBuildArtifactRecord& artifact);
    void CommitArtifacts(const std::vector<ShaderBuildArtifactRecord>& artifacts);
    ShaderBuildManifestSnapshot CaptureSnapshot() const;

    std::vector<std::string> FindLogicalBuildIdsDependingOn(
        const std::string& normalizedSourcePath) const;
    std::vector<std::string> GetAllLogicalBuildIds() const;

    const std::filesystem::path& GetShaderRoot() const { return shaderRoot; }
    const std::filesystem::path& GetGlslRoot() const { return glslRoot; }
    const std::filesystem::path& GetSpirvRoot() const { return spirvRoot; }
    std::string GetInitialInvalidationReason() const;

private:
    void Load();
    void SaveLocked() const;
    void RebuildReverseDependencyIndexLocked();

    mutable std::mutex mutex;
    std::filesystem::path shaderRoot;
    std::filesystem::path glslRoot;
    std::filesystem::path spirvRoot;
    std::filesystem::path manifestPath;
    ShaderBuildManifestSnapshot manifest;
    std::unordered_map<std::string, std::vector<std::string>> reverseDependencies;
    std::string initialInvalidationReason;
    bool initialized = false;
};

} // namespace VL

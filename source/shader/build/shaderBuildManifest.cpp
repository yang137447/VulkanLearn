#include "shader/build/shaderBuildManifest.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "shader/build/atomicFile.h"

namespace VL
{
namespace
{

nlohmann::json ToJson(const ShaderBuildArtifactRecord& artifact)
{
    nlohmann::json primarySources = nlohmann::json::array();
    for (const ShaderBuildSourceRecord& source : artifact.primarySources)
    {
        primarySources.push_back({
            {"identity", source.identity},
            {"digest", source.digest}});
    }

    nlohmann::json dependencies = nlohmann::json::array();
    for (const ShaderDependencyRecord& dependency : artifact.dependencies)
    {
        dependencies.push_back({
            {"path", dependency.path},
            {"digest", dependency.digest},
            {"requestingSources", dependency.requestingSources},
            {"minimumIncludeDepth", dependency.minimumIncludeDepth}});
    }

    nlohmann::json outputs = nlohmann::json::object();
    for (const auto& [role, output] : artifact.outputs)
    {
        outputs[role] = {
            {"path", output.path},
            {"digest", output.digest}};
    }

    return {
        {"kind", artifact.kind},
        {"normalizedKey", artifact.normalizedKey},
        {"sourceFingerprint", artifact.sourceFingerprint},
        {"primarySources", std::move(primarySources)},
        {"dependencies", std::move(dependencies)},
        {"outputs", std::move(outputs)},
        {"abiFingerprint", artifact.abiFingerprint}};
}

ShaderBuildArtifactRecord ParseArtifact(
    const std::string& logicalBuildId,
    const nlohmann::json& json)
{
    ShaderBuildArtifactRecord artifact;
    artifact.logicalBuildId = logicalBuildId;
    artifact.kind = json.at("kind").get<std::string>();
    artifact.normalizedKey = json.at("normalizedKey").get<std::string>();
    artifact.sourceFingerprint = json.at("sourceFingerprint").get<std::string>();
    artifact.abiFingerprint = json.at("abiFingerprint").get<std::string>();

    for (const nlohmann::json& sourceJson : json.at("primarySources"))
    {
        artifact.primarySources.push_back({
            sourceJson.at("identity").get<std::string>(),
            sourceJson.at("digest").get<std::string>()});
    }

    for (const nlohmann::json& dependencyJson : json.at("dependencies"))
    {
        ShaderDependencyRecord dependency;
        dependency.path = dependencyJson.at("path").get<std::string>();
        dependency.digest = dependencyJson.at("digest").get<std::string>();
        dependency.requestingSources =
            dependencyJson.value("requestingSources", std::vector<std::string>{});
        dependency.minimumIncludeDepth =
            dependencyJson.value("minimumIncludeDepth", 0u);
        artifact.dependencies.push_back(std::move(dependency));
    }

    for (const auto& [role, outputJson] : json.at("outputs").items())
    {
        artifact.outputs.emplace(
            role,
            ShaderBuildOutputRecord{
                outputJson.at("path").get<std::string>(),
                outputJson.at("digest").get<std::string>()});
    }
    return artifact;
}

} // namespace

void ShaderBuildManifestStore::Initialize(
    const std::filesystem::path& initializeShaderRoot,
    std::string compilePolicy)
{
    std::lock_guard<std::mutex> lock(mutex);
    shaderRoot = std::filesystem::absolute(initializeShaderRoot).lexically_normal();
    glslRoot = shaderRoot / "glsl";
    spirvRoot = shaderRoot / "spv";
    manifestPath = spirvRoot / "shader-build-cache.json";
    manifest = ShaderBuildManifestSnapshot{};
    manifest.compilePolicy = std::move(compilePolicy);
    reverseDependencies.clear();
    initialInvalidationReason.clear();
    initialized = true;
    Load();
}

std::optional<ShaderBuildArtifactRecord> ShaderBuildManifestStore::FindArtifact(
    const std::string& logicalBuildId) const
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto artifactIt = manifest.artifacts.find(logicalBuildId);
    if (artifactIt == manifest.artifacts.end())
    {
        return std::nullopt;
    }
    return artifactIt->second;
}

void ShaderBuildManifestStore::CommitArtifact(
    const ShaderBuildArtifactRecord& artifact)
{
    CommitArtifacts({artifact});
}

void ShaderBuildManifestStore::CommitArtifacts(
    const std::vector<ShaderBuildArtifactRecord>& artifacts)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!initialized)
    {
        throw std::runtime_error("Shader build manifest store is not initialized");
    }

    const ShaderBuildManifestSnapshot previousManifest = manifest;
    const auto previousReverseDependencies = reverseDependencies;
    try
    {
        for (const ShaderBuildArtifactRecord& artifact : artifacts)
        {
            manifest.artifacts[artifact.logicalBuildId] = artifact;
        }
        RebuildReverseDependencyIndexLocked();
        SaveLocked();
    }
    catch (...)
    {
        manifest = previousManifest;
        reverseDependencies = previousReverseDependencies;
        throw;
    }
}

ShaderBuildManifestSnapshot ShaderBuildManifestStore::CaptureSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return manifest;
}

std::vector<std::string> ShaderBuildManifestStore::FindLogicalBuildIdsDependingOn(
    const std::string& normalizedSourcePath) const
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto dependencyIt = reverseDependencies.find(normalizedSourcePath);
    if (dependencyIt == reverseDependencies.end())
    {
        return {};
    }
    return dependencyIt->second;
}

std::vector<std::string> ShaderBuildManifestStore::GetAllLogicalBuildIds() const
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> logicalBuildIds;
    logicalBuildIds.reserve(manifest.artifacts.size());
    for (const auto& artifactEntry : manifest.artifacts)
    {
        logicalBuildIds.push_back(artifactEntry.first);
    }
    std::sort(logicalBuildIds.begin(), logicalBuildIds.end());
    return logicalBuildIds;
}

std::string ShaderBuildManifestStore::GetInitialInvalidationReason() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return initialInvalidationReason;
}

void ShaderBuildManifestStore::Load()
{
    if (!std::filesystem::is_regular_file(manifestPath))
    {
        initialInvalidationReason = "manifest missing";
        return;
    }

    try
    {
        std::ifstream input(manifestPath);
        if (!input.is_open())
        {
            initialInvalidationReason = "manifest unreadable";
            return;
        }

        nlohmann::json json;
        input >> json;
        const uint32_t schemaVersion = json.at("schemaVersion").get<uint32_t>();
        const std::string hashAlgorithm =
            json.at("hashAlgorithm").get<std::string>();
        const std::string compilePolicy =
            json.at("compilePolicy").get<std::string>();
        const std::string targetEnvironment =
            json.at("targetEnvironment").get<std::string>();

        if (schemaVersion != ShaderBuildCacheSchemaVersion)
        {
            initialInvalidationReason = "cache schema version changed";
            return;
        }
        if (hashAlgorithm != ShaderBuildHashAlgorithm)
        {
            initialInvalidationReason = "hash algorithm changed";
            return;
        }
        if (compilePolicy != manifest.compilePolicy)
        {
            initialInvalidationReason = "compile policy changed";
            return;
        }
        if (targetEnvironment != ShaderBuildTargetEnvironment)
        {
            initialInvalidationReason = "target environment changed";
            return;
        }

        for (const auto& [logicalBuildId, artifactJson] :
             json.at("artifacts").items())
        {
            manifest.artifacts.emplace(
                logicalBuildId,
                ParseArtifact(logicalBuildId, artifactJson));
        }
        RebuildReverseDependencyIndexLocked();
    }
    catch (const std::exception&)
    {
        manifest.artifacts.clear();
        reverseDependencies.clear();
        initialInvalidationReason = "manifest corrupt";
    }
}

void ShaderBuildManifestStore::SaveLocked() const
{
    nlohmann::json artifacts = nlohmann::json::object();
    std::vector<std::string> logicalBuildIds;
    logicalBuildIds.reserve(manifest.artifacts.size());
    for (const auto& artifactEntry : manifest.artifacts)
    {
        logicalBuildIds.push_back(artifactEntry.first);
    }
    std::sort(logicalBuildIds.begin(), logicalBuildIds.end());
    for (const std::string& logicalBuildId : logicalBuildIds)
    {
        artifacts[logicalBuildId] = ToJson(manifest.artifacts.at(logicalBuildId));
    }

    const nlohmann::json json = {
        {"schemaVersion", manifest.schemaVersion},
        {"hashAlgorithm", manifest.hashAlgorithm},
        {"compilePolicy", manifest.compilePolicy},
        {"targetEnvironment", manifest.targetEnvironment},
        {"artifacts", std::move(artifacts)}};
    WriteTextFileAtomically(manifestPath, json.dump(2) + "\n");
}

void ShaderBuildManifestStore::RebuildReverseDependencyIndexLocked()
{
    reverseDependencies.clear();
    for (const auto& [logicalBuildId, artifact] : manifest.artifacts)
    {
        for (const ShaderBuildSourceRecord& source : artifact.primarySources)
        {
            reverseDependencies[source.identity].push_back(logicalBuildId);
        }
        for (const ShaderDependencyRecord& dependency : artifact.dependencies)
        {
            reverseDependencies[dependency.path].push_back(logicalBuildId);
        }
    }

    for (auto& [sourcePath, logicalBuildIds] : reverseDependencies)
    {
        std::sort(logicalBuildIds.begin(), logicalBuildIds.end());
        logicalBuildIds.erase(
            std::unique(logicalBuildIds.begin(), logicalBuildIds.end()),
            logicalBuildIds.end());
    }
}

} // namespace VL

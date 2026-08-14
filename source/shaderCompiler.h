#pragma once

// File responsibility: Coordinates deterministic GLSL builds, dependency
// tracking, persistent cache validation, and atomic artifact publication. It
// performs CPU work only and owns no Vulkan resources.

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "shader/build/atomicFile.h"
#include "shader/build/shaderBuildArtifact.h"
#include "shader/build/shaderBuildManifest.h"
#include "shaderVariant.h"

namespace VL
{
struct ComposedMaterialShaderSource;
struct MaterialShaderCompileRequest;
}

class ShaderCompiler
{
public:
    struct CandidateSourceValidationResult
    {
        bool current = true;
        std::string reason;
        std::string sourceIdentity;
        std::string capturedDigest;
        std::string currentDigest;
    };

    void Initialize(const std::filesystem::path& shaderRoot);

    VL::ShaderBuildStatistics StartCompile(
        const std::filesystem::path& shaderRoot,
        bool forceRebuild = false);

    VL::ShaderBuildArtifact EnsureGraphicsVariantCompiled(
        const ShaderVariantKey& shaderVariantKey,
        bool forceRebuild = false);
    VL::ShaderBuildArtifact EnsureMaterialGraphicsVariantCompiled(
        const VL::MaterialShaderCompileRequest& request,
        const VL::ComposedMaterialShaderSource& source,
        bool forceRebuild = false);
    VL::ShaderBuildArtifact EnsureComputeStageCompiled(
        const std::string& shaderName,
        bool forceRebuild = false);

    VL::ShaderBuildRequest CreateGraphicsVariantBuildRequest(
        const ShaderVariantKey& shaderVariantKey) const;
    VL::ShaderBuildRequest CreateMaterialGraphicsBuildRequest(
        const VL::MaterialShaderCompileRequest& request,
        const VL::ComposedMaterialShaderSource& source) const;
    VL::ShaderBuildRequest CreateComputeStageBuildRequestForShaderName(
        const std::string& shaderName) const;

    // Candidate builds stay in memory until the runtime transaction explicitly
    // publishes them after all CPU and Vulkan validation succeeds.
    VL::ShaderBuildArtifact CompileCandidate(
        const VL::ShaderBuildRequest& request) const;
    // Uses a valid formal cache entry when available; otherwise compiles an
    // in-memory candidate without publishing outputs or the manifest.
    VL::ShaderBuildArtifact PrepareCandidate(
        const VL::ShaderBuildRequest& request) const;
    CandidateSourceValidationResult ValidateCandidateSourcesStillCurrent(
        const VL::ShaderBuildRequest& request,
        const VL::ShaderBuildArtifact& artifact) const;
    // Snapshot-fills the request's include overlay with the current bytes of
    // every file under shader/glsl. Captured plans use this so asynchronous
    // compilation cannot observe newer disk content mid-batch.
    void FreezeSourceSnapshot(
        VL::ShaderBuildRequest& request) const;
    void CommitArtifacts(std::vector<VL::ShaderBuildArtifact>& artifacts);
    void CommitArtifactsWithAdditionalFiles(
        std::vector<VL::ShaderBuildArtifact>& artifacts,
        const std::vector<VL::AtomicFileWrite>& additionalFiles);

    std::vector<std::string> FindLogicalBuildIdsDependingOn(
        const std::string& normalizedSourcePath) const;
    std::vector<std::string> FindChangedSourcePaths() const;
    VL::ShaderBuildManifestSnapshot CaptureManifestSnapshot() const;

    const VL::ShaderBuildStatistics& GetLastStatistics() const
    {
        return lastStatistics;
    }
    const std::filesystem::path& GetShaderRoot() const { return shaderRoot; }

    static std::string GetCompilePolicy();
    static std::string FormatStatistics(
        const VL::ShaderBuildStatistics& statistics);

private:
    VL::ShaderBuildArtifact EnsureBuild(
        const VL::ShaderBuildRequest& request,
        bool forceRebuild);
    VL::ShaderBuildArtifact CompileRequest(
        const VL::ShaderBuildRequest& request) const;
    bool TryLoadCachedArtifact(
        const VL::ShaderBuildRequest& request,
        VL::ShaderBuildArtifact& artifact,
        std::string& missReason) const;

    VL::ShaderBuildRequest CreateComputeStageBuildRequest(
        const std::filesystem::path& sourcePath) const;
    std::string NormalizeSourceIdentity(
        const std::filesystem::path& sourcePath) const;
    std::string NormalizeOutputIdentity(
        const std::filesystem::path& outputPath) const;
    std::string BuildLogicalBuildId(
        VL::ShaderBuildKind kind,
        const std::string& normalizedKey) const;
    void RequireInitialized() const;

    std::filesystem::path shaderRoot;
    std::filesystem::path glslRoot;
    std::filesystem::path spirvRoot;
    std::string compilePolicy;
    VL::ShaderBuildManifestStore manifestStore;
    VL::ShaderBuildStatistics lastStatistics;
    std::mutex artifactPublicationMutex;
    bool initialized = false;
};

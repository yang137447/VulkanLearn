#include "shaderCompiler.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <shaderc/shaderc.hpp>

#include "core/logStore.h"
#include "material/compiler/materialShaderCompileRequest.h"
#include "material/compiler/materialShaderComposer.h"
#include "pipeline/shaderReflectionService.h"
#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"
#include "shaderCompilerInternal.h"

using namespace VL::ShaderCompilerDetail;

void ShaderCompiler::CommitArtifacts(
    std::vector<VL::ShaderBuildArtifact>& artifacts)
{
    CommitArtifactsWithAdditionalFiles(artifacts, {});
}

void ShaderCompiler::CommitArtifactsWithAdditionalFiles(
    std::vector<VL::ShaderBuildArtifact>& artifacts,
    const std::vector<VL::AtomicFileWrite>& additionalFiles)
{
    struct ArtifactOutputIdentity
    {
        std::string logicalBuildId;
        std::string normalizedKey;
        std::string role;
        std::string digest;
        bool cacheHit = false;
    };

    struct OutputBackup
    {
        std::filesystem::path path;
        bool existed = false;
        std::vector<uint8_t> bytes;
    };

    RequireInitialized();
    std::lock_guard<std::mutex> publicationLock(
        artifactPublicationMutex);
    std::vector<VL::AtomicFileWrite> writes;
    std::vector<VL::ShaderBuildArtifactRecord> records;
    std::vector<OutputBackup> outputBackups;
    std::set<PhysicalPathIdentity> outputPaths;
    std::map<PhysicalPathIdentity, ArtifactOutputIdentity>
        artifactOutputsByPath;

    for (const VL::AtomicFileWrite& additionalFile :
         additionalFiles)
    {
        const std::filesystem::path normalizedPath =
            NormalizeAbsolutePath(additionalFile.path);
        const PhysicalPathIdentity physicalPathIdentity =
            BuildPhysicalPathIdentity(normalizedPath);
        if (!outputPaths.insert(physicalPathIdentity).second)
        {
            throw std::runtime_error(
                "Shader publication batch contains duplicate output path: " +
                normalizedPath.string());
        }
        writes.push_back({
            normalizedPath,
            additionalFile.bytes});
        OutputBackup backup;
        backup.path = normalizedPath;
        backup.existed =
            std::filesystem::is_regular_file(normalizedPath);
        if (backup.existed)
        {
            backup.bytes =
                VL::ReadBinaryFile(normalizedPath);
        }
        outputBackups.push_back(std::move(backup));
    }

    for (const VL::ShaderBuildArtifact& artifact : artifacts)
    {
        for (const auto& [role, output] : artifact.outputs)
        {
            const std::filesystem::path normalizedPath =
                NormalizeAbsolutePath(output.path);
            const PhysicalPathIdentity physicalPathIdentity =
                BuildPhysicalPathIdentity(normalizedPath);
            const ArtifactOutputIdentity outputIdentity{
                artifact.logicalBuildId,
                artifact.normalizedKey,
                role,
                output.digest,
                artifact.cacheHit};
            const auto [existingIt, inserted] =
                artifactOutputsByPath.emplace(
                    physicalPathIdentity,
                    outputIdentity);
            if (!inserted)
            {
                const ArtifactOutputIdentity& existing =
                    existingIt->second;
                throw std::runtime_error(
                    "Shader artifact batch contains conflicting output path before publication: " +
                    normalizedPath.string() +
                    "; firstLogicalBuildId=" +
                    existing.logicalBuildId +
                    "; firstNormalizedKey=" +
                    existing.normalizedKey +
                    "; firstRole=" +
                    existing.role +
                    "; firstDigest=" +
                    existing.digest +
                    "; firstCacheHit=" +
                    std::to_string(existing.cacheHit) +
                    "; secondLogicalBuildId=" +
                    outputIdentity.logicalBuildId +
                    "; secondNormalizedKey=" +
                    outputIdentity.normalizedKey +
                    "; secondRole=" +
                    outputIdentity.role +
                    "; secondDigest=" +
                    outputIdentity.digest +
                    "; secondCacheHit=" +
                    std::to_string(outputIdentity.cacheHit));
            }
        }

        if (artifact.cacheHit)
        {
            for (const auto& [role, output] : artifact.outputs)
            {
                const std::filesystem::path normalizedPath =
                    NormalizeAbsolutePath(output.path);
                if (!std::filesystem::is_regular_file(
                        normalizedPath))
                {
                    throw std::runtime_error(
                        "Cached shader output disappeared before publication: " +
                        normalizedPath.string() +
                        "; logicalBuildId=" +
                        artifact.logicalBuildId +
                        "; normalizedKey=" +
                        artifact.normalizedKey +
                        "; role=" + role);
                }
                const std::string currentDigest =
                    VL::ContentHasher::HashFile(
                        normalizedPath).ToHex();
                if (currentDigest != output.digest)
                {
                    throw std::runtime_error(
                        "Cached shader output changed before publication: " +
                        normalizedPath.string() +
                        "; logicalBuildId=" +
                        artifact.logicalBuildId +
                        "; normalizedKey=" +
                        artifact.normalizedKey +
                        "; role=" + role +
                        "; capturedDigest=" +
                        output.digest +
                        "; currentDigest=" +
                        currentDigest);
                }
            }
            continue;
        }

        for (const auto& [role, output] : artifact.outputs)
        {
            const std::filesystem::path normalizedPath =
                NormalizeAbsolutePath(output.path);
            const PhysicalPathIdentity physicalPathIdentity =
                BuildPhysicalPathIdentity(normalizedPath);
            if (!outputPaths.insert(physicalPathIdentity).second)
            {
                throw std::runtime_error(
                    "Shader artifact batch contains duplicate output path: " +
                    normalizedPath.string());
            }
            writes.push_back({
                normalizedPath,
                SpirvToBytes(output.spirv)});
            OutputBackup backup;
            backup.path = normalizedPath;
            backup.existed =
                std::filesystem::is_regular_file(normalizedPath);
            if (backup.existed)
            {
                backup.bytes =
                    VL::ReadBinaryFile(normalizedPath);
            }
            outputBackups.push_back(std::move(backup));
        }
        records.push_back(BuildManifestRecord(artifact, *this));
    }

    if (records.empty() && writes.empty())
    {
        for (VL::ShaderBuildArtifact& artifact : artifacts)
        {
            artifact.committed = true;
        }
        return;
    }

    try
    {
        VL::WriteFileBatchAtomically(writes);
        for (const VL::ShaderBuildArtifact& artifact :
             artifacts)
        {
            for (const auto& [role, output] :
                 artifact.outputs)
            {
                (void)role;
                const std::string committedDigest =
                    VL::ContentHasher::HashFile(
                        output.path).ToHex();
                if (committedDigest != output.digest)
                {
                    throw std::runtime_error(
                        "Shader output digest verification failed after commit: " +
                        output.path.string());
                }
            }
        }

        if (!records.empty())
        {
            manifestStore.CommitArtifacts(records);
        }
    }
    catch (...)
    {
        std::vector<VL::AtomicFileWrite> restoreWrites;
        for (const OutputBackup& backup : outputBackups)
        {
            if (backup.existed)
            {
                restoreWrites.push_back({
                    backup.path,
                    backup.bytes});
            }
        }

        try
        {
            if (!restoreWrites.empty())
            {
                VL::WriteFileBatchAtomically(restoreWrites);
            }
            for (const OutputBackup& backup : outputBackups)
            {
                if (!backup.existed)
                {
                    std::error_code removeError;
                    std::filesystem::remove(
                        backup.path,
                        removeError);
                    if (removeError)
                    {
                        throw std::runtime_error(
                            "Failed to remove new shader output while rolling back: " +
                            backup.path.string() + ": " +
                            removeError.message());
                    }
                }
            }
        }
        catch (const std::exception& rollbackException)
        {
            throw std::runtime_error(
                "Shader artifact commit failed and output rollback also failed: " +
                std::string(rollbackException.what()));
        }
        throw;
    }

    for (VL::ShaderBuildArtifact& artifact : artifacts)
    {
        artifact.committed = true;
    }
}

std::vector<std::string>
ShaderCompiler::FindLogicalBuildIdsDependingOn(
    const std::string& normalizedSourcePath) const
{
    RequireInitialized();
    return manifestStore.FindLogicalBuildIdsDependingOn(
        std::filesystem::path(normalizedSourcePath)
            .lexically_normal()
            .generic_string());
}

std::vector<std::string> ShaderCompiler::FindChangedSourcePaths() const
{
    RequireInitialized();
    const VL::ShaderBuildManifestSnapshot snapshot =
        manifestStore.CaptureSnapshot();
    std::map<std::string, std::string> recordedDigests;
    for (const auto& [logicalBuildId, artifact] : snapshot.artifacts)
    {
        (void)logicalBuildId;
        for (const VL::ShaderBuildSourceRecord& source :
             artifact.primarySources)
        {
            if (source.identity.rfind("__composed__/", 0) == 0)
            {
                // Composed stage identities are diagnostic virtual paths. Their
                // source bytes are rebuilt from the immutable Material recipe;
                // only real source-of-truth dependencies participate in the
                // changed-file scan.
                continue;
            }
            const std::filesystem::path sourcePath =
                glslRoot / source.identity;
            if (std::filesystem::is_regular_file(sourcePath))
            {
                recordedDigests.emplace(source.identity, source.digest);
            }
        }
        for (const VL::ShaderDependencyRecord& dependency :
             artifact.dependencies)
        {
            recordedDigests.emplace(
                dependency.path,
                dependency.digest);
        }
    }

    std::vector<std::string> changedPaths;
    for (const auto& [sourceIdentity, recordedDigest] : recordedDigests)
    {
        const std::filesystem::path sourcePath =
            glslRoot / sourceIdentity;
        if (!std::filesystem::is_regular_file(sourcePath))
        {
            changedPaths.push_back(sourceIdentity);
            continue;
        }

        if (VL::ContentHasher::HashFile(sourcePath).ToHex() !=
            recordedDigest)
        {
            changedPaths.push_back(sourceIdentity);
        }
    }
    return changedPaths;
}

VL::ShaderBuildManifestSnapshot
ShaderCompiler::CaptureManifestSnapshot() const
{
    RequireInitialized();
    return manifestStore.CaptureSnapshot();
}

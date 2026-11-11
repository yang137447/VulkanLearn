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

bool ShaderCompiler::TryLoadCachedArtifact(
    const VL::ShaderBuildRequest& request,
    VL::ShaderBuildArtifact& artifact,
    std::string& missReason) const
{
    const std::optional<VL::ShaderBuildArtifactRecord> recordOptional =
        manifestStore.FindArtifact(request.logicalBuildId);
    if (!recordOptional.has_value())
    {
        missReason = manifestStore.GetInitialInvalidationReason();
        if (missReason.empty())
        {
            missReason = "artifact missing";
        }
        return false;
    }

    const VL::ShaderBuildArtifactRecord& record = *recordOptional;
    if (record.kind != VL::ShaderBuildKindToString(request.kind) ||
        record.normalizedKey != request.normalizedKey)
    {
        missReason = "logical build key changed";
        return false;
    }

    const std::vector<VL::ShaderBuildSourceRecord> primarySources =
        BuildPrimarySourceRecords(request);
    if (!EqualPrimarySources(record.primarySources, primarySources))
    {
        missReason = "primary source changed";
        return false;
    }

    std::vector<VL::ShaderDependencyRecord> dependencies =
        record.dependencies;
    std::sort(
        dependencies.begin(),
        dependencies.end(),
        [](const VL::ShaderDependencyRecord& lhs,
           const VL::ShaderDependencyRecord& rhs)
        {
            return lhs.path < rhs.path;
        });
    for (VL::ShaderDependencyRecord& dependency : dependencies)
    {
        std::string currentDigest;
        const auto frozenIt =
            request.sourceSnapshot.find(
                dependency.path);
        if (frozenIt !=
            request.sourceSnapshot.end())
        {
            currentDigest =
                VL::ContentHasher::HashString(
                    frozenIt->second).ToHex();
        }
        else
        {
            const std::filesystem::path dependencyPath =
                glslRoot / dependency.path;
            if (!std::filesystem::is_regular_file(
                    dependencyPath))
            {
                missReason =
                    "dependency missing: " +
                    dependency.path;
                return false;
            }
            currentDigest =
                VL::ContentHasher::HashFile(
                    dependencyPath).ToHex();
        }
        if (currentDigest != dependency.digest)
        {
            missReason =
                "dependency changed: " + dependency.path;
            return false;
        }
    }

    const std::string sourceFingerprint = BuildSourceFingerprint(
        request,
        primarySources,
        dependencies,
        compilePolicy);
    if (sourceFingerprint != record.sourceFingerprint)
    {
        missReason = "source fingerprint changed";
        return false;
    }

    if (record.outputs.size() != request.outputPaths.size())
    {
        missReason = "artifact output set changed";
        return false;
    }

    artifact = {};
    artifact.kind = request.kind;
    artifact.logicalBuildId = request.logicalBuildId;
    artifact.normalizedKey = request.normalizedKey;
    artifact.sourceFingerprint = sourceFingerprint;
    artifact.primarySources = primarySources;
    artifact.dependencies = dependencies;
    artifact.abiFingerprint = record.abiFingerprint;

    for (const auto& [role, expectedPathInput] : request.outputPaths)
    {
        const auto outputRecordIt = record.outputs.find(role);
        if (outputRecordIt == record.outputs.end())
        {
            missReason = "artifact output missing from manifest: " + role;
            return false;
        }

        const std::filesystem::path expectedPath =
            NormalizeAbsolutePath(expectedPathInput);
        const std::string expectedIdentity =
            NormalizeOutputIdentity(expectedPath);
        if (outputRecordIt->second.path != expectedIdentity)
        {
            missReason = "artifact output path changed: " + role;
            return false;
        }
        if (!std::filesystem::is_regular_file(expectedPath))
        {
            missReason = "artifact output missing: " + expectedIdentity;
            return false;
        }

        const std::vector<uint8_t> bytes =
            VL::ReadBinaryFile(expectedPath);
        const std::string digest =
            VL::ContentHasher::HashBytes(
                bytes.data(),
                bytes.size()).ToHex();
        if (digest != outputRecordIt->second.digest)
        {
            missReason =
                "artifact output digest mismatch: " + expectedIdentity;
            return false;
        }
        artifact.outputs.emplace(
            role,
            VL::ShaderBuildOutput{
                expectedPath,
                BytesToSpirv(bytes, expectedPath),
                digest});
    }

    std::vector<std::vector<uint32_t>> debugCodes;
    for (const std::string& debugRole : BuildDebugRoles(request))
    {
        debugCodes.push_back(artifact.outputs.at(debugRole).spirv);
    }
    try
    {
        const ShaderReflectionResult reflection =
            ShaderReflectionService::ReflectDetailedFromDebugSpirvCode(
                debugCodes);
        artifact.shaderBindings = reflection.shaderBindings;
        artifact.abiSignature = reflection.abiSignature;
        const std::string reflectedAbiFingerprint =
            artifact.abiSignature.GetFingerprint();
        if (reflectedAbiFingerprint != record.abiFingerprint)
        {
            missReason = "ABI fingerprint changed";
            return false;
        }
    }
    catch (const std::exception&)
    {
        missReason = "cached reflection failed";
        return false;
    }

    artifact.artifactGenerationKey =
        BuildArtifactGenerationKey(artifact);
    return true;
}

VL::ShaderBuildRequest ShaderCompiler::CreateComputeStageBuildRequest(
    const std::filesystem::path& sourcePath) const
{
    RequireInitialized();
    const std::string sourceIdentity =
        NormalizeSourceIdentity(sourcePath);
    std::filesystem::path shaderNamePath(sourceIdentity);
    shaderNamePath.replace_extension();
    const std::string shaderName = shaderNamePath.generic_string();

    VL::ShaderBuildRequest request;
    request.kind = VL::ShaderBuildKind::StageEntry;
    request.normalizedKey =
        "kind=StageEntry|path=" + sourceIdentity +
        "|stage=compute|macros=|target=" +
        VL::ShaderBuildTargetEnvironment;
    request.logicalBuildId =
        BuildLogicalBuildId(request.kind, request.normalizedKey);
    request.stages = {
        {
            "compute",
            sourceIdentity,
            NormalizeAbsolutePath(sourcePath),
            ReadTextFile(sourcePath),
            shaderc_compute_shader}};
    request.outputPaths = {
        {RuntimeComputeRole, spirvRoot / (shaderName + "_comp.spv")},
        {DebugComputeRole, spirvRoot / (shaderName + "_comp.debug")}};
    return request;
}

std::string ShaderCompiler::NormalizeSourceIdentity(
    const std::filesystem::path& sourcePath) const
{
    RequireInitialized();
    const std::filesystem::path normalized =
        NormalizeAbsolutePath(sourcePath);
    if (!IsPathInsideRoot(normalized, glslRoot))
    {
        throw std::runtime_error(
            "Shader source must live under shader/glsl: " +
            normalized.string());
    }
    return std::filesystem::relative(normalized, glslRoot)
        .generic_string();
}

std::string ShaderCompiler::NormalizeOutputIdentity(
    const std::filesystem::path& outputPath) const
{
    RequireInitialized();
    const std::filesystem::path normalized =
        NormalizeAbsolutePath(outputPath);
    if (!IsPathInsideRoot(normalized, shaderRoot))
    {
        throw std::runtime_error(
            "Shader output must live under the shader root: " +
            normalized.string());
    }
    return std::filesystem::relative(normalized, shaderRoot)
        .generic_string();
}

std::string ShaderCompiler::BuildLogicalBuildId(
    VL::ShaderBuildKind kind,
    const std::string& normalizedKey) const
{
    VL::CanonicalFieldHasher hasher("ShaderLogicalBuildIdV1");
    hasher.AddUInt32(
        "cacheSchemaVersion",
        VL::ShaderBuildCacheSchemaVersion);
    hasher.AddString("compilePolicy", compilePolicy);
    hasher.AddString(
        "targetEnvironment",
        VL::ShaderBuildTargetEnvironment);
    hasher.AddUInt32("kind", static_cast<uint32_t>(kind));
    hasher.AddString("normalizedKey", normalizedKey);
    return hasher.Finalize().ToHex();
}

void ShaderCompiler::RequireInitialized() const
{
    if (!initialized)
    {
        throw std::runtime_error("ShaderCompiler is not initialized");
    }
}

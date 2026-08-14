#pragma once

// File responsibility: Defines immutable CPU-side shader build requests and
// candidate artifacts shared by startup compilation, manual reload, and the
// asynchronous compile worker. It owns no Vulkan resources.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <shaderc/shaderc.h>

#include "shader/build/shaderBuildManifest.h"
#include "shader/shaderAbiSignature.h"
#include "shaderReflect.h"

namespace VL
{

enum class ShaderBuildKind
{
    StageEntry,
    GraphicsPair,
    MaterialGraphicsPair
};

inline const char* ShaderBuildKindToString(ShaderBuildKind kind)
{
    switch (kind)
    {
    case ShaderBuildKind::StageEntry:
        return "StageEntry";
    case ShaderBuildKind::GraphicsPair:
        return "GraphicsPair";
    case ShaderBuildKind::MaterialGraphicsPair:
        return "MaterialGraphicsPair";
    }
    return "Unknown";
}

struct ShaderStageBuildSource
{
    std::string stageName;
    std::string sourceIdentity;
    std::filesystem::path virtualSourcePath;
    std::string sourceCode;
    shaderc_shader_kind shaderKind = shaderc_glsl_infer_from_source;
};

struct ShaderBuildRequest
{
    ShaderBuildKind kind = ShaderBuildKind::StageEntry;
    std::string logicalBuildId;
    std::string normalizedKey;
    std::vector<std::string> macros;
    std::vector<ShaderStageBuildSource> stages;
    std::map<std::string, std::filesystem::path> outputPaths;
    // Optional frozen include overlay: normalized identity -> file bytes. When
    // present, shaderc resolves includes from this snapshot instead of disk so
    // a captured reload plan compiles exactly the source generation it saw.
    std::map<std::string, std::string> sourceSnapshot;
    // Generated candidate inputs intentionally differ from the current formal
    // file. Commit-time validation compares these dependencies with the
    // captured overlay while their source-of-truth M_ digest is validated
    // separately through validationSourceDigests.
    std::map<std::string, std::string> candidateOverlayDigests;
    // Source-of-truth inputs that influence request composition but are not
    // consumed by shaderc as includes, such as the M_ material definition.
    // Commit-time validation re-hashes these identities before publishing.
    std::map<std::string, std::string> validationSourceDigests;
};

struct ShaderBuildOutput
{
    std::filesystem::path path;
    std::vector<uint32_t> spirv;
    std::string digest;
};

struct ShaderBuildArtifact
{
    ShaderBuildKind kind = ShaderBuildKind::StageEntry;
    std::string logicalBuildId;
    std::string normalizedKey;
    std::string sourceFingerprint;
    std::string artifactGenerationKey;
    std::vector<ShaderBuildSourceRecord> primarySources;
    std::vector<ShaderDependencyRecord> dependencies;
    std::map<std::string, ShaderBuildOutput> outputs;
    std::vector<ShaderBinding> shaderBindings;
    ShaderAbiSignature abiSignature;
    std::string abiFingerprint;
    uint32_t shadercInvocations = 0;
    bool cacheHit = false;
    bool committed = false;
    std::string cacheReason;
};

struct ShaderBuildStatistics
{
    uint64_t entries = 0;
    uint64_t artifacts = 0;
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
    uint64_t compiledArtifacts = 0;
    uint64_t failedArtifacts = 0;
    uint64_t shadercInvocations = 0;
    double elapsedMilliseconds = 0.0;
};

} // namespace VL

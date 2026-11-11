#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "shaderCompiler.h"

namespace VL
{
namespace ShaderCompilerDetail
{

inline constexpr const char* RuntimeVertexRole = "runtimeVertex";
inline constexpr const char* RuntimeFragmentRole = "runtimeFragment";
inline constexpr const char* RuntimeComputeRole = "runtimeCompute";
inline constexpr const char* DebugVertexRole = "debugVertex";
inline constexpr const char* DebugFragmentRole = "debugFragment";
inline constexpr const char* DebugComputeRole = "debugCompute";

std::string ReadTextFile(const std::filesystem::path& path);

std::vector<uint8_t> SpirvToBytes(const std::vector<uint32_t>& spirv);

std::vector<uint32_t> BytesToSpirv(
    const std::vector<uint8_t>& bytes,
    const std::filesystem::path& path);

std::vector<std::string> BuildGraphicsVariantMacros(
    const ShaderVariantKey& shaderVariantKey);

bool IsPathInsideRoot(
    const std::filesystem::path& path,
    const std::filesystem::path& root);

std::filesystem::path NormalizeAbsolutePath(
    const std::filesystem::path& path);

#if defined(_WIN32)
using PhysicalPathIdentity = std::wstring;
#else
using PhysicalPathIdentity = std::string;
#endif

PhysicalPathIdentity BuildPhysicalPathIdentity(
    const std::filesystem::path& path);

std::vector<VL::ShaderBuildSourceRecord> BuildPrimarySourceRecords(
    const VL::ShaderBuildRequest& request);

std::string BuildSourceFingerprint(
    const VL::ShaderBuildRequest& request,
    const std::vector<VL::ShaderBuildSourceRecord>& primarySources,
    const std::vector<VL::ShaderDependencyRecord>& dependencies,
    const std::string& compilePolicy);

std::string BuildArtifactGenerationKey(
    const VL::ShaderBuildArtifact& artifact);

bool EqualPrimarySources(
    const std::vector<VL::ShaderBuildSourceRecord>& lhs,
    const std::vector<VL::ShaderBuildSourceRecord>& rhs);

VL::ShaderBuildArtifactRecord BuildManifestRecord(
    const VL::ShaderBuildArtifact& artifact,
    const ShaderCompiler& compiler);

std::vector<std::string> BuildDebugRoles(
    const VL::ShaderBuildRequest& request);

} // namespace ShaderCompilerDetail
} // namespace VL

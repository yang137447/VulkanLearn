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

namespace VL
{
namespace ShaderCompilerDetail
{

std::string ReadTextFile(const std::filesystem::path& path)
{
    const std::vector<uint8_t> bytes = VL::ReadBinaryFile(path);
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
}

std::vector<uint8_t> SpirvToBytes(const std::vector<uint32_t>& spirv)
{
    std::vector<uint8_t> bytes(spirv.size() * sizeof(uint32_t));
    if (!bytes.empty())
    {
        std::memcpy(bytes.data(), spirv.data(), bytes.size());
    }
    return bytes;
}

std::vector<uint32_t> BytesToSpirv(
    const std::vector<uint8_t>& bytes,
    const std::filesystem::path& path)
{
    if (bytes.size() % sizeof(uint32_t) != 0)
    {
        throw std::runtime_error(
            "SPIR-V file size is not aligned to 32-bit words: " +
            path.string());
    }

    std::vector<uint32_t> spirv(bytes.size() / sizeof(uint32_t));
    if (!bytes.empty())
    {
        std::memcpy(spirv.data(), bytes.data(), bytes.size());
    }
    return spirv;
}

std::string JoinMacros(const std::vector<std::string>& macros)
{
    std::ostringstream stream;
    for (size_t index = 0; index < macros.size(); ++index)
    {
        if (index > 0)
        {
            stream << ",";
        }
        stream << macros[index];
    }
    return stream.str();
}

std::vector<std::string> BuildRenderModeMacros(RenderMode renderMode)
{
    switch (renderMode)
    {
    case RenderMode::Opaque:
        return {"RENDER_MODE_OPAQUE"};
    case RenderMode::OpaqueClip:
        return {"RENDER_MODE_OPAQUE_CLIP"};
    case RenderMode::ForwardOpaque:
        return {"RENDER_MODE_FORWARD_OPAQUE"};
    case RenderMode::TransparentAlphaBlend:
        return {"RENDER_MODE_TRANSPARENT_ALPHA_BLEND"};
    case RenderMode::TransparentAlphaBlendWriteDepth:
        return {"RENDER_MODE_TRANSPARENT_ALPHA_BLEND_WRITE_DEPTH"};
    case RenderMode::TransparentAdditive:
        return {"RENDER_MODE_TRANSPARENT_ADDITIVE"};
    case RenderMode::ThinTranslucent:
        // 具体是否输出第二颜色源由独立能力宏决定；RenderMode 宏只选择闭包输出壳。
        return {"RENDER_MODE_THIN_TRANSLUCENT"};
    }
    throw std::runtime_error("Unknown RenderMode when building shader macros");
}

std::vector<std::string> BuildGraphicsVariantMacros(
    const ShaderVariantKey& shaderVariantKey)
{
    std::vector<std::string> macros =
        BuildRenderModeMacros(shaderVariantKey.renderMode);
    macros.push_back(
        "MATERIAL_SHADING_MODEL=" + shaderVariantKey.shadingModelMacro);
    // 旧实现清理（2026-09-12）删掉了这里注入的 MATERIAL_IS_EYE：它只服务 Eye 的
    // 材质专用资源声明，Eye 求值器删除后没有任何 shader 消费它。重建需要"按 shading model
    // 选择资源声明"的模型时，在这里重新引入同类的数值宏（GLSL 的 shading model 常量是
    // const uint，不能用于预处理器 #if）。
    macros.insert(
        macros.end(),
        shaderVariantKey.macros.begin(),
        shaderVariantKey.macros.end());
    return NormalizeMaterialMacros(std::move(macros));
}

std::string StageNameForKind(shaderc_shader_kind kind)
{
    switch (kind)
    {
    case shaderc_vertex_shader:
        return "vertex";
    case shaderc_fragment_shader:
        return "fragment";
    case shaderc_compute_shader:
        return "compute";
    default:
        throw std::runtime_error("Unsupported shader stage in build request");
    }
}

const char* RuntimeRoleForStage(const std::string& stageName)
{
    if (stageName == "vertex")
    {
        return RuntimeVertexRole;
    }
    if (stageName == "fragment")
    {
        return RuntimeFragmentRole;
    }
    if (stageName == "compute")
    {
        return RuntimeComputeRole;
    }
    throw std::runtime_error("Unsupported shader stage name: " + stageName);
}

const char* DebugRoleForStage(const std::string& stageName)
{
    if (stageName == "vertex")
    {
        return DebugVertexRole;
    }
    if (stageName == "fragment")
    {
        return DebugFragmentRole;
    }
    if (stageName == "compute")
    {
        return DebugComputeRole;
    }
    throw std::runtime_error("Unsupported shader stage name: " + stageName);
}

bool IsPathInsideRoot(
    const std::filesystem::path& path,
    const std::filesystem::path& root)
{
    std::error_code relativeError;
    const std::filesystem::path relative =
        std::filesystem::relative(path, root, relativeError);
    if (relativeError || relative.empty() || relative.is_absolute())
    {
        return path == root;
    }
    return *relative.begin() != "..";
}

std::filesystem::path NormalizeAbsolutePath(
    const std::filesystem::path& path)
{
    std::error_code canonicalError;
    const std::filesystem::path canonicalPath =
        std::filesystem::weakly_canonical(path, canonicalError);
    if (!canonicalError)
    {
        return canonicalPath;
    }
    return std::filesystem::absolute(path).lexically_normal();
}

#if defined(_WIN32)
using PhysicalPathIdentity = std::wstring;
#else
using PhysicalPathIdentity = std::string;
#endif

PhysicalPathIdentity BuildPhysicalPathIdentity(
    const std::filesystem::path& path)
{
#if defined(_WIN32)
    std::wstring identity =
        NormalizeAbsolutePath(path).native();
    std::transform(
        identity.begin(),
        identity.end(),
        identity.begin(),
        [](wchar_t character)
        {
            return std::towlower(character);
        });
    return identity;
#else
    return NormalizeAbsolutePath(path).generic_string();
#endif
}

struct CollectedDependency
{
    std::string path;
    std::string digest;
    std::set<std::string> requestingSources;
    uint32_t minimumIncludeDepth = std::numeric_limits<uint32_t>::max();
};

class DependencyCollector
{
public:
    explicit DependencyCollector(std::filesystem::path glslRoot)
        : glslRoot(NormalizeAbsolutePath(std::move(glslRoot)))
    {
    }

    std::string NormalizeIdentity(const std::filesystem::path& path) const
    {
        const std::filesystem::path normalized = NormalizeAbsolutePath(path);
        if (IsPathInsideRoot(normalized, glslRoot))
        {
            return std::filesystem::relative(normalized, glslRoot)
                .generic_string();
        }
        return normalized.generic_string();
    }

    void Record(
        const std::filesystem::path& sourcePath,
        const std::string& content,
        const std::filesystem::path& requestingSource,
        size_t includeDepth)
    {
        const std::string normalizedPath = NormalizeIdentity(sourcePath);
        const std::string digest =
            VL::ContentHasher::HashString(content).ToHex();
        CollectedDependency& dependency = dependencies[normalizedPath];
        if (!dependency.digest.empty() && dependency.digest != digest)
        {
            throw std::runtime_error(
                "Shader include changed while an artifact was compiling: " +
                normalizedPath);
        }

        dependency.path = normalizedPath;
        dependency.digest = digest;
        dependency.requestingSources.insert(
            NormalizeIdentity(requestingSource));
        dependency.minimumIncludeDepth = std::min(
            dependency.minimumIncludeDepth,
            static_cast<uint32_t>(includeDepth));
    }

    std::vector<VL::ShaderDependencyRecord> BuildSnapshot() const
    {
        std::vector<VL::ShaderDependencyRecord> snapshot;
        snapshot.reserve(dependencies.size());
        for (const auto& [path, collected] : dependencies)
        {
            VL::ShaderDependencyRecord record;
            record.path = path;
            record.digest = collected.digest;
            record.requestingSources.assign(
                collected.requestingSources.begin(),
                collected.requestingSources.end());
            record.minimumIncludeDepth =
                collected.minimumIncludeDepth ==
                    std::numeric_limits<uint32_t>::max()
                ? 0
                : collected.minimumIncludeDepth;
            snapshot.push_back(std::move(record));
        }
        return snapshot;
    }

private:
    std::filesystem::path glslRoot;
    std::map<std::string, CollectedDependency> dependencies;
};

struct IncludeResultStorage
{
    std::string sourceName;
    std::string content;
};

class TrackingIncluder : public shaderc::CompileOptions::IncluderInterface
{
public:
    TrackingIncluder(
        std::filesystem::path glslRoot,
        DependencyCollector& collector,
        const std::map<std::string, std::string>* sourceSnapshot)
        : glslRoot(NormalizeAbsolutePath(std::move(glslRoot))),
          collector(collector),
          sourceSnapshot(sourceSnapshot)
    {
    }

    shaderc_include_result* GetInclude(
        const char* requestedSource,
        shaderc_include_type type,
        const char* requestingSource,
        size_t includeDepth) override
    {
        auto* result = new shaderc_include_result{};
        auto* storage = new IncludeResultStorage();
        result->user_data = storage;

        try
        {
            const std::filesystem::path requestingPath =
                ResolveRequestingPath(requestingSource);
            std::filesystem::path includePath;
            if (type == shaderc_include_type_standard)
            {
                includePath = glslRoot / requestedSource;
            }
            else
            {
                includePath =
                    requestingPath.parent_path() / requestedSource;
            }
            includePath = NormalizeAbsolutePath(includePath);
            if (!IsPathInsideRoot(includePath, glslRoot))
            {
                throw std::runtime_error(
                    "Shader include escapes shader/glsl: " +
                    includePath.generic_string());
            }

            storage->sourceName = includePath.generic_string();
            const std::string normalizedIdentity =
                collector.NormalizeIdentity(includePath);
            bool foundFrozenSource = false;
            if (sourceSnapshot != nullptr)
            {
                const auto frozenIt =
                    sourceSnapshot->find(normalizedIdentity);
                if (frozenIt != sourceSnapshot->end())
                {
                    storage->content = frozenIt->second;
                    foundFrozenSource = true;
                }
            }
            if (!foundFrozenSource)
            {
                storage->content = ReadTextFile(includePath);
            }
            collector.Record(
                includePath,
                storage->content,
                requestingPath,
                includeDepth);
        }
        catch (const std::exception& exception)
        {
            storage->sourceName =
                requestedSource == nullptr ? "" : requestedSource;
            storage->content = exception.what();
        }

        result->source_name = storage->sourceName.data();
        result->source_name_length = storage->sourceName.size();
        result->content = storage->content.data();
        result->content_length = storage->content.size();
        return result;
    }

    void ReleaseInclude(shaderc_include_result* result) override
    {
        if (result == nullptr)
        {
            return;
        }
        delete static_cast<IncludeResultStorage*>(result->user_data);
        delete result;
    }

private:
    std::filesystem::path ResolveRequestingPath(
        const char* requestingSource) const
    {
        std::filesystem::path path =
            requestingSource == nullptr
            ? std::filesystem::path()
            : std::filesystem::path(requestingSource);
        if (!path.is_absolute())
        {
            path = glslRoot / path;
        }
        return NormalizeAbsolutePath(path);
    }

    std::filesystem::path glslRoot;
    DependencyCollector& collector;
    const std::map<std::string, std::string>* sourceSnapshot;
};

std::vector<uint32_t> CompileGlsl(
    const VL::ShaderStageBuildSource& stage,
    const std::vector<std::string>& macros,
    bool generateDebugInfo,
    const std::filesystem::path& glslRoot,
    DependencyCollector& dependencyCollector,
    const std::map<std::string, std::string>* sourceSnapshot)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    if (generateDebugInfo)
    {
        options.SetGenerateDebugInfo();
    }
    options.SetTargetEnvironment(
        shaderc_target_env_vulkan,
        shaderc_env_version_vulkan_1_4);
    options.SetSourceLanguage(shaderc_source_language_glsl);
    options.SetIncluder(std::make_unique<TrackingIncluder>(
        glslRoot,
        dependencyCollector,
        sourceSnapshot));

    for (const std::string& macro : macros)
    {
        const size_t valueSeparator = macro.find('=');
        if (valueSeparator == std::string::npos)
        {
            options.AddMacroDefinition(macro, "1");
        }
        else
        {
            options.AddMacroDefinition(
                macro.substr(0, valueSeparator),
                macro.substr(valueSeparator + 1));
        }
    }

    const std::string sourcePath = stage.virtualSourcePath.generic_string();
    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        stage.sourceCode.data(),
        stage.sourceCode.size(),
        stage.shaderKind,
        sourcePath.c_str(),
        "main",
        options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        throw std::runtime_error(
            "Shader compile failed for '" + stage.sourceIdentity +
            "':\n" + result.GetErrorMessage());
    }
    // 失败仍由上层异常诊断统一报告；这里只旁路成功编译的 warning，避免重复记录同一错误。
    if (result.GetNumWarnings() > 0)
        VL::ProcessLog().Append(VL::LogSeverity::Warning, "Shader Compiler",
            "Shader compile warnings for '" + stage.sourceIdentity + "':\n" + result.GetErrorMessage());
    return {result.cbegin(), result.cend()};
}

std::vector<VL::ShaderBuildSourceRecord> BuildPrimarySourceRecords(
    const VL::ShaderBuildRequest& request)
{
    std::vector<VL::ShaderBuildSourceRecord> records;
    records.reserve(request.stages.size());
    for (const VL::ShaderStageBuildSource& stage : request.stages)
    {
        records.push_back({
            stage.sourceIdentity,
            VL::ContentHasher::HashString(stage.sourceCode).ToHex()});
    }
    std::sort(
        records.begin(),
        records.end(),
        [](const VL::ShaderBuildSourceRecord& lhs,
           const VL::ShaderBuildSourceRecord& rhs)
        {
            return lhs.identity < rhs.identity;
        });
    return records;
}

std::string BuildSourceFingerprint(
    const VL::ShaderBuildRequest& request,
    const std::vector<VL::ShaderBuildSourceRecord>& primarySources,
    const std::vector<VL::ShaderDependencyRecord>& dependencies,
    const std::string& compilePolicy)
{
    std::unordered_map<std::string, std::string> primaryDigests;
    for (const VL::ShaderBuildSourceRecord& source : primarySources)
    {
        primaryDigests[source.identity] = source.digest;
    }

    VL::CanonicalFieldHasher hasher("ShaderSourceFingerprintV1");
    hasher.AddUInt32(
        "cacheSchemaVersion",
        VL::ShaderBuildCacheSchemaVersion);
    hasher.AddString("compilePolicy", compilePolicy);
    hasher.AddString(
        "targetEnvironment",
        VL::ShaderBuildTargetEnvironment);
    hasher.AddString("optimizationMode", "performance");
#if defined(NDEBUG)
    hasher.AddString(
        "debugReflectionPolicy",
        "runtime-optimized;debug-debug-info+ENABLE_DEBUG_VIEW");
#else
    hasher.AddString(
        "debugReflectionPolicy",
        "runtime-and-debug-debug-info+ENABLE_DEBUG_VIEW");
#endif
    hasher.AddUInt32(
        "kind",
        static_cast<uint32_t>(request.kind));
    hasher.AddString("normalizedKey", request.normalizedKey);
    hasher.AddUInt32(
        "macroCount",
        static_cast<uint32_t>(request.macros.size()));
    for (size_t index = 0; index < request.macros.size(); ++index)
    {
        hasher.AddString(
            "macro." + std::to_string(index),
            request.macros[index]);
    }

    hasher.AddUInt32(
        "stageCount",
        static_cast<uint32_t>(request.stages.size()));
    for (size_t index = 0; index < request.stages.size(); ++index)
    {
        const VL::ShaderStageBuildSource& stage = request.stages[index];
        const std::string prefix = "stage." + std::to_string(index);
        hasher.AddString(prefix + ".name", stage.stageName);
        hasher.AddString(prefix + ".sourceIdentity", stage.sourceIdentity);
        hasher.AddDigest(
            prefix + ".sourceDigest",
            VL::ContentDigest::FromHex(
                primaryDigests.at(stage.sourceIdentity)));
    }

    hasher.AddUInt32(
        "dependencyCount",
        static_cast<uint32_t>(dependencies.size()));
    for (size_t index = 0; index < dependencies.size(); ++index)
    {
        const std::string prefix =
            "dependency." + std::to_string(index);
        hasher.AddString(prefix + ".path", dependencies[index].path);
        hasher.AddDigest(
            prefix + ".digest",
            VL::ContentDigest::FromHex(dependencies[index].digest));
    }
    return hasher.Finalize().ToHex();
}

std::string BuildArtifactGenerationKey(
    const VL::ShaderBuildArtifact& artifact)
{
    VL::CanonicalFieldHasher hasher("ShaderArtifactGenerationV1");
    hasher.AddString("logicalBuildId", artifact.logicalBuildId);
    hasher.AddDigest(
        "sourceFingerprint",
        VL::ContentDigest::FromHex(artifact.sourceFingerprint));
    hasher.AddUInt32(
        "outputCount",
        static_cast<uint32_t>(artifact.outputs.size()));
    for (const auto& [role, output] : artifact.outputs)
    {
        hasher.AddString("output.role", role);
        hasher.AddDigest(
            "output.digest",
            VL::ContentDigest::FromHex(output.digest));
    }
    hasher.AddDigest(
        "abiFingerprint",
        VL::ContentDigest::FromHex(artifact.abiFingerprint));
    return hasher.Finalize().ToHex();
}

VL::ShaderBuildArtifactRecord BuildManifestRecord(
    const VL::ShaderBuildArtifact& artifact,
    const ShaderCompiler& compiler)
{
    VL::ShaderBuildArtifactRecord record;
    record.logicalBuildId = artifact.logicalBuildId;
    record.kind = VL::ShaderBuildKindToString(artifact.kind);
    record.normalizedKey = artifact.normalizedKey;
    record.sourceFingerprint = artifact.sourceFingerprint;
    record.primarySources = artifact.primarySources;
    record.dependencies = artifact.dependencies;
    record.abiFingerprint = artifact.abiFingerprint;
    for (const auto& [role, output] : artifact.outputs)
    {
        std::error_code relativeError;
        const std::filesystem::path relative =
            std::filesystem::relative(
                output.path,
                compiler.GetShaderRoot(),
                relativeError);
        if (relativeError || relative.is_absolute())
        {
            throw std::runtime_error(
                "Shader output must live under the shader root: " +
                output.path.string());
        }
        record.outputs.emplace(
            role,
            VL::ShaderBuildOutputRecord{
                relative.generic_string(),
                output.digest});
    }
    return record;
}

bool EqualPrimarySources(
    const std::vector<VL::ShaderBuildSourceRecord>& lhs,
    const std::vector<VL::ShaderBuildSourceRecord>& rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (size_t index = 0; index < lhs.size(); ++index)
    {
        if (lhs[index].identity != rhs[index].identity ||
            lhs[index].digest != rhs[index].digest)
        {
            return false;
        }
    }
    return true;
}

std::vector<std::string> BuildDebugRoles(
    const VL::ShaderBuildRequest& request)
{
    std::vector<std::string> roles;
    roles.reserve(request.stages.size());
    for (const VL::ShaderStageBuildSource& stage : request.stages)
    {
        roles.emplace_back(DebugRoleForStage(stage.stageName));
    }
    return roles;
}

} // namespace ShaderCompilerDetail
} // namespace VL

using namespace VL::ShaderCompilerDetail;

std::string ShaderCompiler::GetCompilePolicy()
{
#if defined(NDEBUG)
    return
        "ShaderCompilePolicyV1;"
        "configuration=Release;"
        "optimization=performance;"
        "runtime=optimized;"
        "reflection=debug-info+ENABLE_DEBUG_VIEW;"
        "abi=ShaderAbiSignatureV2";
#else
    return
        "ShaderCompilePolicyV1;"
        "configuration=Debug;"
        "optimization=performance;"
        "runtime=debug-info+ENABLE_DEBUG_VIEW;"
        "reflection=runtime;"
        "abi=ShaderAbiSignatureV2";
#endif
}

std::string ShaderCompiler::FormatStatistics(
    const VL::ShaderBuildStatistics& statistics)
{
    std::ostringstream stream;
    stream << "Shader build: entries=" << statistics.entries
           << ", artifacts=" << statistics.artifacts
           << ", hits=" << statistics.cacheHits
           << ", misses=" << statistics.cacheMisses
           << ", compiled=" << statistics.compiledArtifacts
           << ", shaderc=" << statistics.shadercInvocations
           << ", failed=" << statistics.failedArtifacts
           << ", elapsedMs=" << statistics.elapsedMilliseconds;
    return stream.str();
}

VL::ShaderBuildArtifact ShaderCompiler::EnsureBuild(
    const VL::ShaderBuildRequest& request,
    bool forceRebuild)
{
    RequireInitialized();
    VL::ShaderBuildArtifact artifact;
    std::string missReason;
    if (!forceRebuild &&
        TryLoadCachedArtifact(request, artifact, missReason))
    {
        artifact.cacheHit = true;
        artifact.committed = true;
        artifact.cacheReason = "cache hit";
        const auto cacheMessage = "Shader cache hit: " + request.normalizedKey;
        std::cout << cacheMessage << std::endl;
        VL::ProcessLog().Append(VL::LogSeverity::Debug, "Shader Compiler", cacheMessage);
        return artifact;
    }

    if (forceRebuild)
    {
        missReason = "force rebuild";
    }
    const auto cacheMessage = "Shader cache miss: " + request.normalizedKey + " (" + missReason + ")";
    std::cout << cacheMessage << std::endl;
    VL::ProcessLog().Append(VL::LogSeverity::Info, "Shader Compiler", cacheMessage);

    artifact = CompileRequest(request);
    artifact.cacheReason = missReason;
    std::vector<VL::ShaderBuildArtifact> artifacts;
    artifacts.push_back(std::move(artifact));
    CommitArtifacts(artifacts);
    return std::move(artifacts.front());
}

VL::ShaderBuildArtifact ShaderCompiler::CompileRequest(
    const VL::ShaderBuildRequest& request) const
{
    RequireInitialized();
    if (request.stages.empty())
    {
        throw std::runtime_error("Shader build request has no stages");
    }

    VL::ShaderBuildArtifact artifact;
    artifact.kind = request.kind;
    artifact.logicalBuildId = request.logicalBuildId;
    artifact.normalizedKey = request.normalizedKey;
    artifact.primarySources = BuildPrimarySourceRecords(request);

    DependencyCollector dependencyCollector(glslRoot);
    std::vector<std::vector<uint32_t>> debugSpirv;
    debugSpirv.reserve(request.stages.size());

    std::vector<std::string> runtimeMacros = request.macros;
    std::vector<std::string> debugMacros = request.macros;
    debugMacros.push_back("ENABLE_DEBUG_VIEW");
    debugMacros = NormalizeMaterialMacros(std::move(debugMacros));

#if defined(NDEBUG)
    constexpr bool RuntimeHasDebugInfo = false;
    constexpr bool RuntimeIsDebugArtifact = false;
#else
    constexpr bool RuntimeHasDebugInfo = true;
    constexpr bool RuntimeIsDebugArtifact = true;
    runtimeMacros = debugMacros;
#endif

    for (const VL::ShaderStageBuildSource& stage : request.stages)
    {
        if (stage.stageName != StageNameForKind(stage.shaderKind))
        {
            throw std::runtime_error(
                "Shader build stage name does not match shaderc kind: " +
                stage.sourceIdentity);
        }

        const char* runtimeRole = RuntimeRoleForStage(stage.stageName);
        const char* debugRole = DebugRoleForStage(stage.stageName);
        const auto runtimePathIt = request.outputPaths.find(runtimeRole);
        const auto debugPathIt = request.outputPaths.find(debugRole);
        if (runtimePathIt == request.outputPaths.end() ||
            debugPathIt == request.outputPaths.end())
        {
            throw std::runtime_error(
                "Shader build request is missing output paths for stage: " +
                stage.stageName);
        }

        std::vector<uint32_t> runtimeCode = CompileGlsl(
            stage,
            runtimeMacros,
            RuntimeHasDebugInfo,
            glslRoot,
            dependencyCollector,
            &request.sourceSnapshot);
        ++artifact.shadercInvocations;

        std::vector<uint32_t> debugCode;
        if (RuntimeIsDebugArtifact)
        {
            debugCode = runtimeCode;
        }
        else
        {
            debugCode = CompileGlsl(
                stage,
                debugMacros,
                true,
                glslRoot,
                dependencyCollector,
                &request.sourceSnapshot);
            ++artifact.shadercInvocations;
        }

        artifact.outputs.emplace(
            runtimeRole,
            VL::ShaderBuildOutput{
                NormalizeAbsolutePath(runtimePathIt->second),
                std::move(runtimeCode),
                {}});
        artifact.outputs.emplace(
            debugRole,
            VL::ShaderBuildOutput{
                NormalizeAbsolutePath(debugPathIt->second),
                debugCode,
                {}});
        debugSpirv.push_back(std::move(debugCode));
    }

    artifact.dependencies = dependencyCollector.BuildSnapshot();
    artifact.sourceFingerprint = BuildSourceFingerprint(
        request,
        artifact.primarySources,
        artifact.dependencies,
        compilePolicy);

    for (auto& [role, output] : artifact.outputs)
    {
        output.digest = VL::ContentHasher::HashBytes(
            output.spirv.data(),
            output.spirv.size() * sizeof(uint32_t)).ToHex();
    }

    const ShaderReflectionResult reflection =
        ShaderReflectionService::ReflectDetailedFromDebugSpirvCode(
            debugSpirv);
    artifact.shaderBindings = reflection.shaderBindings;
    artifact.abiSignature = reflection.abiSignature;
    artifact.abiFingerprint =
        artifact.abiSignature.GetFingerprint();
    artifact.artifactGenerationKey =
        BuildArtifactGenerationKey(artifact);
    return artifact;
}

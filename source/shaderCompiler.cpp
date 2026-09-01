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

#include "material/compiler/materialShaderCompileRequest.h"
#include "material/compiler/materialShaderComposer.h"
#include "pipeline/shaderReflectionService.h"
#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"

namespace
{

constexpr const char* RuntimeVertexRole = "runtimeVertex";
constexpr const char* RuntimeFragmentRole = "runtimeFragment";
constexpr const char* RuntimeComputeRole = "runtimeCompute";
constexpr const char* DebugVertexRole = "debugVertex";
constexpr const char* DebugFragmentRole = "debugFragment";
constexpr const char* DebugComputeRole = "debugCompute";

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
    case RenderMode::ForwardEyeInner:
        return {"RENDER_MODE_FORWARD_EYE_INNER"};
    case RenderMode::ForwardEyeCornea:
        return {"RENDER_MODE_FORWARD_EYE_CORNEA"};
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
    // GLSL 的 shading model 常量是 const uint，不能用于预处理器 #if；
    // 额外注入数值宏供材质专用资源声明选择，避免 Eye binding 污染其他 Forward pass。
    macros.push_back(
        shaderVariantKey.shadingModelMacro == "SHADING_MODEL_EYE"
            ? "MATERIAL_IS_EYE=1"
            : "MATERIAL_IS_EYE=0");
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

} // namespace

void ShaderCompiler::Initialize(const std::filesystem::path& initializeShaderRoot)
{
    shaderRoot = NormalizeAbsolutePath(initializeShaderRoot);
    glslRoot = shaderRoot / "glsl";
    spirvRoot = shaderRoot / "spv";
    if (!std::filesystem::is_directory(glslRoot))
    {
        throw std::runtime_error(
            "Shader GLSL root does not exist: " + glslRoot.string());
    }

    compilePolicy = GetCompilePolicy();
    manifestStore.Initialize(shaderRoot, compilePolicy);
    initialized = true;
}

VL::ShaderBuildStatistics ShaderCompiler::StartCompile(
    const std::filesystem::path& startShaderRoot,
    bool forceRebuild)
{
    const std::filesystem::path normalizedRoot =
        NormalizeAbsolutePath(startShaderRoot);
    if (!initialized || normalizedRoot != shaderRoot)
    {
        Initialize(normalizedRoot);
    }

    lastStatistics = {};
    const auto startTime = std::chrono::steady_clock::now();

    struct GraphicsSources
    {
        std::filesystem::path vertex;
        std::filesystem::path fragment;
    };
    std::map<std::string, GraphicsSources> graphicsSources;
    std::vector<std::filesystem::path> computeSources;

    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(glslRoot))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        const std::string extension = entry.path().extension().string();
        if (extension == ".comp")
        {
            computeSources.push_back(entry.path());
            continue;
        }
        if (extension != ".vert" && extension != ".frag")
        {
            continue;
        }

        std::filesystem::path relative =
            std::filesystem::relative(entry.path(), glslRoot);
        relative.replace_extension();
        GraphicsSources& sources =
            graphicsSources[relative.generic_string()];
        if (extension == ".vert")
        {
            sources.vertex = entry.path();
        }
        else
        {
            sources.fragment = entry.path();
        }
    }
    std::sort(computeSources.begin(), computeSources.end());

    try
    {
        for (const auto& [shaderName, sources] : graphicsSources)
        {
            if (sources.vertex.empty() || sources.fragment.empty())
            {
                throw std::runtime_error(
                    "Graphics shader entry must provide a complete .vert/.frag pair: " +
                    shaderName);
            }

            ShaderVariantKey variant;
            variant.shaderName = shaderName;
            const VL::ShaderBuildArtifact artifact =
                EnsureBuild(
                    CreateGraphicsVariantBuildRequest(variant),
                    forceRebuild);
            lastStatistics.entries += 2;
            ++lastStatistics.artifacts;
            if (artifact.cacheHit)
            {
                ++lastStatistics.cacheHits;
            }
            else
            {
                ++lastStatistics.cacheMisses;
                ++lastStatistics.compiledArtifacts;
                lastStatistics.shadercInvocations +=
                    artifact.shadercInvocations;
            }
        }

        for (const std::filesystem::path& computeSource : computeSources)
        {
            const VL::ShaderBuildArtifact artifact =
                EnsureBuild(
                    CreateComputeStageBuildRequest(computeSource),
                    forceRebuild);
            ++lastStatistics.entries;
            ++lastStatistics.artifacts;
            if (artifact.cacheHit)
            {
                ++lastStatistics.cacheHits;
            }
            else
            {
                ++lastStatistics.cacheMisses;
                ++lastStatistics.compiledArtifacts;
                lastStatistics.shadercInvocations +=
                    artifact.shadercInvocations;
            }
        }
    }
    catch (...)
    {
        ++lastStatistics.failedArtifacts;
        const auto endTime = std::chrono::steady_clock::now();
        lastStatistics.elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(
                endTime - startTime).count();
        std::cerr << FormatStatistics(lastStatistics) << std::endl;
        throw;
    }

    const auto endTime = std::chrono::steady_clock::now();
    lastStatistics.elapsedMilliseconds =
        std::chrono::duration<double, std::milli>(
            endTime - startTime).count();
    std::cout << FormatStatistics(lastStatistics) << std::endl;
    return lastStatistics;
}

VL::ShaderBuildArtifact ShaderCompiler::EnsureGraphicsVariantCompiled(
    const ShaderVariantKey& shaderVariantKey,
    bool forceRebuild)
{
    return EnsureBuild(
        CreateGraphicsVariantBuildRequest(shaderVariantKey),
        forceRebuild);
}

VL::ShaderBuildArtifact
ShaderCompiler::EnsureMaterialGraphicsVariantCompiled(
    const VL::MaterialShaderCompileRequest& request,
    const VL::ComposedMaterialShaderSource& source,
    bool forceRebuild)
{
    return EnsureBuild(
        CreateMaterialGraphicsBuildRequest(request, source),
        forceRebuild);
}

VL::ShaderBuildArtifact ShaderCompiler::EnsureComputeStageCompiled(
    const std::string& shaderName,
    bool forceRebuild)
{
    return EnsureBuild(
        CreateComputeStageBuildRequestForShaderName(shaderName),
        forceRebuild);
}

VL::ShaderBuildRequest ShaderCompiler::CreateGraphicsVariantBuildRequest(
    const ShaderVariantKey& shaderVariantKeyInput) const
{
    RequireInitialized();
    ShaderVariantKey shaderVariantKey = shaderVariantKeyInput;
    shaderVariantKey.macros =
        NormalizeMaterialMacros(std::move(shaderVariantKey.macros));

    VL::ShaderBuildRequest request;
    request.kind = VL::ShaderBuildKind::GraphicsPair;
    request.normalizedKey =
        "kind=GraphicsPair|" + shaderVariantKey.GetNormalizedKey() +
        "|target=" + VL::ShaderBuildTargetEnvironment;
    request.logicalBuildId =
        BuildLogicalBuildId(request.kind, request.normalizedKey);
    request.macros = BuildGraphicsVariantMacros(shaderVariantKey);

    const std::filesystem::path vertexPath =
        glslRoot / (shaderVariantKey.shaderName + ".vert");
    const std::filesystem::path fragmentPath =
        glslRoot / (shaderVariantKey.shaderName + ".frag");
    request.stages = {
        {
            "vertex",
            NormalizeSourceIdentity(vertexPath),
            NormalizeAbsolutePath(vertexPath),
            ReadTextFile(vertexPath),
            shaderc_vertex_shader},
        {
            "fragment",
            NormalizeSourceIdentity(fragmentPath),
            NormalizeAbsolutePath(fragmentPath),
            ReadTextFile(fragmentPath),
            shaderc_fragment_shader}};

    request.outputPaths = {
        {
            RuntimeVertexRole,
            spirvRoot /
                shaderVariantKey.GetStageSpvRelativePath("vert")},
        {
            RuntimeFragmentRole,
            spirvRoot /
                shaderVariantKey.GetStageSpvRelativePath("frag")},
        {
            DebugVertexRole,
            spirvRoot /
                shaderVariantKey.GetStageDebugRelativePath("vert")},
        {
            DebugFragmentRole,
            spirvRoot /
                shaderVariantKey.GetStageDebugRelativePath("frag")}};
    return request;
}

VL::ShaderBuildRequest
ShaderCompiler::CreateComputeStageBuildRequestForShaderName(
    const std::string& shaderName) const
{
    RequireInitialized();
    const std::filesystem::path sourcePath =
        glslRoot / (shaderName + ".comp");
    if (!std::filesystem::is_regular_file(sourcePath))
    {
        throw std::runtime_error(
            "Compute shader entry does not exist: " +
            sourcePath.generic_string());
    }
    return CreateComputeStageBuildRequest(sourcePath);
}

VL::ShaderBuildRequest
ShaderCompiler::CreateMaterialGraphicsBuildRequest(
    const VL::MaterialShaderCompileRequest& requestInput,
    const VL::ComposedMaterialShaderSource& source) const
{
    RequireInitialized();
    VL::MaterialShaderCompileRequest materialRequest = requestInput;
    materialRequest.shaderVariantKey.macros = NormalizeMaterialMacros(
        std::move(materialRequest.shaderVariantKey.macros));

    VL::ShaderBuildRequest request;
    request.kind = VL::ShaderBuildKind::MaterialGraphicsPair;
    request.normalizedKey =
        "kind=MaterialGraphicsPair|" +
        materialRequest.GetNormalizedKey();
    request.logicalBuildId =
        BuildLogicalBuildId(request.kind, request.normalizedKey);
    request.macros =
        BuildGraphicsVariantMacros(materialRequest.shaderVariantKey);
    request.stages = {
        {
            "vertex",
            NormalizeSourceIdentity(source.vertexVirtualPath),
            NormalizeAbsolutePath(source.vertexVirtualPath),
            source.vertexSource,
            shaderc_vertex_shader},
        {
            "fragment",
            NormalizeSourceIdentity(source.fragmentVirtualPath),
            NormalizeAbsolutePath(source.fragmentVirtualPath),
            source.fragmentSource,
            shaderc_fragment_shader}};
    const std::filesystem::path materialSourcePath =
        glslRoot / materialRequest.source.materialSourcePath;
    if (!std::filesystem::is_regular_file(materialSourcePath))
    {
        throw std::runtime_error(
            "Material definition source does not exist: " +
            materialSourcePath.generic_string());
    }
    request.validationSourceDigests.emplace(
        materialRequest.source.materialSourcePath,
        VL::ContentHasher::HashFile(materialSourcePath).ToHex());
    if (materialRequest.source.parameterIncludeBytes.has_value())
    {
        request.sourceSnapshot.emplace(
            materialRequest.source.parameterIncludePath,
            *materialRequest.source.parameterIncludeBytes);
        request.candidateOverlayDigests.emplace(
            materialRequest.source.parameterIncludePath,
            VL::ContentHasher::HashString(
                *materialRequest.source.parameterIncludeBytes).ToHex());
    }

    const std::filesystem::path outputRoot =
        spirvRoot / "material" / materialRequest.GetRequestHash();
    request.outputPaths = {
        {RuntimeVertexRole, outputRoot.string() + ".vert.spv"},
        {RuntimeFragmentRole, outputRoot.string() + ".frag.spv"},
        {DebugVertexRole, outputRoot.string() + ".vert.debug"},
        {DebugFragmentRole, outputRoot.string() + ".frag.debug"}};
    return request;
}

VL::ShaderBuildArtifact ShaderCompiler::CompileCandidate(
    const VL::ShaderBuildRequest& request) const
{
    RequireInitialized();
    return CompileRequest(request);
}

VL::ShaderBuildArtifact ShaderCompiler::PrepareCandidate(
    const VL::ShaderBuildRequest& request) const
{
    RequireInitialized();
    VL::ShaderBuildArtifact artifact;
    std::string missReason;
    if (TryLoadCachedArtifact(request, artifact, missReason))
    {
        artifact.cacheHit = true;
        artifact.committed = true;
        artifact.cacheReason = "cache hit";
        return artifact;
    }

    artifact = CompileRequest(request);
    artifact.cacheReason = std::move(missReason);
    return artifact;
}

ShaderCompiler::CandidateSourceValidationResult
ShaderCompiler::ValidateCandidateSourcesStillCurrent(
    const VL::ShaderBuildRequest& request,
    const VL::ShaderBuildArtifact& artifact) const
{
    RequireInitialized();

    auto mismatch = [](
        const std::string& reason,
        const std::string& identity,
        const std::string& capturedDigest,
        const std::string& currentDigest)
    {
        CandidateSourceValidationResult result;
        result.current = false;
        result.reason = reason;
        result.sourceIdentity = identity;
        result.capturedDigest = capturedDigest;
        result.currentDigest = currentDigest;
        return result;
    };

    for (const VL::ShaderBuildSourceRecord& source :
         artifact.primarySources)
    {
        if (source.identity.rfind("__composed__/", 0) == 0)
        {
            bool capturedPrimaryMatches = false;
            for (const VL::ShaderStageBuildSource& stage :
                 request.stages)
            {
                if (stage.sourceIdentity == source.identity &&
                    VL::ContentHasher::HashString(
                        stage.sourceCode).ToHex() == source.digest)
                {
                    capturedPrimaryMatches = true;
                    break;
                }
            }
            if (!capturedPrimaryMatches)
            {
                return mismatch(
                    "captured composed primary source changed",
                    source.identity,
                    source.digest,
                    "request source mismatch");
            }
            continue;
        }

        const std::filesystem::path sourcePath =
            glslRoot / source.identity;
        if (!std::filesystem::is_regular_file(sourcePath))
        {
            return mismatch(
                "primary source missing",
                source.identity,
                source.digest,
                "<missing>");
        }

        const std::string currentDigest =
            VL::ContentHasher::HashFile(sourcePath).ToHex();
        if (currentDigest != source.digest)
        {
            return mismatch(
                "primary source digest changed",
                source.identity,
                source.digest,
                currentDigest);
        }
    }

    for (const VL::ShaderDependencyRecord& dependency :
         artifact.dependencies)
    {
        const auto overlayIt =
            request.candidateOverlayDigests.find(dependency.path);
        if (overlayIt != request.candidateOverlayDigests.end())
        {
            if (dependency.digest != overlayIt->second)
            {
                return mismatch(
                    "candidate overlay digest changed",
                    dependency.path,
                    dependency.digest,
                    overlayIt->second);
            }
            continue;
        }

        const std::filesystem::path dependencyPath =
            glslRoot / dependency.path;
        if (!std::filesystem::is_regular_file(dependencyPath))
        {
            return mismatch(
                "dependency missing",
                dependency.path,
                dependency.digest,
                "<missing>");
        }

        const std::string currentDigest =
            VL::ContentHasher::HashFile(dependencyPath).ToHex();
        if (currentDigest != dependency.digest)
        {
            return mismatch(
                "dependency digest changed",
                dependency.path,
                dependency.digest,
                currentDigest);
        }
    }

    for (const auto& [identity, capturedDigest] :
         request.validationSourceDigests)
    {
        const std::filesystem::path validationPath =
            glslRoot / identity;
        if (!std::filesystem::is_regular_file(validationPath))
        {
            return mismatch(
                "validation source missing",
                identity,
                capturedDigest,
                "<missing>");
        }

        const std::string currentDigest =
            VL::ContentHasher::HashFile(validationPath).ToHex();
        if (currentDigest != capturedDigest)
        {
            return mismatch(
                "validation source digest changed",
                identity,
                capturedDigest,
                currentDigest);
        }
    }

    return CandidateSourceValidationResult{};
}

void ShaderCompiler::FreezeSourceSnapshot(
    VL::ShaderBuildRequest& request) const
{
    RequireInitialized();
    std::map<std::string, std::string> preparedOverlay =
        std::move(request.sourceSnapshot);
    request.sourceSnapshot.clear();

    std::error_code iterationError;
    std::filesystem::recursive_directory_iterator iterator(
        glslRoot,
        std::filesystem::directory_options::skip_permission_denied,
        iterationError);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end)
    {
        if (iterationError)
        {
            break;
        }

        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(iterationError) && !iterationError)
        {
            const std::filesystem::path relative =
                std::filesystem::relative(
                    entry.path(),
                    glslRoot,
                    iterationError);
            if (!iterationError)
            {
                request.sourceSnapshot.emplace(
                    relative.generic_string(),
                    ReadTextFile(entry.path()));
            }
        }
        iterationError.clear();
        iterator.increment(iterationError);
    }

    for (auto& [identity, bytes] : preparedOverlay)
    {
        request.sourceSnapshot.insert_or_assign(
            std::move(identity),
            std::move(bytes));
    }
}

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
        std::cout << "Shader cache hit: " << request.normalizedKey
                  << std::endl;
        return artifact;
    }

    if (forceRebuild)
    {
        missReason = "force rebuild";
    }
    std::cout << "Shader cache miss: " << request.normalizedKey
              << " (" << missReason << ")" << std::endl;

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

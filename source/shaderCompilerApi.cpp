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

void ShaderCompiler::SetExcludedShaderNames(std::vector<std::string> names)
{
    excludedShaderNames = std::move(names);
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
        const std::string shaderName = relative.generic_string();
        if (std::find(
                excludedShaderNames.begin(),
                excludedShaderNames.end(),
                shaderName) != excludedShaderNames.end())
        {
            continue;
        }
        GraphicsSources& sources =
            graphicsSources[shaderName];
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
        const auto statisticsMessage = FormatStatistics(lastStatistics);
        std::cerr << statisticsMessage << std::endl;
        VL::ProcessLog().Append(VL::LogSeverity::Error, "Shader Compiler", statisticsMessage);
        throw;
    }

    const auto endTime = std::chrono::steady_clock::now();
    lastStatistics.elapsedMilliseconds =
        std::chrono::duration<double, std::milli>(
            endTime - startTime).count();
    const auto statisticsMessage = FormatStatistics(lastStatistics);
    std::cout << statisticsMessage << std::endl;
    VL::ProcessLog().Append(VL::LogSeverity::Info, "Shader Compiler", statisticsMessage);
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

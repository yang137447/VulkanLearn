#include "pipelineFactory.h"
#include <algorithm>
#include <sstream>
#include "computePipeline.h"
#include "graphicsPipeline.h"
#include "shaderCompiler.h"
#include "shaderReflectionService.h"
#include "material/compiler/materialShaderComposer.h"
#include "render/backend/rendererBackendVulkan.h"
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

static_assert(
    std::is_nothrow_move_constructible_v<
        PipelineFactory::PreparedGraphicsCandidateCommit>);

namespace
{

ShaderVariantKey NormalizeShaderVariantKey(const ShaderVariantKey& shaderVariantKey)
{
    ShaderVariantKey normalizedKey = shaderVariantKey;
    normalizedKey.macros = NormalizeMaterialMacros(std::move(normalizedKey.macros));
    return normalizedKey;
}

} // namespace

size_t GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const
{
    const size_t passPipelineContractHash =
        std::hash<std::string>{}(key.passPipelineContractKey);
    const size_t shaderVariantHash = std::hash<std::string>{}(key.shaderArtifactKey);
    const size_t cullModeHash = std::hash<uint32_t>{}(static_cast<uint32_t>(key.cullMode));
    const size_t blendModeHash = std::hash<uint32_t>{}(static_cast<uint32_t>(key.blendMode));
    const size_t pipelineLayoutHash = std::hash<std::string>{}(key.pipelineLayoutKey);

    size_t hash = passPipelineContractHash;
    hash ^= shaderVariantHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= cullModeHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= blendModeHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= pipelineLayoutHash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
}

PipelineFactory::PipelineFactory(
    VL::RendererBackendVulkan* rendererBackend,
    vk::Device& device)
{
    this->rendererBackend = rendererBackend;
    this->device = &device;
}

void PipelineFactory::SetShaderCompiler(ShaderCompiler* compiler)
{
    shaderCompiler = compiler;
}

ShaderCompiler& PipelineFactory::GetShaderCompiler() const
{
    if (shaderCompiler == nullptr)
    {
        throw std::runtime_error(
            "PipelineFactory has no ShaderCompiler service");
    }
    return *shaderCompiler;
}

std::string PipelineFactory::GetGraphicsShaderVariantCacheKey(
    const ShaderVariantKey& shaderVariantKey)
{
    return NormalizeShaderVariantKey(shaderVariantKey).GetNormalizedKey();
}

std::string PipelineFactory::GetMaterialShaderVariantCacheKey(
    const VL::MaterialShaderCompileRequest& requestInput)
{
    VL::MaterialShaderCompileRequest request = requestInput;
    request.shaderVariantKey.macros =
        NormalizeMaterialMacros(std::move(request.shaderVariantKey.macros));
    return request.GetNormalizedKey();
}

std::shared_ptr<ComputePipeline> PipelineFactory::CreateComputePipeline(
    const std::string& shaderName,
    ComputeShaderArtifact* activeArtifact)
{
    std::shared_ptr<ComputePipeline> pipeline;

    if (shaderCompiler != nullptr)
    {
        const VL::ShaderBuildArtifact buildArtifact =
            GetShaderCompiler().EnsureComputeStageCompiled(
                shaderName);
        ComputeShaderArtifact artifact =
            BuildComputeShaderArtifact(
                buildArtifact,
                shaderName);
        if (activeArtifact != nullptr)
        {
            *activeArtifact = artifact;
        }

        const auto cacheIt = computePipelines.find(shaderName);
        if (cacheIt != computePipelines.end())
        {
            pipeline = cacheIt->second.pipeline.lock();
            if (pipeline &&
                cacheIt->second.artifactGenerationKey ==
                    artifact.artifactGenerationKey)
            {
                // 进程内命中必须与磁盘上的当前源码代际一致，避免热重载后
                // 复用旧 Compute pipeline。
                return pipeline;
            }
        }

        pipeline = std::shared_ptr<ComputePipeline>(new ComputePipeline(
            rendererBackend,
            *device,
            artifact));
        computePipelines[shaderName] = {
            pipeline,
            artifact.artifactGenerationKey};
        return pipeline;
    }

    const auto cacheIt = computePipelines.find(shaderName);
    if (cacheIt != computePipelines.end())
    {
        pipeline = cacheIt->second.pipeline.lock();
        if (pipeline)
        {
            return pipeline;
        }
    }

    pipeline = std::shared_ptr<ComputePipeline>(new ComputePipeline(
        rendererBackend,
        *device,
        shaderName));
    computePipelines[shaderName] = {pipeline, {}};
    return pipeline;
}

std::shared_ptr<ComputePipeline>
PipelineFactory::CreateComputePipelineCandidate(
    const std::string& shaderName,
    ComputeShaderArtifact* candidateArtifact)
{
    if (shaderCompiler != nullptr)
    {
        const VL::ShaderBuildArtifact buildArtifact =
            GetShaderCompiler().EnsureComputeStageCompiled(
                shaderName);
        ComputeShaderArtifact artifact =
            BuildComputeShaderArtifact(
                buildArtifact,
                shaderName);
        if (candidateArtifact != nullptr)
        {
            *candidateArtifact = artifact;
        }

        // Candidate 生成器的 pipeline 只服务本次资源 prepare，不能污染
        // active compute cache；资源事务失败时它会随局部 shared_ptr 释放。
        return CreateComputePipeline(artifact);
    }

    return std::shared_ptr<ComputePipeline>(new ComputePipeline(
        rendererBackend,
        *device,
        shaderName));
}

std::shared_ptr<ComputePipeline> PipelineFactory::CreateComputePipeline(
    const ComputeShaderArtifact& artifact)
{
    // Replacement pipelines are created per reload batch and owned by the
    // participant; they intentionally bypass the weak process cache.
    return std::shared_ptr<ComputePipeline>(new ComputePipeline(
        rendererBackend,
        *device,
        artifact));
}

const GraphicsShaderVariantArtifact& PipelineFactory::PrepareGraphicsShaderVariant(
    const ShaderVariantKey& shaderVariantKey)
{
    const ShaderVariantKey normalizedKey = NormalizeShaderVariantKey(shaderVariantKey);
    const std::string cacheKey =
        GetGraphicsShaderVariantCacheKey(normalizedKey);
    auto artifactIt = graphicsShaderVariants.find(cacheKey);
    if (artifactIt != graphicsShaderVariants.end())
    {
        // 进程内缓存按逻辑变体键索引，但磁盘缓存以源码代际为准。命中时必须
        // 重新确认当前源码代际，避免 M_ 定义或源文件变化后继续复用旧 artifact。
        const VL::ShaderBuildArtifact freshBuild =
            GetShaderCompiler().EnsureGraphicsVariantCompiled(normalizedKey);
        GraphicsShaderVariantArtifact freshArtifact =
            BuildGraphicsShaderVariantArtifact(
                freshBuild,
                normalizedKey.GetDisplayName());
        if (freshArtifact.artifactGenerationKey !=
            artifactIt->second.artifactGenerationKey)
        {
            artifactIt->second = std::move(freshArtifact);
        }
        return artifactIt->second;
    }

    const VL::ShaderBuildArtifact buildArtifact =
        GetShaderCompiler().EnsureGraphicsVariantCompiled(normalizedKey);
    GraphicsShaderVariantArtifact artifact = BuildGraphicsShaderVariantArtifact(
        buildArtifact,
        normalizedKey.GetDisplayName());
    auto insertedResult = graphicsShaderVariants.emplace(cacheKey, std::move(artifact));
    return insertedResult.first->second;
}

const GraphicsShaderVariantArtifact& PipelineFactory::PrepareMaterialShaderVariant(
    const VL::MaterialShaderCompileRequest& requestInput)
{
    VL::MaterialShaderCompileRequest request = requestInput;
    request.shaderVariantKey.macros =
        NormalizeMaterialMacros(std::move(request.shaderVariantKey.macros));
    const std::string cacheKey =
        GetMaterialShaderVariantCacheKey(request);
    auto artifactIt = graphicsShaderVariants.find(cacheKey);
    if (artifactIt != graphicsShaderVariants.end())
    {
        const VL::ComposedMaterialShaderSource freshSource =
            VL::MaterialShaderComposer::Compose(request);
        const VL::ShaderBuildArtifact freshBuild =
            GetShaderCompiler()
                .EnsureMaterialGraphicsVariantCompiled(
                    request,
                    freshSource);
        GraphicsShaderVariantArtifact freshArtifact =
            BuildGraphicsShaderVariantArtifact(
                freshBuild,
                request.GetDisplayName());
        if (freshArtifact.artifactGenerationKey !=
            artifactIt->second.artifactGenerationKey)
        {
            artifactIt->second = std::move(freshArtifact);
        }
        return artifactIt->second;
    }

    const VL::ComposedMaterialShaderSource source =
        VL::MaterialShaderComposer::Compose(request);
    const VL::ShaderBuildArtifact buildArtifact =
        GetShaderCompiler().EnsureMaterialGraphicsVariantCompiled(
            request,
            source);
    GraphicsShaderVariantArtifact artifact = BuildGraphicsShaderVariantArtifact(
        buildArtifact,
        request.GetDisplayName());
    auto insertedResult = graphicsShaderVariants.emplace(cacheKey, std::move(artifact));
    return insertedResult.first->second;
}

const GraphicsShaderVariantArtifact&
PipelineFactory::PrepareGraphicsShaderVariantCandidate(
    GraphicsCandidateState& candidate,
    const ShaderVariantKey& shaderVariantKeyInput)
{
    const ShaderVariantKey shaderVariantKey =
        NormalizeShaderVariantKey(shaderVariantKeyInput);
    const std::string cacheKey =
        GetGraphicsShaderVariantCacheKey(shaderVariantKey);
    const auto candidateIt =
        candidate.shaderVariants.find(cacheKey);
    if (candidateIt != candidate.shaderVariants.end())
    {
        return candidateIt->second;
    }

    VL::ShaderBuildRequest request =
        GetShaderCompiler().CreateGraphicsVariantBuildRequest(
            shaderVariantKey);
    GetShaderCompiler().FreezeSourceSnapshot(request);
    VL::ShaderBuildArtifact buildArtifact =
        GetShaderCompiler().PrepareCandidate(request);
    GraphicsShaderVariantArtifact artifact =
        BuildGraphicsShaderVariantArtifact(
            buildArtifact,
            shaderVariantKey.GetDisplayName());

    const auto activeIt = graphicsShaderVariants.find(cacheKey);
    if (activeIt == graphicsShaderVariants.end() ||
        activeIt->second.artifactGenerationKey !=
            artifact.artifactGenerationKey)
    {
        candidate.publications.push_back(
            {cacheKey, artifact});
    }
    candidate.shaderBuildArtifacts.push_back(
        {std::move(request), std::move(buildArtifact)});
    return candidate.shaderVariants.emplace(
        cacheKey,
        std::move(artifact)).first->second;
}

const GraphicsShaderVariantArtifact&
PipelineFactory::PrepareMaterialShaderVariantCandidate(
    GraphicsCandidateState& candidate,
    const VL::MaterialShaderCompileRequest& requestInput)
{
    VL::MaterialShaderCompileRequest request = requestInput;
    request.shaderVariantKey.macros =
        NormalizeMaterialMacros(
            std::move(request.shaderVariantKey.macros));
    const std::string cacheKey =
        GetMaterialShaderVariantCacheKey(request);
    const auto candidateIt =
        candidate.shaderVariants.find(cacheKey);
    if (candidateIt != candidate.shaderVariants.end())
    {
        return candidateIt->second;
    }

    const VL::ComposedMaterialShaderSource source =
        VL::MaterialShaderComposer::Compose(request);
    VL::ShaderBuildRequest buildRequest =
        GetShaderCompiler().CreateMaterialGraphicsBuildRequest(
            request,
            source);
    GetShaderCompiler().FreezeSourceSnapshot(buildRequest);
    VL::ShaderBuildArtifact buildArtifact =
        GetShaderCompiler().PrepareCandidate(buildRequest);
    GraphicsShaderVariantArtifact artifact =
        BuildGraphicsShaderVariantArtifact(
            buildArtifact,
            request.GetDisplayName());

    const auto activeIt = graphicsShaderVariants.find(cacheKey);
    if (activeIt == graphicsShaderVariants.end() ||
        activeIt->second.artifactGenerationKey !=
            artifact.artifactGenerationKey)
    {
        candidate.publications.push_back(
            {cacheKey, artifact});
    }
    candidate.shaderBuildArtifacts.push_back(
        {std::move(buildRequest), std::move(buildArtifact)});
    return candidate.shaderVariants.emplace(
        cacheKey,
        std::move(artifact)).first->second;
}

std::shared_ptr<PipelineBase> PipelineFactory::CreateGraphicsPipeline(
    vk::RenderPass* renderPass,
    const PassPipelineContractKey& passPipelineContractKey,
    const GraphicsShaderVariantArtifact& shaderArtifact,
    vk::CullModeFlags cullMode,
    GraphicsPipelineBlendMode blendMode,
    const GraphicsPipelineLayoutDesc& pipelineLayoutDesc)
{
    return CreateGraphicsPipelineInternal(
        graphicsPipelines,
        renderPass,
        passPipelineContractKey,
        shaderArtifact,
        cullMode,
        blendMode,
        pipelineLayoutDesc);
}

std::shared_ptr<PipelineBase>
PipelineFactory::CreateGraphicsPipelineCandidate(
    GraphicsCandidateState& candidate,
    vk::RenderPass* renderPass,
    const PassPipelineContractKey& passPipelineContractKey,
    const GraphicsShaderVariantArtifact& shaderArtifact,
    vk::CullModeFlags cullMode,
    GraphicsPipelineBlendMode blendMode,
    const GraphicsPipelineLayoutDesc& pipelineLayoutDesc)
{
    return CreateGraphicsPipelineInternal(
        candidate.graphicsPipelines,
        renderPass,
        passPipelineContractKey,
        shaderArtifact,
        cullMode,
        blendMode,
        pipelineLayoutDesc);
}

std::shared_ptr<PipelineBase>
PipelineFactory::CreateGraphicsPipelineInternal(
    std::unordered_map<
        GraphicsPipelineKey,
        std::weak_ptr<GraphicsPipeline>,
        GraphicsPipelineKeyHash>& pipelineCache,
    vk::RenderPass* renderPass,
    const PassPipelineContractKey& passPipelineContractKey,
    const GraphicsShaderVariantArtifact& shaderArtifact,
    vk::CullModeFlags cullMode,
    GraphicsPipelineBlendMode blendMode,
    const GraphicsPipelineLayoutDesc& pipelineLayoutDesc)
{
    // Graphics pipeline 身份由 Pass 合同、shader variant、Material 固定状态和
    // layout 合同共同组成。renderPass 指针本身不进 key，兼容 pass 可共享管线。
    GraphicsPipelineKey key{
        passPipelineContractKey.GetNormalizedKey(),
        shaderArtifact.normalizedKey + "@" +
            shaderArtifact.artifactGenerationKey,
        cullMode,
        blendMode,
        pipelineLayoutDesc.GetNormalizedKey()
    };
    auto it = pipelineCache.find(key);
    if (it != pipelineCache.end())
    {
        auto pipeline = it->second.lock();
        if (pipeline)
        {
            return pipeline;
        }
    }

    ++uncachedGraphicsPipelineCreationCount;
    if (testFaultInjection.failGraphicsPipelineCreationAt != 0 &&
        uncachedGraphicsPipelineCreationCount ==
            testFaultInjection.failGraphicsPipelineCreationAt)
    {
        testFaultInjection.failGraphicsPipelineCreationAt = 0;
        throw std::runtime_error(
            "Injected graphics pipeline creation failure");
    }

    const GraphicsPipelineStateDesc pipelineStateDesc =
        passPipelineContractKey.BuildGraphicsPipelineStateDesc(cullMode, blendMode);
    // PassPipelineContractKey 已包含 Pass 默认状态和强类型 RenderMode 派生状态，
    // cull/blend 来自 Material；这里只在 Vulkan 管线创建边界合成为最终 state desc。
    auto pipeline = std::shared_ptr<GraphicsPipeline>(new GraphicsPipeline(
        rendererBackend,
        *device,
        renderPass,
        shaderArtifact,
        passPipelineContractKey.renderPassCompatibilityKey.GetRasterizationSampleCount(),
        passPipelineContractKey.renderPassCompatibilityKey.GetColorAttachmentCount(),
        pipelineStateDesc,
        passPipelineContractKey.isShadowPass,
        pipelineLayoutDesc));
    pipelineCache[key] = pipeline;
    return pipeline;
}

PipelineFactory::PreparedGraphicsCandidateCommit
PipelineFactory::PrepareCandidateCommit(
    const GraphicsCandidateState& candidate) const
{
    PreparedGraphicsCandidateCommit prepared;
    prepared.shaderVariants = graphicsShaderVariants;
    prepared.graphicsPipelines = graphicsPipelines;

    for (const GraphicsShaderArtifactPublication& publication :
         candidate.publications)
    {
        const auto activeIt =
            prepared.shaderVariants.find(publication.cacheKey);
        if (activeIt != prepared.shaderVariants.end() &&
            activeIt->second.logicalBuildId !=
                publication.artifact.logicalBuildId)
        {
            throw std::runtime_error(
                "Candidate shader publication changed logical build identity");
        }
        prepared.shaderVariants.insert_or_assign(
            publication.cacheKey,
            publication.artifact);
    }
    for (const auto& [key, pipeline] :
         candidate.graphicsPipelines)
    {
        prepared.graphicsPipelines.insert_or_assign(
            key,
            pipeline);
    }
    return prepared;
}

void PipelineFactory::CommitPreparedCandidate(
    PreparedGraphicsCandidateCommit candidate) noexcept
{
    graphicsShaderVariants.swap(candidate.shaderVariants);
    graphicsPipelines.swap(candidate.graphicsPipelines);
}

void PipelineFactory::PublishGraphicsShaderVariantArtifacts(
    std::vector<GraphicsShaderArtifactPublication>& publications)
{
    ValidateGraphicsShaderVariantArtifactPublications(publications);
    GraphicsCandidateState candidate;
    candidate.publications.reserve(publications.size());
    for (GraphicsShaderArtifactPublication& publication : publications)
    {
        candidate.publications.push_back({
            std::move(publication.cacheKey),
            std::move(publication.artifact)});
    }
    CommitPreparedCandidate(
        PrepareCandidateCommit(candidate));
}

void PipelineFactory::ValidateGraphicsShaderVariantArtifactPublications(
    const std::vector<GraphicsShaderArtifactPublication>& publications) const
{
    for (const GraphicsShaderArtifactPublication& publication : publications)
    {
        const auto artifactIt = graphicsShaderVariants.find(
            publication.cacheKey);
        if (artifactIt == graphicsShaderVariants.end())
        {
            throw std::runtime_error(
                "Cannot publish a shader reload generation for an unknown active variant: " +
                publication.cacheKey);
        }
        if (artifactIt->second.logicalBuildId !=
            publication.artifact.logicalBuildId)
        {
            throw std::runtime_error(
                "Cannot publish a shader reload generation with a different logical build identity");
        }
    }
}

void PipelineFactory::SetTestFaultInjection(TestFaultInjection injection)
{
    testFaultInjection = injection;
    uncachedGraphicsPipelineCreationCount = 0;
}

std::string PipelineFactory::CaptureIdentityFingerprintForTest(
    bool includeWeakPipelineLiveness) const
{
    std::vector<std::string> entries;
    entries.reserve(
        computePipelines.size() +
        graphicsShaderVariants.size() +
        graphicsPipelines.size());
    for (const auto& [key, entry] :
         computePipelines)
    {
        entries.push_back(
            "compute:" + key + ":" +
            entry.artifactGenerationKey +
            (includeWeakPipelineLiveness
                ? ":" +
                    std::to_string(
                        reinterpret_cast<std::uintptr_t>(
                            entry.pipeline.lock().get()))
                : std::string()));
    }
    for (const auto& [key, artifact] :
         graphicsShaderVariants)
    {
        entries.push_back(
            "artifact:" + key + ":" +
            artifact.logicalBuildId + ":" +
            artifact.artifactGenerationKey);
    }
    for (const auto& [key, pipeline] :
         graphicsPipelines)
    {
        std::ostringstream stream;
        stream << "pipeline:" <<
            key.passPipelineContractKey << ":" <<
            key.shaderArtifactKey << ":" <<
            static_cast<uint32_t>(key.cullMode) << ":" <<
            static_cast<uint32_t>(key.blendMode) << ":" <<
            key.pipelineLayoutKey;
        if (includeWeakPipelineLiveness)
        {
            stream << ":" <<
                reinterpret_cast<std::uintptr_t>(
                    pipeline.lock().get());
        }
        entries.push_back(stream.str());
    }
    std::sort(entries.begin(), entries.end());
    std::ostringstream fingerprint;
    for (const std::string& entry : entries)
    {
        fingerprint << entry << ";";
    }
    return fingerprint.str();
}

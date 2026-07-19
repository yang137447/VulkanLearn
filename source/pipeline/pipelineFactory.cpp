#include "pipelineFactory.h"
#include "computePipeline.h"
#include "graphicsPipeline.h"
#include "shaderCompiler.h"
#include "shaderReflectionService.h"
#include "material/compiler/materialShaderComposer.h"
#include <cstdint>
#include <stdexcept>
#include <utility>

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

PipelineFactory::PipelineFactory(vk::Device* device)
{
    this->device = device;
}

std::shared_ptr<ComputePipeline> PipelineFactory::CreateComputePipeline(const std::string& shaderName)
{
    auto it = computePipelines.find(shaderName);
    if (it != computePipelines.end())
    {
        auto pipeline = it->second.lock();
        if (pipeline)
        {
            return pipeline;
        }
    }

    auto pipeline = std::make_shared<ComputePipeline>(device, shaderName);
    computePipelines[shaderName] = pipeline;
    return pipeline;
}

const GraphicsShaderVariantArtifact& PipelineFactory::PrepareGraphicsShaderVariant(
    const ShaderVariantKey& shaderVariantKey)
{
    const ShaderVariantKey normalizedKey = NormalizeShaderVariantKey(shaderVariantKey);
    const std::string cacheKey = normalizedKey.GetNormalizedKey();
    auto artifactIt = graphicsShaderVariants.find(cacheKey);
    if (artifactIt != graphicsShaderVariants.end())
    {
        return artifactIt->second;
    }

    const ShaderCompiler::ShaderVariantCompileResult compileResult =
        ShaderCompiler::EnsureGraphicsVariantCompiled(normalizedKey);
    GraphicsShaderVariantArtifact artifact;
    artifact.normalizedKey = normalizedKey.GetNormalizedKey();
    artifact.displayName = normalizedKey.GetDisplayName();
    artifact.vertexSpvPath = compileResult.vertexSpvPath;
    artifact.fragmentSpvPath = compileResult.fragmentSpvPath;
    artifact.vertexDebugPath = compileResult.vertexDebugPath;
    artifact.fragmentDebugPath = compileResult.fragmentDebugPath;
    artifact.shaderBindings = ShaderReflectionService::ReflectFromDebugSpirvFiles({
        artifact.vertexDebugPath,
        artifact.fragmentDebugPath});
    auto insertedResult = graphicsShaderVariants.emplace(cacheKey, std::move(artifact));
    return insertedResult.first->second;
}

const GraphicsShaderVariantArtifact& PipelineFactory::PrepareMaterialShaderVariant(
    const VL::MaterialShaderCompileRequest& requestInput)
{
    VL::MaterialShaderCompileRequest request = requestInput;
    request.shaderVariantKey.macros =
        NormalizeMaterialMacros(std::move(request.shaderVariantKey.macros));
    const std::string cacheKey = request.GetNormalizedKey();
    auto artifactIt = graphicsShaderVariants.find(cacheKey);
    if (artifactIt != graphicsShaderVariants.end())
    {
        return artifactIt->second;
    }

    const VL::ComposedMaterialShaderSource source =
        VL::MaterialShaderComposer::Compose(request);
    const ShaderCompiler::ShaderVariantCompileResult compileResult =
        ShaderCompiler::EnsureMaterialGraphicsVariantCompiled(request, source);

    GraphicsShaderVariantArtifact artifact;
    artifact.normalizedKey = cacheKey;
    artifact.displayName = request.GetDisplayName();
    artifact.vertexSpvPath = compileResult.vertexSpvPath;
    artifact.fragmentSpvPath = compileResult.fragmentSpvPath;
    artifact.vertexDebugPath = compileResult.vertexDebugPath;
    artifact.fragmentDebugPath = compileResult.fragmentDebugPath;
    artifact.shaderBindings = ShaderReflectionService::ReflectFromDebugSpirvFiles({
        artifact.vertexDebugPath,
        artifact.fragmentDebugPath});
    auto insertedResult = graphicsShaderVariants.emplace(cacheKey, std::move(artifact));
    return insertedResult.first->second;
}

std::shared_ptr<PipelineBase> PipelineFactory::CreateGraphicsPipeline(
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
        shaderArtifact.normalizedKey,
        cullMode,
        blendMode,
        pipelineLayoutDesc.GetNormalizedKey()
    };
    auto it = graphicsPipelines.find(key);
    if (it != graphicsPipelines.end())
    {
        auto pipeline = it->second.lock();
        if (pipeline)
        {
            return pipeline;
        }
    }

    const GraphicsPipelineStateDesc pipelineStateDesc =
        passPipelineContractKey.BuildGraphicsPipelineStateDesc(cullMode, blendMode);
    // PassPipelineContractKey 提供 pass-owned 状态，cull/blend 来自 Material；
    // 这里只在 Vulkan 管线创建边界把两部分合成为最终 state desc。
    auto pipeline = std::make_shared<GraphicsPipeline>(
        device,
        renderPass,
        shaderArtifact,
        passPipelineContractKey.renderPassCompatibilityKey.GetRasterizationSampleCount(),
        passPipelineContractKey.renderPassCompatibilityKey.GetColorAttachmentCount(),
        pipelineStateDesc,
        passPipelineContractKey.isShadowPass,
        pipelineLayoutDesc);
    graphicsPipelines[key] = pipeline;
    return pipeline;
}

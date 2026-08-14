#pragma once

#include <cstddef>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "vulkan/vulkan.hpp"
#include "graphicsPipelineBuilder.h"
#include "graphicsPipelineLayoutDesc.h"
#include "graphicsShaderVariantArtifact.h"
#include "passPipelineContractKey.h"
#include "../shaderVariant.h"
#include "material/compiler/materialShaderCompileRequest.h"
#include "shader/reload/computeShaderArtifact.h"

class ComputePipeline;
class GraphicsPipeline;
class PipelineBase;
class ShaderCompiler;
namespace VL
{
class RendererBackendVulkan;
}
namespace vk
{
    class Device;
}

struct GraphicsPipelineKey
{
    std::string passPipelineContractKey;
    std::string shaderArtifactKey;
    vk::CullModeFlags cullMode;
    GraphicsPipelineBlendMode blendMode;
    std::string pipelineLayoutKey;

    bool operator==(const GraphicsPipelineKey& other) const
    {
        return passPipelineContractKey == other.passPipelineContractKey &&
            shaderArtifactKey == other.shaderArtifactKey &&
            cullMode == other.cullMode &&
            blendMode == other.blendMode &&
            pipelineLayoutKey == other.pipelineLayoutKey;
    }
};

struct GraphicsPipelineKeyHash
{
    size_t operator()(const GraphicsPipelineKey& key) const;
};

struct GraphicsShaderArtifactPublication
{
    std::string cacheKey;
    GraphicsShaderVariantArtifact artifact;
};

class PipelineFactory
{
public:
    struct TestFaultInjection
    {
        // Zero disables injection. A positive value fails that numbered
        // uncached graphics-pipeline creation and then clears itself.
        size_t failGraphicsPipelineCreationAt = 0;
    };

    struct GraphicsCandidateState
    {
        struct PreparedShaderBuild
        {
            VL::ShaderBuildRequest request;
            VL::ShaderBuildArtifact artifact;
        };

        std::unordered_map<std::string, GraphicsShaderVariantArtifact>
            shaderVariants;
        std::unordered_map<
            GraphicsPipelineKey,
            std::weak_ptr<GraphicsPipeline>,
            GraphicsPipelineKeyHash>
            graphicsPipelines;
        std::vector<PreparedShaderBuild> shaderBuildArtifacts;
        std::vector<GraphicsShaderArtifactPublication> publications;
    };

    struct PreparedGraphicsCandidateCommit
    {
        std::unordered_map<std::string, GraphicsShaderVariantArtifact>
            shaderVariants;
        std::unordered_map<
            GraphicsPipelineKey,
            std::weak_ptr<GraphicsPipeline>,
            GraphicsPipelineKeyHash>
            graphicsPipelines;
    };

    void SetShaderCompiler(ShaderCompiler* shaderCompiler);
    ShaderCompiler& GetShaderCompiler() const;
    static std::string GetGraphicsShaderVariantCacheKey(
        const ShaderVariantKey& shaderVariantKey);
    static std::string GetMaterialShaderVariantCacheKey(
        const VL::MaterialShaderCompileRequest& request);
    std::shared_ptr<ComputePipeline> CreateComputePipeline(
        const std::string& shaderName,
        ComputeShaderArtifact* activeArtifact = nullptr);
    std::shared_ptr<ComputePipeline> CreateComputePipeline(
        const ComputeShaderArtifact& artifact);
    const GraphicsShaderVariantArtifact& PrepareGraphicsShaderVariant(
        const ShaderVariantKey& shaderVariantKey);
    // 將 Material Evaluation 與固定 Pass Template 組合後編譯並反射；
    // cache key 使用完整 MaterialShaderCompileRequest identity。
    const GraphicsShaderVariantArtifact& PrepareMaterialShaderVariant(
        const VL::MaterialShaderCompileRequest& request);
    const GraphicsShaderVariantArtifact& PrepareGraphicsShaderVariantCandidate(
        GraphicsCandidateState& candidate,
        const ShaderVariantKey& shaderVariantKey);
    const GraphicsShaderVariantArtifact& PrepareMaterialShaderVariantCandidate(
        GraphicsCandidateState& candidate,
        const VL::MaterialShaderCompileRequest& request);
    std::shared_ptr<PipelineBase> CreateGraphicsPipeline(
        vk::RenderPass* renderPass,
        const PassPipelineContractKey& passPipelineContractKey,
        const GraphicsShaderVariantArtifact& shaderArtifact,
        vk::CullModeFlags cullMode,
        GraphicsPipelineBlendMode blendMode,
        const GraphicsPipelineLayoutDesc& pipelineLayoutDesc = {});
    std::shared_ptr<PipelineBase> CreateGraphicsPipelineCandidate(
        GraphicsCandidateState& candidate,
        vk::RenderPass* renderPass,
        const PassPipelineContractKey& passPipelineContractKey,
        const GraphicsShaderVariantArtifact& shaderArtifact,
        vk::CullModeFlags cullMode,
        GraphicsPipelineBlendMode blendMode,
        const GraphicsPipelineLayoutDesc& pipelineLayoutDesc = {});
    PreparedGraphicsCandidateCommit PrepareCandidateCommit(
        const GraphicsCandidateState& candidate) const;
    void CommitPreparedCandidate(
        PreparedGraphicsCandidateCommit candidate) noexcept;
    void PublishGraphicsShaderVariantArtifacts(
        std::vector<GraphicsShaderArtifactPublication>& publications);
    void ValidateGraphicsShaderVariantArtifactPublications(
        const std::vector<GraphicsShaderArtifactPublication>& publications) const;
    void SetTestFaultInjection(TestFaultInjection injection);
    std::string CaptureIdentityFingerprintForTest(
        bool includeWeakPipelineLiveness = true) const;
private:
    friend class VL::RendererBackendVulkan;

    PipelineFactory(
        VL::RendererBackendVulkan* rendererBackend,
        vk::Device& device);

    struct CachedComputePipeline
    {
        std::weak_ptr<ComputePipeline> pipeline;
        std::string artifactGenerationKey;
    };

    VL::RendererBackendVulkan* rendererBackend = nullptr;
    vk::Device* device = nullptr;
    ShaderCompiler* shaderCompiler = nullptr;
    std::unordered_map<std::string, CachedComputePipeline> computePipelines;
    std::unordered_map<std::string, GraphicsShaderVariantArtifact> graphicsShaderVariants;
    std::unordered_map<GraphicsPipelineKey, std::weak_ptr<GraphicsPipeline>, GraphicsPipelineKeyHash> graphicsPipelines;
    TestFaultInjection testFaultInjection;
    size_t uncachedGraphicsPipelineCreationCount = 0;

    std::shared_ptr<PipelineBase> CreateGraphicsPipelineInternal(
        std::unordered_map<
            GraphicsPipelineKey,
            std::weak_ptr<GraphicsPipeline>,
            GraphicsPipelineKeyHash>& pipelineCache,
        vk::RenderPass* renderPass,
        const PassPipelineContractKey& passPipelineContractKey,
        const GraphicsShaderVariantArtifact& shaderArtifact,
        vk::CullModeFlags cullMode,
        GraphicsPipelineBlendMode blendMode,
        const GraphicsPipelineLayoutDesc& pipelineLayoutDesc);
};

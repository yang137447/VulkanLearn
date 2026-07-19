#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <unordered_map>
#include "vulkan/vulkan.hpp"
#include "graphicsPipelineBuilder.h"
#include "graphicsPipelineLayoutDesc.h"
#include "graphicsShaderVariantArtifact.h"
#include "passPipelineContractKey.h"
#include "../shaderVariant.h"
#include "material/compiler/materialShaderCompileRequest.h"

class ComputePipeline;
class GraphicsPipeline;
class PipelineBase;
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

class PipelineFactory
{
public:
    PipelineFactory(vk::Device* device);
    std::shared_ptr<ComputePipeline> CreateComputePipeline(const std::string& shaderName);
    const GraphicsShaderVariantArtifact& PrepareGraphicsShaderVariant(
        const ShaderVariantKey& shaderVariantKey);
    // 將 Material Evaluation 與固定 Pass Template 組合後編譯並反射；
    // cache key 使用完整 MaterialShaderCompileRequest identity。
    const GraphicsShaderVariantArtifact& PrepareMaterialShaderVariant(
        const VL::MaterialShaderCompileRequest& request);
    std::shared_ptr<PipelineBase> CreateGraphicsPipeline(
        vk::RenderPass* renderPass,
        const PassPipelineContractKey& passPipelineContractKey,
        const GraphicsShaderVariantArtifact& shaderArtifact,
        vk::CullModeFlags cullMode,
        GraphicsPipelineBlendMode blendMode,
        const GraphicsPipelineLayoutDesc& pipelineLayoutDesc = {});
private:
    vk::Device* device;
    std::unordered_map<std::string, std::weak_ptr<ComputePipeline>> computePipelines;
    std::unordered_map<std::string, GraphicsShaderVariantArtifact> graphicsShaderVariants;
    std::unordered_map<GraphicsPipelineKey, std::weak_ptr<GraphicsPipeline>, GraphicsPipelineKeyHash> graphicsPipelines;
};

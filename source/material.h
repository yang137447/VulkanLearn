#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "pipeline/graphicsPipelineBuilder.h"
#include "pipeline/passPipelineContractKey.h"
#include "shaderVariant.h"
#include "material/compiler/materialShaderCompileRequest.h"
#include "material/materialDescriptorSchema.h"

class MaterialInstance; // Forward declaration
class PipelineBase;
class PipelineFactory;
class Material: public std::enable_shared_from_this<Material>
{
public:
    ~Material() = default;
    Material(
        PipelineFactory& pipelineFactory,
        vk::RenderPass* renderPass,
        const PassPipelineContractKey& passPipelineContractKey,
        const ShaderVariantKey& shaderVariantKey,
        const VL::MaterialFeatureKey& materialFeatureKey,
        const VL::MaterialDescriptorSchema& materialDescriptorSchema,
        const std::optional<VL::MaterialShaderCompileRequest>& baseShaderCompileRequest,
        const std::string& materialKey,
        vk::CullModeFlags cullMode,
        GraphicsPipelineBlendMode blendMode);

    std::shared_ptr<MaterialInstance> CreateInstance();

    const std::string& GetShaderName() const{ return shaderVariantKey.shaderName; }
    const ShaderVariantKey& GetShaderVariantKey() const { return shaderVariantKey; }
    const std::string& GetMaterialKey() const { return materialKey; }
    const PassPipelineContractKey& GetPassPipelineContractKey() const { return passPipelineContractKey; }
    const VL::MaterialFeatureKey& GetMaterialFeatureKey() const { return materialFeatureKey; }
    const VL::MaterialDescriptorSchema& GetMaterialDescriptorSchema() const { return materialDescriptorSchema; }
    const std::vector<ShaderBinding>& GetActiveShaderBindings() const { return activeShaderBindings; }
    const std::shared_ptr<PipelineBase>& GetRenderPipeline() const { return renderPipeline; }
    const std::shared_ptr<PipelineBase>& GetShadowPipeline() const { return shadowPipeline; }
    void SetShadowPipeline(std::shared_ptr<PipelineBase> pipeline);
    
private:
    ShaderVariantKey shaderVariantKey;
    VL::MaterialFeatureKey materialFeatureKey;
    VL::MaterialDescriptorSchema materialDescriptorSchema;
    std::string materialKey;
    PassPipelineContractKey passPipelineContractKey;
    std::vector<ShaderBinding> activeShaderBindings;
    std::shared_ptr<PipelineBase> renderPipeline;
    std::shared_ptr<PipelineBase> shadowPipeline;
};

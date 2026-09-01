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
#include "pipeline/pipelineFactory.h"
#include "shaderVariant.h"
#include "material/compiler/materialShaderCompileRequest.h"
#include "material/materialDescriptorSchema.h"
#include "material/materialPipelineReload.h"

class MaterialInstance; // Forward declaration
class PipelineBase;
struct Renderpass;
class Material: public std::enable_shared_from_this<Material>
{
public:
    ~Material() = default;
    Material(
        PipelineFactory& pipelineFactory,
        Renderpass& renderPass,
        const PassPipelineContractKey& surfacePassPipelineContractKey,
        const ShaderVariantKey& shaderVariantKey,
        const VL::MaterialFeatureKey& materialFeatureKey,
        const VL::MaterialDescriptorSchema& materialDescriptorSchema,
        const std::optional<VL::MaterialShaderCompileRequest>& baseShaderCompileRequest,
        const std::string& materialKey,
        vk::CullModeFlags cullMode,
        GraphicsPipelineBlendMode blendMode,
        PipelineFactory::GraphicsCandidateState* candidateState = nullptr);

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
    const VL::MaterialPipelineReloadRecipe& GetPipelineReloadRecipe() const
    {
        return pipelineReloadRecipe;
    }
    const GraphicsShaderVariantArtifact& GetSurfaceShaderArtifact() const
    {
        return surfaceShaderArtifact;
    }
    const std::optional<GraphicsShaderVariantArtifact>& GetShadowShaderArtifact() const
    {
        return shadowShaderArtifact;
    }
    static std::vector<ShaderBinding> BuildActiveShaderBindings(
        const std::vector<ShaderBinding>& surfaceBindings,
        const std::vector<ShaderBinding>* shadowBindings);
    void SetShadowPipeline(
        std::shared_ptr<PipelineBase> pipeline,
        const GraphicsShaderVariantArtifact& shaderArtifact,
        const VL::MaterialGraphicsPassReloadRecipe& reloadRecipe);
    void ValidatePipelineReloadCommit(
        const VL::MaterialPipelineReloadCommit& commit) const;
    VL::RetiredMaterialPipelines CommitPipelineReload(
        VL::MaterialPipelineReloadCommit commit) noexcept;
    
private:
    ShaderVariantKey shaderVariantKey;
    VL::MaterialFeatureKey materialFeatureKey;
    VL::MaterialDescriptorSchema materialDescriptorSchema;
    std::string materialKey;
    PassPipelineContractKey passPipelineContractKey;
    std::vector<ShaderBinding> activeShaderBindings;
    std::shared_ptr<PipelineBase> renderPipeline;
    std::shared_ptr<PipelineBase> shadowPipeline;
    VL::MaterialPipelineReloadRecipe pipelineReloadRecipe;
    GraphicsShaderVariantArtifact surfaceShaderArtifact;
    std::optional<GraphicsShaderVariantArtifact> shadowShaderArtifact;
};

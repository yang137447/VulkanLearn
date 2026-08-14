#pragma once

// File responsibility: Defines immutable, Vulkan-handle-free recipes for
// rebuilding a Material's graphics pipelines and the value package used for
// an all-or-nothing runtime commit.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "material/compiler/materialShaderCompileRequest.h"
#include "pipeline/graphicsPipelineBuilder.h"
#include "pipeline/graphicsShaderVariantArtifact.h"
#include "pipeline/passPipelineContractKey.h"
#include "shaderVariant.h"

class PipelineBase;

namespace VL
{

enum class GraphicsShaderReloadRecipeKind
{
    StandaloneVariant,
    MaterialComposed
};

struct GraphicsShaderReloadRecipe
{
    GraphicsShaderReloadRecipeKind kind =
        GraphicsShaderReloadRecipeKind::StandaloneVariant;
    ShaderVariantKey shaderVariantKey;
    std::optional<MaterialShaderCompileRequest> materialCompileRequest;
};

enum class MaterialPipelineLayoutRecipeKind
{
    SurfaceMaterialSchema,
    ShadowInheritedSurface
};

struct MaterialGraphicsPassReloadRecipe
{
    std::string passName;
    PassPipelineContractKey passPipelineContractKey;
    GraphicsShaderReloadRecipe shader;
    vk::CullModeFlags cullMode = vk::CullModeFlagBits::eBack;
    GraphicsPipelineBlendMode blendMode = GraphicsPipelineBlendMode::Opaque;
    MaterialPipelineLayoutRecipeKind layoutKind =
        MaterialPipelineLayoutRecipeKind::SurfaceMaterialSchema;
};

struct MaterialPipelineReloadRecipe
{
    MaterialGraphicsPassReloadRecipe surface;
    std::optional<MaterialGraphicsPassReloadRecipe> shadow;
};

struct MaterialPipelineReloadCommit
{
    bool replaceSurface = false;
    bool replaceShadow = false;
    std::shared_ptr<PipelineBase> surfacePipeline;
    std::shared_ptr<PipelineBase> shadowPipeline;
    GraphicsShaderVariantArtifact surfaceArtifact;
    std::optional<GraphicsShaderVariantArtifact> shadowArtifact;
    std::vector<ShaderBinding> activeShaderBindings;
};

struct RetiredMaterialPipelines
{
    std::shared_ptr<PipelineBase> surfacePipeline;
    std::shared_ptr<PipelineBase> shadowPipeline;
};

} // namespace VL

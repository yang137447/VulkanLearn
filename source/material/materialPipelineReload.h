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
    // 局部阴影是独立 pass 变体；与 CSM Shadow 共用同一事务提交，
    // 但不共享 pipeline/layout，避免把 fragment gl_FragDepth 变体
    // 误用到普通 shadow pass。
    std::optional<MaterialGraphicsPassReloadRecipe> localShadow;
};

struct MaterialPipelineReloadCommit
{
    bool replaceSurface = false;
    bool replaceShadow = false;
    bool replaceLocalShadow = false;
    std::shared_ptr<PipelineBase> surfacePipeline;
    std::shared_ptr<PipelineBase> shadowPipeline;
    std::shared_ptr<PipelineBase> localShadowPipeline;
    GraphicsShaderVariantArtifact surfaceArtifact;
    std::optional<GraphicsShaderVariantArtifact> shadowArtifact;
    std::optional<GraphicsShaderVariantArtifact> localShadowArtifact;
    std::vector<ShaderBinding> activeShaderBindings;
};

struct RetiredMaterialPipelines
{
    std::shared_ptr<PipelineBase> surfacePipeline;
    std::shared_ptr<PipelineBase> shadowPipeline;
    std::shared_ptr<PipelineBase> localShadowPipeline;
};

} // namespace VL

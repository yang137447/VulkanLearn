#pragma once

// 文件职责：声明材质专用 Shadow 管线的构建入口，由材质加载阶段调用；
// 普通 Opaque 的公共 Shadow 管线和 ShadowCaster 路由不由本文件负责。
// File responsibility: Declares material-specific shadow pipeline construction for the material loading stage;
// common opaque shadow pipelines and ShadowCaster routing remain outside this file.

#include <memory>
#include <vector>

#include "material/materialPipelineReload.h"
#include "pipeline/graphicsPipelineLayoutDesc.h"
#include "pipeline/pipelineFactory.h"

class Material;
class PipelineBase;
struct MaterialInstanceBuildPlan;
struct Renderpass;

namespace VL
{

class MaterialDescriptorSchema;

struct MaterialShadowPipelineBuildResult
{
    std::shared_ptr<PipelineBase> pipeline;
    GraphicsShaderVariantArtifact shaderArtifact;
    MaterialGraphicsPassReloadRecipe reloadRecipe;
};

// 按显式 `.shadow` override、自动 Material ShadowDepth 的优先级构建材质专用管线。
// 输入来自已验证的 MI 构建计划；普通 Opaque 返回空指针，由渲染场景选择公共管线。
// Builds a material-specific pipeline, preferring an explicit `.shadow` override over generated ShadowDepth.
// Input comes from a validated MI plan; common opaque materials return null for scene-level routing.
MaterialShadowPipelineBuildResult BuildMaterialShadowPipeline(
    PipelineFactory& pipelineFactory,
    Renderpass* canonicalShadowPass,
    const MaterialInstanceBuildPlan& loadPlan,
    const Material& material,
    PipelineFactory::GraphicsCandidateState* candidateState = nullptr);

// 构建局部阴影专用管线：点光/聚光共享 ShadowDepth 模板，但片段阶段写
// gl_FragDepth（八面体方向长度或透视 z）。普通不透明材质返回空，由公共
// shadowLocal 管线绘制；WPO/alpha 材质使用 composed LocalShadow 变体。
MaterialShadowPipelineBuildResult BuildMaterialLocalShadowPipeline(
    PipelineFactory& pipelineFactory,
    Renderpass* canonicalLocalShadowPass,
    const MaterialInstanceBuildPlan& loadPlan,
    const Material& material,
    PipelineFactory::GraphicsCandidateState* candidateState = nullptr);

void ValidateMaterialShadowShaderBindings(
    const std::string& materialName,
    const MaterialDescriptorSchema& materialDescriptorSchema,
    const std::vector<ShaderBinding>& surfaceBindings,
    const std::vector<ShaderBinding>& shadowBindings);

GraphicsPipelineLayoutDesc BuildMaterialShadowPipelineLayout(
    const MaterialDescriptorSchema& materialDescriptorSchema,
    const std::vector<ShaderBinding>& surfaceBindings);

// LocalShadow 在 Surface/Shadow 的 Set 0/2 基础上还要合并 LocalShadow
// shader 实际反射到的 stage；热重载重建必须与初次创建使用同一 layout。
void ValidateMaterialLocalShadowShaderBindings(
    const std::string& materialName,
    const MaterialDescriptorSchema& materialDescriptorSchema,
    const std::vector<ShaderBinding>& surfaceBindings,
    const std::vector<ShaderBinding>& localShadowBindings);

GraphicsPipelineLayoutDesc BuildMaterialLocalShadowPipelineLayout(
    const MaterialDescriptorSchema& materialDescriptorSchema,
    const std::vector<ShaderBinding>& surfaceBindings,
    const std::vector<ShaderBinding>& localShadowBindings,
    const std::string& materialName);

} // namespace VL

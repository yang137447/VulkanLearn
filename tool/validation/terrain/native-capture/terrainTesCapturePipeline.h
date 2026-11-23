#pragma once

#include "material/compiler/materialShaderCompileRequest.h"
#include "pipeline/graphicsShaderVariantArtifact.h"
#include "pipeline/graphicsPipelineLayoutDesc.h"
#include "pipeline/passPipelineContractKey.h"
#include "pipeline/graphicsPipelineBuilder.h"
#include <memory>
#include <string>

class ShaderCompiler;
class PipelineBase;
class PipelineFactory;

namespace VL
{
ShaderBuildArtifact CompileTerrainTesCaptureVariant(ShaderCompiler& compiler,
    MaterialShaderCompileRequest actualMaterialRequest, const std::string& instrumentedTesSource);
GraphicsPipelineLayoutDesc TerrainTesCaptureLayout(const PipelineBase& originalPipeline,
    const GraphicsShaderVariantArtifact& diagnosticArtifact);
std::shared_ptr<PipelineBase> CreateTerrainTesCapturePipeline(PipelineFactory& factory,
    vk::RenderPass& actualRenderPass, const PassPipelineContractKey& actualPassContract,
    const PipelineBase& originalPipeline, const GraphicsShaderVariantArtifact& diagnosticArtifact,
    vk::CullModeFlags actualCullMode, GraphicsPipelineBlendMode actualBlendMode);
}

#pragma once

#include "pipeline/graphicsPipelineLayoutDesc.h"
#include "pipeline/graphicsShaderVariantArtifact.h"
#include "pipeline/passPipelineContractKey.h"
#include "shaderVariant.h"

#include <memory>

class PipelineBase;
class PipelineFactory;

namespace VL
{
ShaderVariantKey TerrainQualityCaptureVariant(ShaderVariantKey regularVariant);
GraphicsPipelineLayoutDesc TerrainQualityReplayLayout(
    const std::vector<ShaderBinding>& regularLayout,
    const GraphicsShaderVariantArtifact& captureArtifact);
std::shared_ptr<PipelineBase> CreateTerrainQualityReplayPipeline(
    PipelineFactory& factory, vk::RenderPass& renderPass,
    PassPipelineContractKey replayContract, const ShaderVariantKey& regularVariant,
    const std::vector<ShaderBinding>& regularLayout);
}

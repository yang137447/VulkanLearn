#pragma once

// 文件职责：定义由 RenderGraph Pass 决定的图形管线身份与固定状态，供 Material 管线创建和
// RenderGraph reload 兼容性检查使用；不包含由 Material 决定的 cull/blend 状态。
// File responsibility: Defines pass-owned graphics pipeline identity and fixed state for material pipeline
// creation and RenderGraph reload checks; material-owned cull and blend state are intentionally excluded.

#include <cstdint>
#include <sstream>
#include <string>

#include <vulkan/vulkan.hpp>

#include "graphicsPipelineBuilder.h"
#include "renderPassCompatibilityKey.h"

// 图形管线身份中由 Pass 持有的部分。RenderGraph reload 或兼容 Pass 复用 Material 时，
// 必须完整比较该契约；cull/blend 由 Material 提供，仅在构建管线状态时合并。
// The pass-owned portion of graphics pipeline identity, compared in full when a material is reused.
// Material cull and blend state are merged only while building the final pipeline state.
struct PassPipelineContractKey
{
    RenderPassCompatibilityKey renderPassCompatibilityKey;
    bool useVertexInput = true;
    bool depthTestEnable = true;
    bool depthWriteEnable = true;
    vk::CompareOp depthCompareOp = vk::CompareOp::eLess;
    bool isShadowPass = false;

    bool operator==(const PassPipelineContractKey& other) const
    {
        return renderPassCompatibilityKey == other.renderPassCompatibilityKey &&
            useVertexInput == other.useVertexInput &&
            depthTestEnable == other.depthTestEnable &&
            depthWriteEnable == other.depthWriteEnable &&
            depthCompareOp == other.depthCompareOp &&
            isShadowPass == other.isShadowPass;
    }

    bool operator!=(const PassPipelineContractKey& other) const
    {
        return !(*this == other);
    }

    std::string GetNormalizedKey() const
    {
        std::ostringstream stream;
        stream << "renderPass=" << renderPassCompatibilityKey.GetNormalizedKey()
               << "|vertexInput=" << useVertexInput
               << "|depthTest=" << depthTestEnable
               << "|depthWrite=" << depthWriteEnable
               << "|depthCompare=" << static_cast<uint32_t>(depthCompareOp)
               << "|shadowPass=" << isShadowPass;
        return stream.str();
    }

    GraphicsPipelineStateDesc BuildGraphicsPipelineStateDesc(
        vk::CullModeFlags cullMode,
        GraphicsPipelineBlendMode blendMode) const
    {
        GraphicsPipelineStateDesc pipelineStateDesc;
        pipelineStateDesc.bUseVertexInput = useVertexInput;
        pipelineStateDesc.bDepthTestEnable = depthTestEnable;
        pipelineStateDesc.bDepthWriteEnable = depthWriteEnable;
        pipelineStateDesc.depthCompareOp = depthCompareOp;
        pipelineStateDesc.cullMode = cullMode;
        pipelineStateDesc.blendMode = blendMode;
        return pipelineStateDesc;
    }
};

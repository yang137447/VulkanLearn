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

// 图形管线身份以 Pass 默认状态为起点。RenderGraph reload 或兼容 Pass 复用 Material 时，
// 必须完整比较该契约；cull/blend 由 Material 提供，少数强类型 RenderMode 可在进入缓存前
// 派生 depthWrite，最终合同仍在构建管线状态时一次性合并。
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

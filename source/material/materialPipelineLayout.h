#pragma once

// 文件职责：把 Material Set 1 完整 schema 与当前 RenderGraph 的 Pass Set 3
// 输入合同合成为无 Vulkan 句柄的管线布局描述；初建与热重载必须复用同一入口。

#include <vector>

#include "pipeline/graphicsPipelineLayoutDesc.h"

struct Renderpass;

namespace VL
{

class MaterialDescriptorSchema;

GraphicsPipelineLayoutDesc BuildMaterialSurfacePipelineLayout(
    const Renderpass& renderPass,
    const MaterialDescriptorSchema& descriptorSchema,
    const std::vector<ShaderBinding>& reflectedShaderBindings);

} // namespace VL

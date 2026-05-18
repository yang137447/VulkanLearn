#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include <vulkan/vulkan.hpp>
#include "pipeline/graphicsPipelineBuilder.h"
#include "shaderReflect.h"
#include "shaderVariant.h"

// 将材质实例 JSON 转成运行时装配所需的关键派生信息。
struct MaterialInstanceBuildPlan
{
    std::string shaderName;
    ShaderVariantKey shaderVariantKey;
    GraphicsPipelineStateDesc pipelineStateDesc;
    bool bIsShadowPass = false;
    std::string materialKey;
    std::string materialInstanceKey;
};

class MaterialInstanceValidator
{
public:
    // 基于材质实例配置和 pass 上下文，推导 shader variant、pipeline state 与缓存 key。
    static MaterialInstanceBuildPlan BuildLoadPlan(
        std::string_view materialInstancePath,
        std::string_view passName,
        vk::SampleCountFlagBits sampleCount,
        const GraphicsPipelineStateDesc& passPipelineStateDesc,
        const nlohmann::json& materialInstanceJson);

    // 用 shader reflection 结果校验材质实例参数和贴图绑定是否与 shader 声明一致。
    static void Validate(
        std::string_view materialInstancePath,
        const nlohmann::json& materialInstanceJson,
        const std::vector<ShaderBinding>& shaderBindings);
};

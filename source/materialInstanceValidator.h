#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>
#include <vulkan/vulkan.hpp>
#include "pipeline/graphicsPipelineBuilder.h"
#include "pipeline/passPipelineContractKey.h"
#include "shaderReflect.h"
#include "shaderVariant.h"
#include "material/compiler/materialShaderCompileRequest.h"
#include "material/materialDescriptorSchema.h"

// 将已合并的 M_/MI_ 数据转换成材质 Feature、Set 1 Schema、编译请求、
// Pipeline 固定状态与 cache identity；它不创建 GPU 资源。
struct MaterialInstanceBuildPlan
{
    ShaderVariantKey shaderVariantKey;
    // 同一 forwardTransparent RenderPass 可按材质变体选择是否写深度；该合同必须进入缓存身份。
    PassPipelineContractKey surfacePassPipelineContractKey;
    VL::MaterialFeatureKey materialFeatureKey;
    VL::MaterialDescriptorSchema materialDescriptorSchema;
    std::optional<VL::MaterialShaderCompileRequest> baseShaderCompileRequest;
    vk::CullModeFlags cullMode = vk::CullModeFlagBits::eBack;
    GraphicsPipelineBlendMode blendMode = GraphicsPipelineBlendMode::Opaque;
    std::string materialKey;
    std::string materialInstanceKey;
};

class MaterialInstanceValidator
{
public:
    // RenderMode 必须从 M_ 与 MI_ 合并后的 effective JSON 读取，避免资源路由与
    // shader variant 各自解释 override 后得到不同结果。
    static RenderMode ResolveRenderMode(
        const nlohmann::json& effectiveMaterialInstanceJson);

    static PassPipelineContractKey ResolveSurfacePipelineContractKey(
        const PassPipelineContractKey& passPipelineContractKey,
        RenderMode renderMode);

    // 在 MI override 合并完成后推导 Feature，确保 OpaqueClip/TwoSided 使用 effective state。
    // supportsDualSourceBlend 同时决定 Thin Translucent 的 shader 能力宏与固定混合状态。
    static MaterialInstanceBuildPlan BuildLoadPlan(
        std::string_view materialInstancePath,
        const PassPipelineContractKey& passPipelineContractKey,
        const nlohmann::json& materialInstanceJson,
        std::string_view materialPath,
        const nlohmann::json& materialJson,
        bool supportsDualSourceBlend);

    // 用完整 schema 校验参数，并按已选 Base/ShadowDepth reflection 并集校验必需贴图。
    static void Validate(
        std::string_view materialInstancePath,
        const nlohmann::json& materialInstanceJson,
        const VL::MaterialDescriptorSchema& descriptorSchema,
        const std::vector<ShaderBinding>& activeShaderBindings);
};

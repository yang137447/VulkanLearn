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

// 將已合併的 M_/MI_ 資料轉成材質 Feature、Set 1 Schema、編譯請求、
// Pipeline 固定狀態與 cache identity；它不建立 GPU 資源。
struct MaterialInstanceBuildPlan
{
    ShaderVariantKey shaderVariantKey;
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
    // 在 MI override 合併完成後推導 Feature，確保 OpaqueClip/TwoSided 使用 effective state。
    static MaterialInstanceBuildPlan BuildLoadPlan(
        std::string_view materialInstancePath,
        const PassPipelineContractKey& passPipelineContractKey,
        const nlohmann::json& materialInstanceJson,
        std::string_view materialPath,
        const nlohmann::json& materialJson);

    // 用完整 schema 校驗參數，並按已選 Base/ShadowDepth reflection 並集校驗必需貼圖。
    static void Validate(
        std::string_view materialInstancePath,
        const nlohmann::json& materialInstanceJson,
        const VL::MaterialDescriptorSchema& descriptorSchema,
        const std::vector<ShaderBinding>& activeShaderBindings);
};

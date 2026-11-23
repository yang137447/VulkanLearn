#pragma once

// 文件职责：定义一次图形 Shader 变体编译与反射的共享产物，供管线预检和创建复用；
// 不持有 Vulkan ShaderModule，也不决定图形管线固定状态。
// File responsibility: Defines the shared output of compiling and reflecting one graphics shader variant;
// it owns no Vulkan ShaderModule and does not decide fixed-function pipeline state.

#include <string>
#include <vector>

#include "shader/shaderAbiSignature.h"
#include "shader/build/shaderBuildArtifact.h"
#include "shaderReflect.h"

// PipelineFactory 持有的一份规范化图形 Shader 变体产物。
// 管线创建与前置契约校验共用该对象，避免对同一变体重复编译和反射。
// A normalized graphics shader variant artifact owned by PipelineFactory.
// Pipeline creation and preflight validation share it to avoid duplicate compilation and reflection.
struct GraphicsShaderVariantArtifact
{
    std::string logicalBuildId;
    std::string normalizedKey;
    std::string displayName;
    std::string sourceFingerprint;
    std::string artifactGenerationKey;
    std::string vertexSpvPath;
    std::string fragmentSpvPath;
    std::string vertexDebugPath;
    std::string fragmentDebugPath;
    // 地形近场细分：只有同时编译出 runtime tesc/tese 时才置 true。
    // debug 角色是可选的，缺失时对应路径/SPIR-V 保持为空。
    bool hasTessellation = false;
    std::string tessControlSpvPath;
    std::string tessEvaluationSpvPath;
    std::string tessControlDebugPath;
    std::string tessEvaluationDebugPath;
    std::vector<uint32_t> vertexSpirv;
    std::vector<uint32_t> fragmentSpirv;
    std::vector<uint32_t> tessControlSpirv;
    std::vector<uint32_t> tessEvaluationSpirv;
    std::vector<uint32_t> vertexDebugSpirv;
    std::vector<uint32_t> fragmentDebugSpirv;
    std::vector<uint32_t> tessControlDebugSpirv;
    std::vector<uint32_t> tessEvaluationDebugSpirv;
    std::vector<ShaderBinding> shaderBindings;
    VL::ShaderAbiSignature abiSignature;
};

GraphicsShaderVariantArtifact BuildGraphicsShaderVariantArtifact(
    const VL::ShaderBuildArtifact& buildArtifact,
    const std::string& displayName);

#pragma once

// 文件职责：定义一次图形 Shader 变体编译与反射的共享产物，供管线预检和创建复用；
// 不持有 Vulkan ShaderModule，也不决定图形管线固定状态。
// File responsibility: Defines the shared output of compiling and reflecting one graphics shader variant;
// it owns no Vulkan ShaderModule and does not decide fixed-function pipeline state.

#include <string>
#include <vector>

#include "shaderReflect.h"

// PipelineFactory 持有的一份规范化图形 Shader 变体产物。
// 管线创建与前置契约校验共用该对象，避免对同一变体重复编译和反射。
// A normalized graphics shader variant artifact owned by PipelineFactory.
// Pipeline creation and preflight validation share it to avoid duplicate compilation and reflection.
struct GraphicsShaderVariantArtifact
{
    std::string normalizedKey;
    std::string displayName;
    std::string vertexSpvPath;
    std::string fragmentSpvPath;
    std::string vertexDebugPath;
    std::string fragmentDebugPath;
    std::vector<ShaderBinding> shaderBindings;
};

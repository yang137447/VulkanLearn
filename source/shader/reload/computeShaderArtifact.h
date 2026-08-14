#pragma once

// File responsibility: Defines the immutable CPU-side compute artifact shared
// by compute pipeline creation and hot reload. It owns no Vulkan objects.

#include <string>
#include <vector>

#include "shader/build/shaderBuildArtifact.h"
#include "shader/shaderAbiSignature.h"
#include "shaderReflect.h"

struct ComputeShaderArtifact
{
    std::string logicalBuildId;
    std::string normalizedKey;
    std::string shaderName;
    std::string sourceFingerprint;
    std::string artifactGenerationKey;
    std::string runtimeSpvPath;
    std::string debugSpvPath;
    std::vector<uint32_t> runtimeSpirv;
    std::vector<uint32_t> debugSpirv;
    std::vector<ShaderBinding> shaderBindings;
    VL::ShaderAbiSignature abiSignature;
};

ComputeShaderArtifact BuildComputeShaderArtifact(
    const VL::ShaderBuildArtifact& buildArtifact,
    const std::string& shaderName);

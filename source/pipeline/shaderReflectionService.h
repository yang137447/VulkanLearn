#pragma once

#include <string>
#include <vector>
#include "../shaderReflect.h"

struct ShaderReflectionResult
{
    std::vector<ShaderBinding> shaderBindings;
    VL::ShaderAbiSignature abiSignature;
};

class ShaderReflectionService
{
public:
    static std::vector<ShaderBinding> ReflectComputeFromDebugSpirv(const std::string& shaderName);
    static std::vector<ShaderBinding> ReflectFromDebugSpirvFiles(const std::vector<std::string>& shaderPaths);
    static std::vector<ShaderBinding> ReflectFromDebugSpirvCode(const std::vector<std::vector<uint32_t>>& shaderCodes);
    static ShaderReflectionResult ReflectDetailedFromDebugSpirvFiles(
        const std::vector<std::string>& shaderPaths);
    static ShaderReflectionResult ReflectDetailedFromDebugSpirvCode(
        const std::vector<std::vector<uint32_t>>& shaderCodes);
};

#pragma once

#include <string>
#include <vector>
#include "../shaderReflect.h"

class ShaderReflectionService
{
public:
    static std::vector<ShaderBinding> ReflectComputeFromDebugSpirv(const std::string& shaderName);
    static std::vector<ShaderBinding> ReflectFromDebugSpirvFiles(const std::vector<std::string>& shaderPaths);
    static std::vector<ShaderBinding> ReflectFromDebugSpirvCode(const std::vector<std::vector<uint32_t>>& shaderCodes);
};

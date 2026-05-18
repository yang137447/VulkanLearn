#include "shaderReflectionService.h"
#include "../commonFunction.h"

std::vector<ShaderBinding> ShaderReflectionService::ReflectGraphicsFromDebugSpirv(const ShaderVariantKey& shaderVariantKey)
{
    const std::vector<std::string> shaderPaths = {
        CommonFunction::Path(shaderVariantKey.GetStageDebugRelativePath("vert")),
        CommonFunction::Path(shaderVariantKey.GetStageDebugRelativePath("frag"))
    };
    return ReflectFromDebugSpirvFiles(shaderPaths);
}

std::vector<ShaderBinding> ShaderReflectionService::ReflectComputeFromDebugSpirv(const std::string& shaderName)
{
    const std::vector<std::string> shaderPaths = {
        CommonFunction::Path(shaderName + "_comp.debug")
    };
    return ReflectFromDebugSpirvFiles(shaderPaths);
}

std::vector<ShaderBinding> ShaderReflectionService::ReflectFromDebugSpirvFiles(const std::vector<std::string>& shaderPaths)
{
    std::vector<std::vector<uint32_t>> shaderCodes;
    shaderCodes.reserve(shaderPaths.size());
    for (const auto& shaderPath : shaderPaths)
    {
        std::string shaderCode = CommonFunction::ReadFile(shaderPath);
        std::vector<uint32_t> shaderCode32(
            reinterpret_cast<uint32_t*>(shaderCode.data()),
            reinterpret_cast<uint32_t*>(shaderCode.data() + shaderCode.size()));
        shaderCodes.push_back(std::move(shaderCode32));
    }
    return ReflectFromDebugSpirvCode(shaderCodes);
}

std::vector<ShaderBinding> ShaderReflectionService::ReflectFromDebugSpirvCode(const std::vector<std::vector<uint32_t>>& shaderCodes)
{
    ShaderReflect shaderReflect(shaderCodes);
    return shaderReflect.GetShaderBindings();
}

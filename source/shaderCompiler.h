#pragma once

#include <string>
#include <vector>
#include <shaderc/shaderc.hpp>
#include "shaderVariant.h"

class Include: public shaderc::CompileOptions::IncluderInterface
{
public:
    shaderc_include_result* GetInclude(const char* requested_source, shaderc_include_type type, const char* requesting_source, size_t include_depth) override;
    void ReleaseInclude(shaderc_include_result* data) override;
};

class ShaderCompiler
{
public:
    struct ShaderVariantCompileResult
    {
        std::string variantHash;
        std::string normalizedKey;
        std::string vertexSpvPath;
        std::string fragmentSpvPath;
        std::string vertexDebugPath;
        std::string fragmentDebugPath;
    };

    void StartCompile(const std::string& shaderFilePath);
    static ShaderVariantCompileResult EnsureGraphicsVariantCompiled(const ShaderVariantKey& shaderVariantKey);
private:
    //设置shader文件夹的路径
    void SetShaderPath(const std::string& shaderFilePath);
    //glsl->spirv
    std::vector<uint32_t> CompileGLSLToSPIRV(const std::string& glslCode,shaderc_shader_kind kind, const std::string& shaderFileFullPath, const std::vector<std::string>& macros = {}, bool isDebug = false);
    //保存spirv
    void SaveSPIRVToFile(const std::vector<uint32_t>& spirv,const std::string& spvPath);
    ShaderVariantCompileResult CompileGraphicsVariant(const ShaderVariantKey& shaderVariantKey);
    static std::vector<std::string> BuildRenderModeMacros(RenderMode renderMode);
    static void UpdateVariantManifest(const ShaderVariantKey& shaderVariantKey);
    std::string shaderPath;
    std::string glslPath;
    std::string spirvPath;
};

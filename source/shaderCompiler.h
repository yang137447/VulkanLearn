#pragma once

#include <string>
#include <vector>
#include <shaderc/shaderc.hpp>
#include "shaderVariant.h"

namespace VL
{
struct ComposedMaterialShaderSource;
struct MaterialShaderCompileRequest;
}

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
    // 編譯 Composer 已裝配的完整 GLSL；不在此判斷材質 Feature 或 Shadow 路由。
    static ShaderVariantCompileResult EnsureMaterialGraphicsVariantCompiled(
        const VL::MaterialShaderCompileRequest& request,
        const VL::ComposedMaterialShaderSource& source);
private:
    //设置shader文件夹的路径
    void SetShaderPath(const std::string& shaderFilePath);
    //glsl->spirv
    std::vector<uint32_t> CompileGLSLToSPIRV(const std::string& glslCode,shaderc_shader_kind kind, const std::string& shaderFileFullPath, const std::vector<std::string>& macros = {}, bool isDebug = false);
    //保存spirv
    void SaveSPIRVToFile(const std::vector<uint32_t>& spirv,const std::string& spvPath);
    ShaderVariantCompileResult CompileGraphicsVariant(const ShaderVariantKey& shaderVariantKey);
    ShaderVariantCompileResult CompileMaterialGraphicsVariant(
        const VL::MaterialShaderCompileRequest& request,
        const VL::ComposedMaterialShaderSource& source);
    void CompileGraphicsSourcePair(
        const std::string& vertexShaderCode,
        const std::string& fragmentShaderCode,
        const std::string& vertexSourcePath,
        const std::string& fragmentSourcePath,
        const std::vector<std::string>& compileMacros,
        const ShaderVariantCompileResult& compileResult);
    static std::vector<std::string> BuildRenderModeMacros(RenderMode renderMode);
    static std::vector<std::string> BuildGraphicsVariantMacros(
        const ShaderVariantKey& shaderVariantKey);
    static void UpdateVariantManifest(const ShaderVariantKey& shaderVariantKey);
    static void UpdateMaterialVariantManifest(const VL::MaterialShaderCompileRequest& request);
    std::string shaderPath;
    std::string glslPath;
    std::string spirvPath;
};

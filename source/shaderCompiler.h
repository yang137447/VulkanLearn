#pragma once

#include <shaderc/shaderc.hpp>

class Include: public shaderc::CompileOptions::IncluderInterface
{
public:
    shaderc_include_result* GetInclude(const char* requested_source, shaderc_include_type type, const char* requesting_source, size_t include_depth) override;
    void ReleaseInclude(shaderc_include_result* data) override;
private:
    shaderc_include_result* includeResult;
};

class ShaderCompiler
{
public:
    ShaderCompiler();
    ~ShaderCompiler();
    void StartCompile(const std::string& shaderFilePath);
private:
    //设置shader文件夹的路径
    void SetShaderPath(const std::string& shaderFilePath);
    //glsl->spirv
    std::vector<uint32_t> CompileGLSLToSPIRV(const std::string& glslCode,shaderc_shader_kind kind, const std::string& shaderFileFullPath, bool isDebug = false);
    //保存spirv
    void SaveSPIRVToFile(const std::vector<uint32_t>& spirv,const std::string& spvPath);
    std::string shaderPath;
    std::string glslPath;
    std::string spirvPath;
};
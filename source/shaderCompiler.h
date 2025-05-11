#pragma once

#include <shaderc/shaderc.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

class ShaderCompiler
{
public:
    ShaderCompiler();
    ~ShaderCompiler();
    void StartCompile(const std::string& shaderFilePath);
private:
    //设置shader文件夹的路径
    void SetShaderPath(const std::string& shaderFilePath);
    //读取文件内容
    std::string ReadFile(const std::string& glslPath);
    //glsl->spirv
    std::vector<uint32_t> CompileGLSLToSPIRV(const std::string& glslCode,shaderc_shader_kind kind, const std::string& shaderName);
    //保存spirv
    void SaveSPIRVToFile(const std::vector<uint32_t>& spirv,const std::string& spvPath);
    std::string shaderPath;
    std::string glslPath;
    std::string spirvPath;
};
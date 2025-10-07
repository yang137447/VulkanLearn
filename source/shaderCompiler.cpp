#include "shaderCompiler.h"
#include "settings.h"

#include <shaderc/shaderc.h>
#include <shaderc/shaderc.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

ShaderCompiler::ShaderCompiler()
{
}

ShaderCompiler::~ShaderCompiler()
{
}

void ShaderCompiler::StartCompile(const std::string& shaderFilePath)
{
    std::cout << "Info: "
              << "start compiled shader: "
              << std::endl;
    SetShaderPath(shaderFilePath);
    std::filesystem::recursive_directory_iterator glslShaders(glslPath);
    for (const auto &shader : glslShaders)
    {
        if (shader.is_regular_file())
        {
            shaderc_shader_kind kind;
            //获取文件名
            std::string shaderName = shader.path().stem().string();
            //获取文件后缀
            std::string shaderExtension = shader.path().extension().string().substr(1);
            //获取编译前文件路径
            std::string glslShaderPath = glslPath + "/" + shaderName + "." + shaderExtension;
            //获取编译后文件路径
            std::string compiledShaderPath = spirvPath + "/" + shaderName + "_" + shaderExtension + "." + "spv";
            if (glslShaderPath.find("vert") != std::string::npos)
            {
                kind = shaderc_shader_kind::shaderc_vertex_shader;
            }
            else if (glslShaderPath.find("frag") != std::string::npos)
            {
                kind = shaderc_shader_kind::shaderc_fragment_shader;
            }
            else
            {
                std::cout << "Unknown shader type: " << glslShaderPath << std::endl;
                continue;
            }

            std::string glslCode = ReadFile(glslShaderPath);
            std::vector<uint32_t> spvCode = CompileGLSLToSPIRV(glslCode, kind, shaderName);
            SaveSPIRVToFile(spvCode, compiledShaderPath);

            std::cout << "Info: "
                      << "compiled shader: "
                      << compiledShaderPath
                      << std::endl;
        }
    }
}

void ShaderCompiler::SetShaderPath(const std::string& shaderFilePath)
{
    shaderPath = shaderFilePath;
    glslPath = shaderPath + "/glsl";
    spirvPath = shaderPath + "/spv";
}

std::string ShaderCompiler::ReadFile(const std::string& glslPath)
{
    std::ifstream file(glslPath.data(), std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + std::string(glslPath));
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string buffer;
    buffer.resize(size);
    if (!file.read(buffer.data(), size))
    {
        throw std::runtime_error("Failed to read file: " + std::string(glslPath));
    }
    file.close();
    if (size <= 0) {
        throw std::runtime_error("File is empty or unreadable: " + glslPath);
    }  

    return buffer;
}

std::vector<uint32_t> ShaderCompiler::CompileGLSLToSPIRV(const std::string& glslCode, shaderc_shader_kind kind, const std::string& shaderName)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level::shaderc_optimization_level_performance);  //设置编译选项
    options.SetTargetEnvironment(shaderc_target_env::shaderc_target_env_vulkan, shaderc_env_version::shaderc_env_version_vulkan_1_4);
    options.SetSourceLanguage(shaderc_source_language::shaderc_source_language_glsl);

    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(glslCode.data(), kind, shaderName.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        throw std::runtime_error(result.GetErrorMessage());
    }
    
    return {result.cbegin(), result.cend()};
}

void ShaderCompiler::SaveSPIRVToFile(const std::vector<uint32_t>& spirv, const std::string& spvPath)
{
    std::ofstream file(spvPath, std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + std::string(spvPath));
    }
    std::cout << spvPath << spirv.size() * sizeof(uint32_t) << std::endl;
    file.write(reinterpret_cast<const char *>(spirv.data()), spirv.size() * sizeof(uint32_t));
    file.close();
}

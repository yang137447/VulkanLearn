#include "shaderCompiler.h"
#include "settings.h"

#include <shaderc/shaderc.h>
#include <shaderc/shaderc.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "shaderReflect.h"
#include "commonFunction.h"

shaderc_include_result* Include::GetInclude(const char* requested_source, shaderc_include_type type, const char* requesting_source, size_t include_depth)
{
    // 创建一个新的结果对象
    shaderc_include_result* result = new shaderc_include_result;
    
    // 构建文件路径
    std::string fileFolder = std::filesystem::path(requesting_source).parent_path().string();
    std::string filePath = fileFolder + "/" + requested_source;

    std::cout << "Including file: " << filePath << std::endl;
    
    // 读取文件内容
    std::string glslCode = CommonFunction::ReadFile(filePath);
    
    // 为内容分配新的内存，因为 glslCode 会在函数结束时被销毁
    char* content = new char[glslCode.size()];
    memcpy(content, glslCode.data(), glslCode.size());
    
    // 为文件名分配新的内存
    char* filename = new char[filePath.size()];
    memcpy(filename, filePath.data(), filePath.size());
    
    // 设置结果
    result->source_name = filename;
    result->source_name_length = filePath.size();
    result->content = content;
    result->content_length = glslCode.size();
    result->user_data = nullptr;
    
    return result;
}

void Include::ReleaseInclude(shaderc_include_result* data)
{
    delete includeResult;
}

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

            std::string glslCode = CommonFunction::ReadFile(glslShaderPath);
            std::vector<uint32_t> spvCode = CompileGLSLToSPIRV(glslCode, kind, glslShaderPath);
            SaveSPIRVToFile(spvCode, compiledShaderPath);

            ShaderReflect shaderReflect(spvCode);

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

std::vector<uint32_t> ShaderCompiler::CompileGLSLToSPIRV(const std::string& glslCode, shaderc_shader_kind kind, const std::string& shaderFileFullPath)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level::shaderc_optimization_level_performance);  //设置编译选项
    options.SetTargetEnvironment(shaderc_target_env::shaderc_target_env_vulkan, shaderc_env_version::shaderc_env_version_vulkan_1_4);
    options.SetSourceLanguage(shaderc_source_language::shaderc_source_language_glsl);
    options.SetIncluder(std::make_unique<Include>());

    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(glslCode.data(), kind, shaderFileFullPath.c_str(), options);
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

#include "shaderCompiler.h"

#include <shaderc/shaderc.h>
#include <shaderc/shaderc.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "shaderReflect.h"
#include "commonFunction.h"

shaderc_include_result* Include::GetInclude(const char* requested_source, shaderc_include_type type, const char* requesting_source, size_t include_depth)
{
    // 创建一个新的结果对象
    shaderc_include_result* result = new shaderc_include_result;
    
    // 构建文件路径
    std::string fileFolder = std::filesystem::path(requesting_source).parent_path().string();
    std::string filePath = fileFolder + "/" + requested_source;

    std::cout << std::string(2 * include_depth, ' ') << "->Including file: " << filePath << std::endl;
    
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
    if (data == nullptr)
    {
        return;
    }
    delete[] data->content;
    delete[] data->source_name;
    delete data;
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
            std::filesystem::path shaderFullPath = shader.path();
            std::filesystem::path relativeShaderPath = std::filesystem::relative(shaderFullPath, glslPath);
            std::filesystem::path shaderNamePath = relativeShaderPath;
            shaderNamePath.replace_extension();
            std::string shaderName = shaderNamePath.generic_string();
            //获取文件后缀
            std::string shaderExtension = shader.path().extension().string().substr(1);
            //获取编译前文件路径
            std::string glslShaderPath = shaderFullPath.string();
            //获取编译后文件路径
            std::string compiledShaderPath = spirvPath + "/" + shaderName + "_" + shaderExtension + "." + "spv";
            std::string compiledDebugShaderPath = spirvPath + "/" + shaderName + "_" + shaderExtension + "." + "debug";
            if (shaderExtension == "vert")
            {
                kind = shaderc_shader_kind::shaderc_vertex_shader;
            }
            else if (shaderExtension == "frag")
            {
                kind = shaderc_shader_kind::shaderc_fragment_shader;
            }
            else if (shaderExtension == "comp")
            {
                kind = shaderc_shader_kind::shaderc_compute_shader;
            }
            else
            {
                std::cout << "Unknown shader type: " << glslShaderPath << std::endl;
                continue;
            }

            std::string glslCode = CommonFunction::ReadFile(glslShaderPath);
            ShaderVariantKey defaultVariantKey;
            defaultVariantKey.shaderName = shaderName;
            UpdateVariantManifest(defaultVariantKey);
            
            bool isDebugInfo = false;
            std::vector<std::string> macros;
#ifndef NDEBUG
            isDebugInfo = true;
            macros.push_back("ENABLE_DEBUG_VIEW");
#endif
            // Main shader
            std::vector<uint32_t> spvCode = CompileGLSLToSPIRV(glslCode, kind, glslShaderPath, macros, isDebugInfo);
            SaveSPIRVToFile(spvCode, compiledShaderPath);

            // Debug shader (for reflection)
            if (isDebugInfo)
            {
                SaveSPIRVToFile(spvCode, compiledDebugShaderPath);
            }
            else
            {
                // If main shader is optimized, compile a separate debug version for reflection
                std::vector<std::string> debugMacros = macros;
                debugMacros.push_back("ENABLE_DEBUG_VIEW"); // Ensure debug reflection also has it if needed, or maybe just debug version
                std::vector<uint32_t> spvDebugCode = CompileGLSLToSPIRV(glslCode, kind, glslShaderPath, debugMacros, true);
                SaveSPIRVToFile(spvDebugCode, compiledDebugShaderPath);
            }

            //ShaderReflect shaderReflect(spvCode);

            std::cout << "Info: "
                      << "compiled shader: "
                      << compiledShaderPath
                      << std::endl;
        }
    }
}

ShaderCompiler::ShaderVariantCompileResult ShaderCompiler::EnsureGraphicsVariantCompiled(const ShaderVariantKey& shaderVariantKey)
{
    ShaderCompiler shaderCompiler;
    shaderCompiler.SetShaderPath(CommonFunction::GetProjectPath() + "/shader");
    return shaderCompiler.CompileGraphicsVariant(shaderVariantKey);
}

void ShaderCompiler::SetShaderPath(const std::string& shaderFilePath)
{
    shaderPath = shaderFilePath;
    glslPath = shaderPath + "/glsl";
    spirvPath = shaderPath + "/spv";
}

std::vector<uint32_t> ShaderCompiler::CompileGLSLToSPIRV(const std::string& glslCode, shaderc_shader_kind kind, const std::string& shaderFileFullPath, const std::vector<std::string>& macros, bool isDebug)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level::shaderc_optimization_level_performance);
    if (isDebug)
    {
        options.SetGenerateDebugInfo();
    }
    options.SetTargetEnvironment(shaderc_target_env::shaderc_target_env_vulkan, shaderc_env_version::shaderc_env_version_vulkan_1_4);
    options.SetSourceLanguage(shaderc_source_language::shaderc_source_language_glsl);
    options.SetIncluder(std::make_unique<Include>());
    for (const std::string& macro : macros)
    {
        const size_t valueSeparator = macro.find('=');
        if (valueSeparator == std::string::npos)
        {
            options.AddMacroDefinition(macro, "1");
        }
        else
        {
            options.AddMacroDefinition(macro.substr(0, valueSeparator), macro.substr(valueSeparator + 1));
        }
    }

    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(glslCode.data(), kind, shaderFileFullPath.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        throw std::runtime_error(result.GetErrorMessage());
    }
    
    return {result.cbegin(), result.cend()};
}

void ShaderCompiler::SaveSPIRVToFile(const std::vector<uint32_t>& spirv, const std::string& spvPath)
{
    std::filesystem::path outPath(spvPath);
    if (outPath.has_parent_path())
    {
        std::filesystem::create_directories(outPath.parent_path());
    }

    std::ofstream file(spvPath, std::ios::binary);
    if (!file.is_open())
    {
        throw std::runtime_error("Failed to open file: " + std::string(spvPath));
    }
    std::cout << spvPath << spirv.size() * sizeof(uint32_t) << std::endl;
    file.write(reinterpret_cast<const char *>(spirv.data()), spirv.size() * sizeof(uint32_t));
    file.close();
}

std::vector<std::string> ShaderCompiler::BuildRenderModeMacros(RenderMode renderMode)
{
    switch (renderMode)
    {
    case RenderMode::Opaque:
        return {"RENDER_MODE_OPAQUE"};
    case RenderMode::OpaqueClip:
        return {"RENDER_MODE_OPAQUE_CLIP"};
    case RenderMode::TransparentAlphaBlend:
        return {"RENDER_MODE_TRANSPARENT_ALPHA_BLEND"};
    case RenderMode::TransparentAdditive:
        return {"RENDER_MODE_TRANSPARENT_ADDITIVE"};
    default:
        throw std::runtime_error("Unknown RenderMode when building shader macros");
    }
}

void ShaderCompiler::UpdateVariantManifest(const ShaderVariantKey& shaderVariantKey)
{
    const std::string manifestPath = CommonFunction::GetProjectPath() + "/shader/spv/variants.json";
    nlohmann::json manifestJson = nlohmann::json::object();

    if (std::filesystem::exists(manifestPath))
    {
        std::ifstream manifestFile(manifestPath);
        if (manifestFile.is_open() && manifestFile.peek() != std::ifstream::traits_type::eof())
        {
            manifestFile >> manifestJson;
        }
    }

    manifestJson[shaderVariantKey.GetVariantHash()] = {
        {"normalizedKey", shaderVariantKey.GetNormalizedKey()},
        {"shaderName", shaderVariantKey.shaderName},
        {"renderMode", RenderModeToString(shaderVariantKey.renderMode)},
        {"shadingModel", shaderVariantKey.shadingModelMacro},
        {"macros", shaderVariantKey.macros}
    };

    std::filesystem::create_directories(std::filesystem::path(manifestPath).parent_path());
    std::ofstream manifestOutput(manifestPath);
    manifestOutput << manifestJson.dump(4);
}

ShaderCompiler::ShaderVariantCompileResult ShaderCompiler::CompileGraphicsVariant(const ShaderVariantKey& shaderVariantKeyInput)
{
    ShaderVariantKey shaderVariantKey = shaderVariantKeyInput;
    shaderVariantKey.macros = NormalizeMaterialMacros(shaderVariantKey.macros);

    ShaderVariantCompileResult compileResult;
    compileResult.variantHash = shaderVariantKey.GetVariantHash();
    compileResult.normalizedKey = shaderVariantKey.GetNormalizedKey();
    compileResult.vertexSpvPath = spirvPath + "/" + shaderVariantKey.GetStageSpvRelativePath("vert");
    compileResult.fragmentSpvPath = spirvPath + "/" + shaderVariantKey.GetStageSpvRelativePath("frag");
    compileResult.vertexDebugPath = spirvPath + "/" + shaderVariantKey.GetStageDebugRelativePath("vert");
    compileResult.fragmentDebugPath = spirvPath + "/" + shaderVariantKey.GetStageDebugRelativePath("frag");

    const std::string vertexShaderSourcePath = glslPath + "/" + shaderVariantKey.shaderName + ".vert";
    const std::string fragmentShaderSourcePath = glslPath + "/" + shaderVariantKey.shaderName + ".frag";

    std::cout << "Info: compiling shader variant "
              << shaderVariantKey.GetShortDebugString()
              << " hash="
              << compileResult.variantHash
              << std::endl;

    const std::string vertexShaderCode = CommonFunction::ReadFile(vertexShaderSourcePath);
    const std::string fragmentShaderCode = CommonFunction::ReadFile(fragmentShaderSourcePath);

    std::vector<std::string> compileMacros = BuildRenderModeMacros(shaderVariantKey.renderMode);
    compileMacros.push_back("MATERIAL_SHADING_MODEL=" + shaderVariantKey.shadingModelMacro);
    compileMacros.insert(compileMacros.end(), shaderVariantKey.macros.begin(), shaderVariantKey.macros.end());

    bool isDebugInfo = false;
#ifndef NDEBUG
    isDebugInfo = true;
    compileMacros.push_back("ENABLE_DEBUG_VIEW");
#endif

    const std::vector<uint32_t> vertexSpv = CompileGLSLToSPIRV(vertexShaderCode, shaderc_shader_kind::shaderc_vertex_shader, vertexShaderSourcePath, compileMacros, isDebugInfo);
    const std::vector<uint32_t> fragmentSpv = CompileGLSLToSPIRV(fragmentShaderCode, shaderc_shader_kind::shaderc_fragment_shader, fragmentShaderSourcePath, compileMacros, isDebugInfo);
    SaveSPIRVToFile(vertexSpv, compileResult.vertexSpvPath);
    SaveSPIRVToFile(fragmentSpv, compileResult.fragmentSpvPath);

    if (isDebugInfo)
    {
        SaveSPIRVToFile(vertexSpv, compileResult.vertexDebugPath);
        SaveSPIRVToFile(fragmentSpv, compileResult.fragmentDebugPath);
    }
    else
    {
        const std::vector<uint32_t> vertexSpvDebug = CompileGLSLToSPIRV(vertexShaderCode, shaderc_shader_kind::shaderc_vertex_shader, vertexShaderSourcePath, compileMacros, true);
        const std::vector<uint32_t> fragmentSpvDebug = CompileGLSLToSPIRV(fragmentShaderCode, shaderc_shader_kind::shaderc_fragment_shader, fragmentShaderSourcePath, compileMacros, true);
        SaveSPIRVToFile(vertexSpvDebug, compileResult.vertexDebugPath);
        SaveSPIRVToFile(fragmentSpvDebug, compileResult.fragmentDebugPath);
    }

    UpdateVariantManifest(shaderVariantKey);
    return compileResult;
}

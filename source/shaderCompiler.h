#include <shaderc/shaderc.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

class ShaderCompiler
{
public:
    ShaderCompiler();
    ~ShaderCompiler();
    void StartCompile(std::string_view shaderFilePath);
private:
    //设置shader文件夹的路径
    void SetShaderPath(std::string_view shaderFilePath);
    //读取文件内容
    std::string ReadFile(std::string_view filepath);
    //glsl->spirv
    std::vector<uint32_t> CompileGLSLToSPIRV(std::string_view source,shaderc_shader_kind kind);
    //保存spirv
    void SaveSPIRVToFile(const std::vector<uint32_t>& spirv,std::string_view filePath);
    std::string shaderPath;
    std::string glslPath;
    std::string spirvPath;
};
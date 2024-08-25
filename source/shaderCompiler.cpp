#include "shaderCompiler.h"

#include <shaderc/shaderc.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

// 读取文件内容
std::string ReadFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filepath);
    }

    size_t fileSize = (size_t)file.tellg();
    std::string buffer(fileSize, ' ');
    file.seekg(0);
    file.read(&buffer[0], fileSize);
    file.close();

    return buffer;
}

// 编译GLSL到SPIR-V
std::vector<uint32_t> CompileGLSLToSPIRV(const std::string& source, shaderc_shader_kind kind) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(source, kind, "shader.glsl", options);

    if (module.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error("Shader compilation failed: " + std::string(module.GetErrorMessage()));
    }

    return {module.cbegin(), module.cend()};
}

// 保存SPIR-V到文件
void SaveSPIRVToFile(const std::vector<uint32_t>& spirv, const std::string& filepath) {
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filepath);
    }

    file.write(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(uint32_t));
    file.close();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path/to/your/shader/folder>" << std::endl;
        return EXIT_FAILURE;
    }

    std::string folderPath = argv[1];

    try {
        for (const auto& entry : std::filesystem::directory_iterator(folderPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".glsl") {
                std::string inputFilePath = entry.path().string();
                std::string outputFilePath = entry.path().replace_extension(".spv").string();

                shaderc_shader_kind kind;
                if (inputFilePath.find("_vert") != std::string::npos) {
                    kind = shaderc_glsl_vertex_shader;
                } else if (inputFilePath.find("_frag") != std::string::npos) {
                    kind = shaderc_glsl_fragment_shader;
                } else {
                    std::cerr << "Unknown shader type for file: " << inputFilePath << std::endl;
                    continue;
                }

                std::string source = ReadFile(inputFilePath);
                auto spirv = CompileGLSLToSPIRV(source, kind);
                SaveSPIRVToFile(spirv, outputFilePath);
                std::cout << "Compiled and saved: " << outputFilePath << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

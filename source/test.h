#include "shaderc/shaderc.hpp"

    // GLSL 着色器代码
    const std::string shader_code = R"(
        #version 450
        void main() {
            gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        }
    )";

    // 创建编译器实例
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    // 编译 GLSL 到 SPIR-V
    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        shader_code, shaderc_glsl_vertex_shader, "shader.vert", options);

    // 检查编译结果
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        std::cerr << "编译失败: " << result.GetErrorMessage() << std::endl;
        return 1;
    }

    std::cout << "编译成功!" << std::endl;
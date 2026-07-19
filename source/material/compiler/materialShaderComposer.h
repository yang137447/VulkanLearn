#pragma once

// 文件职责：声明材质求值模块与引擎 Pass 模板的装配接口，输出可直接编译的完整 GLSL；
// 不负责选择 Pass、编译 SPIR-V 或创建图形管线。
// File responsibility: Declares the assembly interface that produces complete GLSL from material
// evaluation modules and engine pass templates; it does not select passes, compile SPIR-V, or create pipelines.

#include <string>

#include "material/compiler/materialShaderCompileRequest.h"

namespace VL
{

// 一次装配产生的顶点、片元源码及其诊断用虚拟路径。
// Vertex and fragment sources produced by one composition, with virtual paths used for diagnostics.
struct ComposedMaterialShaderSource
{
    std::string vertexSource;
    std::string fragmentSource;
    std::string vertexVirtualPath;
    std::string fragmentVirtualPath;
};

// 根据编译请求，将材质求值模块装配到引擎持有的 Pass 模板中。
// 输入由材质验证阶段生成；结果交给 PipelineFactory 编译，不解析 GLSL 也不决定 Pass。
// Assembles material evaluation modules into engine-owned pass templates from a validated request.
// PipelineFactory compiles the result; this class neither parses GLSL nor chooses the pass.
class MaterialShaderComposer
{
public:
    static ComposedMaterialShaderSource Compose(const MaterialShaderCompileRequest& request);
};

} // namespace VL

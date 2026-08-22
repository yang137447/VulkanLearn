#ifndef VL_MATERIAL_FUNCTION_DEFAULT_VERTEX_GLSL
#define VL_MATERIAL_FUNCTION_DEFAULT_VERTEX_GLSL

#include "../engine/materialContext.glsl"

// 默认顶点 MF 保留 VertexFactory 提供的规范化几何输入，不执行材质顶点变形。
MaterialVertex EvaluateMFDefaultVertex(in MaterialVertexInput vertexInput)
{
    return CreateDefaultMaterialVertex(vertexInput);
}

#endif

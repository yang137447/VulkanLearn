#ifndef VL_MATERIAL_FUNCTION_SHADER_RELOAD_TEST_VERTEX_GLSL
#define VL_MATERIAL_FUNCTION_SHADER_RELOAD_TEST_VERTEX_GLSL

#include "../engine/materialContext.glsl"

MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return CreateDefaultMaterialVertex(vertexInput);
}

#endif

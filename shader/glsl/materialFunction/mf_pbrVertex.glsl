#ifndef VL_MATERIAL_FUNCTION_PBR_VERTEX_GLSL
#define VL_MATERIAL_FUNCTION_PBR_VERTEX_GLSL

#include "../engine/materialContext.glsl"

// PBR 不修改網格位置；仍提供統一入口，讓所有 Mesh Pass 使用同一裝配流程。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    return CreateDefaultMaterialVertex(vertexInput);
}

#endif

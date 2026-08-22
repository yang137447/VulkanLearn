#ifndef VL_MATERIAL_FUNCTION_SPEEDTREE_VERTEX_GLSL
#define VL_MATERIAL_FUNCTION_SPEEDTREE_VERTEX_GLSL

#include "../engine/materialContext.glsl"
#include "../common/commonUbo.glsl"
#include "mf_speedtreeDeformation.glsl"

// SpeedTree 变形是可复用顶点函数；公开 EvaluateMaterialVertex 位于 M_speedtree.vertex.glsl。
MaterialVertex EvaluateMFSpeedTreeVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex = CreateDefaultMaterialVertex(vertexInput);
    vertex.localPosition = EvaluateSpeedTreeDeformedPosition(
        vertexInput.localPosition,
        vertexInput.localNormal,
        vertexInput.vertexColor,
        vertexInput.texCoord,
        vertexInput.localTangent,
        vertexInput.speedTreeWindBranch1,
        vertexInput.speedTreeWindBranch2,
        vertex.localNormal);
    return vertex;
}

#endif

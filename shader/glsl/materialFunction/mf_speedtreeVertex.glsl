#ifndef VL_MATERIAL_FUNCTION_SPEEDTREE_VERTEX_GLSL
#define VL_MATERIAL_FUNCTION_SPEEDTREE_VERTEX_GLSL

#include "../engine/materialContext.glsl"
#include "../common/commonUbo.glsl"
#include "mf_speedtreeDeformation.glsl"

// Wind/WPO 只在這個材質入口修改局部頂點，Base 與 ShadowDepth 不再各自複製變形。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
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

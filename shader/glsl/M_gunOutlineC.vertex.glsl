#ifndef VL_M_GUNOUTLINE_C_VERTEX_GLSL
#define VL_M_GUNOUTLINE_C_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"
#include "common/commonUbo.glsl"

// C: 经典 cull-Front 倒置壳 + 屏幕空间等宽外扩。
// width = fraction * 2 * tanHalfFovY * d，fraction 为半宽/视口高度（分辨率无关），
// P[1][1] = 1/tanHalfFovY 直接来自投影矩阵；不需要深度偏置，也不需要 facing 判定。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex = CreateDefaultMaterialVertex(vertexInput);
    vec3 worldPosition = (uboM.model * vec4(vertexInput.localPosition, 1.0)).xyz;
    float distanceToCamera = length(worldPosition - uboVP.cameraPosition);
    float tanHalfFovY = 1.0 / max(abs(uboVP.projection[1][1]), 1.0e-6);
    float width = u_outlineScreenFraction * 2.0 * tanHalfFovY * distanceToCamera * u_widthScale;
    vertex.localPosition += normalize(vertexInput.localNormal) * width;
    return vertex;
}

#endif

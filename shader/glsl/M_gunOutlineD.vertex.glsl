#ifndef VL_M_GUNOUTLINE_D_VERTEX_GLSL
#define VL_M_GUNOUTLINE_D_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"
#include "common/commonUbo.glsl"

// D: 屏幕空间等宽 + cull None + facing（只画正面）+ 自适应深度偏置。
// 正面外扩一定贴着剪影，不会像 cull-Front 那样在厚实部位向内缩、导致边断裂；
// 偏置按壳宽自适应（2*width + 0.5cm），因此远处也不会盖到本体。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex = CreateDefaultMaterialVertex(vertexInput);
    vec3 worldPosition = (uboM.model * vec4(vertexInput.localPosition, 1.0)).xyz;
    vec3 viewVector = worldPosition - uboVP.cameraPosition;
    float distanceToCamera = length(viewVector);
    float tanHalfFovY = 1.0 / max(abs(uboVP.projection[1][1]), 1.0e-6);
    float width = u_outlineScreenFraction * 2.0 * tanHalfFovY * distanceToCamera * u_widthScale;
    vec3 localNormal = normalize(vertexInput.localNormal);
    vec3 localViewDirection = normalize(transpose(mat3(uboM.model)) * normalize(viewVector));
    vertex.localPosition += localNormal * width;
    vertex.localPosition += localViewDirection * (2.0 * width + 0.5);
    return vertex;
}

#endif

#ifndef VL_M_GUNOUTLINEB_VERTEX_GLSL
#define VL_M_GUNOUTLINEB_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"
#include "common/commonUbo.glsl"

// B: classic cull-Front inverted hull, world-space width curve, no bias
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex = CreateDefaultMaterialVertex(vertexInput);
    vec3 worldPosition = (uboM.model * vec4(vertexInput.localPosition, 1.0)).xyz;
    vec3 viewVector = worldPosition - uboVP.cameraPosition;
    float distanceToCamera = length(viewVector);
    float fadeT = clamp((u_fadeDistance - distanceToCamera) / u_fadeDistance, 0.0, 1.0);
    float width = mix(u_widthFar, u_widthNear, fadeT) * u_widthScale;
    vertex.localPosition += normalize(vertexInput.localNormal) * width;
    // B 的 MI 把 u_viewBias 固定为 0；平滑版 C 用它把壳整体推远，
    // 避免平滑后的背面在本体凹处盖到本体像素。
    vec3 localViewDirection = normalize(transpose(mat3(uboM.model)) * normalize(viewVector));
    vertex.localPosition += localViewDirection * u_viewBias;
    return vertex;
}

#endif

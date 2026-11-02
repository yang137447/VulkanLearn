#ifndef VL_M_GUNOUTLINEA_VERTEX_GLSL
#define VL_M_GUNOUTLINEA_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"
#include "common/commonUbo.glsl"

// A: capture-exact inverted hull (cull None + facing + fixed 3cm view bias, world-space width curve)
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex = CreateDefaultMaterialVertex(vertexInput);
    vec3 worldPosition = (uboM.model * vec4(vertexInput.localPosition, 1.0)).xyz;
    vec3 viewVector = worldPosition - uboVP.cameraPosition;
    float distanceToCamera = length(viewVector);
    float fadeT = clamp((u_fadeDistance - distanceToCamera) / u_fadeDistance, 0.0, 1.0);
    float width = mix(u_widthFar, u_widthNear, fadeT) * u_widthScale;
    vertex.localPosition += normalize(vertexInput.localNormal) * width;
    vec3 localViewDirection = normalize(transpose(mat3(uboM.model)) * normalize(viewVector));
    vertex.localPosition += localViewDirection * u_viewBias;
    return vertex;
}

#endif

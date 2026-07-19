#ifndef VL_MATERIAL_FUNCTION_SPEEDTREE_DEFORMATION_GLSL
#define VL_MATERIAL_FUNCTION_SPEEDTREE_DEFORMATION_GLSL

// Surface and ShadowCaster must evaluate the same local-space deformation.
// Wind/WPO support extends this function without changing either pass wrapper.
vec3 EvaluateSpeedTreeDeformedPosition(
    vec3 localPosition,
    vec3 localNormal,
    vec4 vertexColor,
    vec2 texCoord,
    vec4 localTangent)
{
    return localPosition;
}

#endif

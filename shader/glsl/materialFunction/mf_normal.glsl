#ifndef VL_MATERIAL_FUNCTION_NORMAL_GLSL
#define VL_MATERIAL_FUNCTION_NORMAL_GLSL

#include "../engine/materialContext.glsl"

// 普通 MF 只负责材质法线语义；它不读取灯光，也不决定 GBuffer 编码。
vec3 TransformMaterialNormalToWorld(
    in MaterialFunctionContext context,
    in vec3 normalTS)
{
    vec3 bitangent =
        cross(context.worldNormal, context.worldTangent.xyz) *
        context.worldTangent.w;
    return normalize(
        normalTS.x * context.worldTangent.xyz +
        normalTS.y * bitangent +
        normalTS.z * context.worldNormal);
}

#endif

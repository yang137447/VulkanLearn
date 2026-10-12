#ifndef VL_MATERIAL_FUNCTION_NORMAL_GLSL
#define VL_MATERIAL_FUNCTION_NORMAL_GLSL

#include "../engine/materialContext.glsl"

// 普通 MF 只负责材质法线语义；它不读取灯光，也不决定 GBuffer 编码。
vec3 TransformMaterialNormalToWorld(
    in MaterialFunctionContext context,
    in vec3 normalTS)
{
    // MikkTSpace 解码保留未归一化的插值基，只归一化最终法线；不能在此
    // 独立归一化或正交化 N/T。模型缩放的长度因子在顶点变换阶段去除。
    // 延续当前烘焙约定：片元阶段用 tangent.w 重建 B，不翻转贴图 G 通道。
    vec3 bitangent =
        cross(context.worldNormal, context.worldTangent.xyz) *
        context.worldTangent.w;
    return normalize(
        normalTS.x * context.worldTangent.xyz +
        normalTS.y * bitangent +
        normalTS.z * context.worldNormal);
}

vec4 OrthonormalizeMaterialTangent(
    in vec3 worldNormal,
    in vec4 worldTangent)
{
    // Normal Map 改变 N 后必须重新把 T 投影到新切平面；B 仍由 handedness
    // 在 lighting 端恢复，镜像 UV 因而不会丢失左右手系语义。
    vec3 tangent = normalize(
        worldTangent.xyz -
        worldNormal * dot(worldNormal, worldTangent.xyz));
    return vec4(tangent, worldTangent.w);
}

#endif

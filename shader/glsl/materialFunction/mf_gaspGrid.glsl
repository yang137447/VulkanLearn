#ifndef VL_MF_GASP_GRID_GLSL
#define VL_MF_GASP_GRID_GLSL

#include "../engine/materialInputs.glsl"
#include "mf_normal.glsl"

struct GaspProjection
{
    vec3 position;
    vec3 normal;
};

GaspProjection GaspTextureProjection(in MaterialFunctionContext context)
{
    GaspProjection result;
#if OBJECT_ALIGNED
    // 对齐源 MF_ObjectAlignedTexture：用归一化物体轴消除非均匀缩放，
    // 从 primitive bounds 中心量厘米距离；不是把 UV 随盒子拉伸。
    result.position = vec3(dot(context.worldPosition, u_projectionX.xyz) - u_projectionX.w,
        dot(context.worldPosition, u_projectionY.xyz) - u_projectionY.w,
        dot(context.worldPosition, u_projectionZ.xyz) - u_projectionZ.w);
    result.normal = context.vertexColor.xyz;
#else
    // 源图的世界投影保持 UE XYZ；VL 是 (UE.X, UE.Z, UE.Y)，单位仍为 cm。
    result.position = context.worldPosition.xzy;
    result.normal = context.worldNormal.xzy;
#endif
    return result;
}

vec4 GaspProjectedTexture(sampler2D image, GaspProjection projection, float size)
{
    // 源 WorldAlignedTexture 除以负的 TextureSize，不能漏掉方向翻转。
    vec3 position = -projection.position / size;
    // 源 XYZ 输出不是归一化三轴权重：CheapContrast(c=1) = saturate(3*n-1)，
    // 先在 XZ/YZ 间混合，再按 Z 法线混入 XY。保留其拐角过渡与源图一致。
    vec2 blend = clamp(3.0 * abs(projection.normal.xz) - 1.0, 0.0, 1.0);
    vec4 side = mix(texture(image, position.xz), texture(image, position.yz), blend.x);
    return mix(side, texture(image, position.xy), blend.y);
}

MaterialInputs EvaluateMFGaspGrid(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    GaspProjection projection = GaspTextureProjection(context);
    vec3 grid = GaspProjectedTexture(grid1Map, projection, u_gridSizes.x).rgb +
        GaspProjectedTexture(grid2Map, projection, u_gridSizes.y).rgb +
        GaspProjectedTexture(grid3Map, projection, u_gridSizes.z).rgb;
    // 源图的 Saturate 是三层 mask 合成的一部分，不是数据范围兜底。
    grid = clamp(grid, 0.0, 1.0);
    vec3 surfaceColor = u_surfaceColor.rgb;
    vec4 gridColor = u_gridColor;
#if DIFFERENT_TOP_COLORS
    if (context.worldNormal.y > 0.5)
    {
        surfaceColor = u_topSurfaceColor.rgb;
        gridColor = u_topGridColor;
    }
#endif
    vec3 color = mix(surfaceColor, gridColor.rgb, grid * gridColor.a);
    inputs.roughness = u_roughness.z;
    inputs.normal = normalize(context.worldNormal);
#if DETAILED_SURFACE
    color *= GaspProjectedTexture(paintColorMap, projection, u_detailScales.x).rgb;
    // 名为 Glossiness 的源纹理实际在 M_Grid 中按 G 通道经幂/lerp 产生 roughness，
    // 不能凭文件名改成 1-gloss；颜色空间和原图翻行均在离线资产转换中固定。
    float roughnessMask = GaspProjectedTexture(paintRoughnessMap, projection, u_detailScales.y).g;
    inputs.roughness = clamp(mix(u_roughness.x, u_roughness.y,
        pow(roughnessMask * u_roughness.z, u_roughness.w)), 0.0, 1.0);
    // 保留源图把投影结果作为切线法线、仅缩放 XY 的语义，不改成 WorldAlignedNormal。
    vec3 normalTS = GaspProjectedTexture(paintNormalMap, projection, u_detailScales.z).xyz * 2.0 - 1.0;
    normalTS.xy *= u_normalStrength;
    inputs.normal = TransformMaterialNormalToWorld(context, normalTS);
#endif
    inputs.baseColor = color;
    inputs.specular = u_specular;
    inputs.tangent = OrthonormalizeMaterialTangent(inputs.normal, context.worldTangent);
    return inputs;
}

#endif

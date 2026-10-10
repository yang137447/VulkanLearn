#ifndef VL_M_UNLIT_SURFACE_GLSL
#define VL_M_UNLIT_SURFACE_GLSL

#include "materialFunction/mf_normal.glsl"
#include "engine/materialInputs.glsl"
#include "common/function.glsl"

// M_unlit 的公开 Surface 入口；Unlit ShadingModel 只消费材质输入，不执行光照。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    #if USE_ALBEDO_MAP
        vec4 albedo = texture(albedoMap, context.texCoord);
    #else
        vec4 albedo = vec4(1.0);
    #endif
    vec4 color = albedo * u_tintColor;
    #if USE_VERTEX_COLOR
        // 顶点颜色按 OBJ/导入器的 sRGB 颜色语义转为线性后再参与场景颜色。
        color *= vec4(SRGBtoLinear(context.vertexColor.rgb), 1.0);
    #endif
    // UE Unlit 的材质颜色语义是 Emissive；BaseColor 仍保留给 Buffer
    // Visualization，避免调试视图失去作者实际输入。
    inputs.baseColor = color.rgb;
    inputs.emissiveColor = color.rgb;
    inputs.opacity = color.a;
    inputs.opacityMask = color.a;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}

#endif

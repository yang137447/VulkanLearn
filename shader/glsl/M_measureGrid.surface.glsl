#ifndef VL_M_MEASURE_GRID_SURFACE_GLSL
#define VL_M_MEASURE_GRID_SURFACE_GLSL

#include "materialFunction/mf_measureGrid.glsl"
#include "engine/materialInputs.glsl"

// M_measureGrid 的公开 Surface 入口；测量网格是 DefaultLit 材质功能，不是独立 Pass。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    const vec4 baseColor = vec4(0.5, 0.5, 0.5, 1.0);
    const vec4 gridColor = vec4(0.0, 0.0, 0.0, 1.0);
    const vec4 subGridColor = vec4(0.2, 0.2, 0.2, 1.0);

    float subGridMask = CalculateMeasureGridMask(
        context.worldPosition,
        context.worldNormal,
        0.2);
    vec4 albedo = mix(baseColor, subGridColor, subGridMask);
    float gridMask = CalculateMeasureGridMask(
        context.worldPosition,
        context.worldNormal,
        1.0);
    albedo = mix(albedo, gridColor, gridMask);

    inputs.baseColor = albedo.rgb;
    inputs.opacity = albedo.a;
    inputs.opacityMask = albedo.a;
    inputs.roughness = mix(1.0, 0.5, max(gridMask, subGridMask));
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}

#endif

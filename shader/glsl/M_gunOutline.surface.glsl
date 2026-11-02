#ifndef VL_M_GUNOUTLINE_SURFACE_GLSL
#define VL_M_GUNOUTLINE_SURFACE_GLSL

#include "engine/materialInputs.glsl"
#include "common/function.glsl"
#include "common/commonUbo.glsl"

// 描边像素阶段：颜色/强度由 gameplay 驱动，fade 与抓帧一致。
// 壳用 cull None + facing 判定只保留正面（抓帧做法），背面 alpha 归零；
// 深度偏置在顶点阶段按壳宽自适应，因此不再有"固定 3cm 在远处不够"的问题。
// 曝光耦合：抓帧先乘 1/exposure 写入场景色、色调映射再乘回 exposure，净结果
// = 0.5*3.2119；VL 的 toneMapping 同样对 scene 乘 exposure，故折进 u_outlineColorScale。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    float distanceToCamera = length(context.worldPosition - uboVP.cameraPosition);
    float fade = clamp((u_fadeDistance - distanceToCamera) / max(u_fadeDistance, 1.0), 0.0, 1.0);
    float facing = gl_FrontFacing ? 1.0 : 0.0;
    vec3 color = u_outlineColor.rgb * u_outlineColorScale;
    float alpha = u_outlineColor.a * u_outlineOpacity * fade * facing;
    inputs.baseColor = color;
    inputs.emissiveColor = color;
    inputs.opacity = alpha;
    inputs.opacityMask = alpha;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}

#endif

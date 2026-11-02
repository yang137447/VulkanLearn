#ifndef VL_M_GUNOUTLINE_C_SURFACE_GLSL
#define VL_M_GUNOUTLINE_C_SURFACE_GLSL

#include "engine/materialInputs.glsl"
#include "common/function.glsl"
#include "common/commonUbo.glsl"

// C: cull Front 只保留壳的背面，剪影内被本体遮挡，故无需 facing 判定。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    float distanceToCamera = length(context.worldPosition - uboVP.cameraPosition);
    float fade = clamp((u_fadeDistance - distanceToCamera) / max(u_fadeDistance, 1.0), 0.0, 1.0);
    vec3 color = u_outlineColor.rgb * u_outlineColorScale;
    float alpha = u_outlineColor.a * u_outlineOpacity * fade;
    inputs.baseColor = color;
    inputs.emissiveColor = color;
    inputs.opacity = alpha;
    inputs.opacityMask = alpha;
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}

#endif

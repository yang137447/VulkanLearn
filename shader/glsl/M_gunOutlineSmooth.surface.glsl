#ifndef VL_M_GUNOUTLINEB_SURFACE_GLSL
#define VL_M_GUNOUTLINEB_SURFACE_GLSL

#include "engine/materialInputs.glsl"
#include "common/function.glsl"
#include "common/commonUbo.glsl"

// C: welded smooth normals + body GBuffer mask; single front-face layer like A.
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    vec2 screenUv = context.clipPosition.xy /
        max(context.clipPosition.w, 1.0e-6) * 0.5 + 0.5;
    if (texture(gbufferVelocity, screenUv).a > 0.5)
        discard;

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

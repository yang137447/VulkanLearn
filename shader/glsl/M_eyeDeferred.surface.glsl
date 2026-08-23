#ifndef VL_M_EYE_SURFACE_GLSL
#define VL_M_EYE_SURFACE_GLSL

#include "common/commonUbo.glsl"
#include "materialFunction/mf_pbrInputs.glsl"
#include "engine/eyeGeometry.glsl"

// Legacy/Substrate authoring 最终都落到同一个 EyeMaterialInputs；这里保持
// CorneaNormal、IrisNormal、IrisPlaneNormal 三个方向的独立语义。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = EvaluateMFPbrInputs(context);
    inputs.baseColor = u_eyeScleraColor.rgb * u_tintColor.rgb;
    inputs.roughness = u_eyeSurface.x;
    inputs.metallic = 0.0;
    inputs.specular = 0.5;
    inputs.ambientOcclusion = u_eyeSurface.z;
    inputs.opacity = u_tintColor.a;
    inputs.opacityMask = u_tintColor.a;
    inputs.emissiveColor = u_eyeEmissiveColor.rgb * u_emissiveStrength;

    vec3 corneaNormal = normalize(context.worldNormal);
    vec3 tangent = normalize(context.worldTangent.xyz);
    vec3 irisPlaneNormal = corneaNormal;
    vec3 viewDirectionToCamera = normalize(
        uboVP.cameraPosition - context.worldPosition);
    EyeGeometrySnapshot geometry = EvaluateEyeGeometry(
        context.worldPosition,
        corneaNormal,
        irisPlaneNormal,
        tangent,
        context.worldTangent.w,
        viewDirectionToCamera,
        u_eyeCorneaIor,
        u_eyeGeometry.x,
        u_eyeGeometry.y,
        u_eyeGeometry.z,
        u_eyeGeometry.w,
        u_eyeUvHandedness);

    vec3 irisNormal = irisPlaneNormal;
    vec3 irisColor = u_eyeIrisColor.rgb;
    float irisMask = 0.0;
    if (geometry.validIrisHit > 0.0)
    {
#if USE_IRIS_NORMAL_MAP
        vec3 irisNormalTS = texture(
            irisNormalMap,
            geometry.irisUv).xyz * 2.0 - 1.0;
        vec3 bitangent = normalize(
            cross(corneaNormal, tangent) * context.worldTangent.w);
        irisNormal = normalize(
            tangent * irisNormalTS.x +
            bitangent * irisNormalTS.y +
            irisPlaneNormal * irisNormalTS.z);
#endif
#if USE_IRIS_COLOR_MAP
        irisColor *= texture(irisColorMap, geometry.irisUv).rgb;
#endif
        irisMask = u_eyeSurface.y;
#if USE_IRIS_MASK_MAP
        irisMask *= texture(irisMaskMap, geometry.irisUv).r;
#endif
    }

    vec3 scleraColor = u_eyeScleraColor.rgb;
#if USE_SCLERA_COLOR_MAP
    scleraColor *= texture(scleraColorMap, context.texCoord).rgb;
#endif

    inputs.modelInputs.eye.corneaNormal = corneaNormal;
    inputs.modelInputs.eye.corneaIor = u_eyeCorneaIor;
    inputs.modelInputs.eye.irisNormal = irisNormal;
    inputs.modelInputs.eye.irisMask = irisMask;
    inputs.modelInputs.eye.irisPlaneNormal = irisPlaneNormal;
    inputs.modelInputs.eye.irisDistance = u_eyeGeometry.x;
    inputs.modelInputs.eye.irisColor = irisColor;
    inputs.modelInputs.eye.irisRadius = u_eyeGeometry.y;
    inputs.modelInputs.eye.scleraColor = scleraColor;
    inputs.modelInputs.eye.pupilRadius = u_eyeGeometry.z;
    inputs.modelInputs.eye.limbusWidth = u_eyeGeometry.w;
    inputs.modelInputs.eye.causticProfileId = u_eyeProfileId;
    inputs.modelInputs.eye.scleraProfileId = u_eyeScleraProfileId;
    inputs.modelInputs.eye.causticStrength = u_eyeCausticStrength;
    inputs.modelInputs.eye.validIrisHit = geometry.validIrisHit;
    inputs.modelInputs.eye.irisUv = geometry.irisUv;
    inputs.modelInputs.eye.irisHitDistance = geometry.irisHitDistance;
    inputs.modelInputs.eye.pupilMask = geometry.pupilMask * irisMask;
    inputs.modelInputs.eye.limbusMask = geometry.limbusMask;
    inputs.modelInputs.eye.eyeLayer = u_eyeLayer;
    inputs.modelInputs.eye.contactVisibility = u_eyeContactVisibility;
    inputs.modelInputs.eye.ciliaVisibility = u_eyeCiliaVisibility;
    inputs.modelInputs.eye.uvHandedness = u_eyeUvHandedness;
    inputs.modelInputs.eye.pupilDilation = u_eyePupilDilation;
    inputs.modelInputs.eye.gazeDirection = normalize(u_eyeGaze.xyz);
    inputs.modelInputs.eye.gazeWeight = u_eyeGaze.w;
    return inputs;
}

#endif

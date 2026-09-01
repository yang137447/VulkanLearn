#version 450

#include "generate/M_deferredLightingParamter.glsl"
#include "../engine/gbufferCodec.glsl"
#include "../engine/deferredLighting.glsl"
#include "../engine/materialDebugView.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outDiffuseLighting;
layout(location = 1) out vec4 outNonDiffuseLighting;
layout(location = 2) out vec4 outTransmissionLighting;
layout(location = 3) out vec4 outSssSource;

// Set 3 是 RenderGraph 为“当前 pass 输入”预留的 descriptor set。
// 这里的 binding 顺序必须和 config/renderGraphConfig.json 中 deferredLighting.input 完全一致。
layout(set = 3, binding = 0) uniform sampler2D gbufferA;
layout(set = 3, binding = 1) uniform sampler2D gbufferB;
layout(set = 3, binding = 2) uniform sampler2D gbufferC;
layout(set = 3, binding = 3) uniform sampler2D gbufferD;
layout(set = 3, binding = 4) uniform sampler2D gbufferE;
layout(set = 3, binding = 5) uniform sampler2D gbufferVelocity;
layout(set = 3, binding = 6) uniform sampler2D gbufferF;
layout(set = 3, binding = 7) uniform sampler2D sceneColorBase;
layout(set = 3, binding = 8) uniform sampler2D sceneDepth;
GBufferData SampleGBuffer(vec2 uv)
{
    GBufferData data;
    data.gbufferA = texture(gbufferA, uv);
    data.gbufferB = texture(gbufferB, uv);
    data.gbufferC = texture(gbufferC, uv);
    data.gbufferD = texture(gbufferD, uv);
    data.gbufferE = texture(gbufferE, uv);
    data.gbufferVelocity = texture(gbufferVelocity, uv);
    data.gbufferF = texture(gbufferF, uv);
    data.sceneColorBase = texture(sceneColorBase, uv);
    return data;
}

void main()
{
    GBufferData gbuffer = SampleGBuffer(inUV);
    MaterialSurface surface = DecodeGBufferSurface(gbuffer);

    float deviceDepth = texture(sceneDepth, inUV).r;
    surface.worldPosition = ReconstructWorldPositionFromSceneDepth(inUV, deviceDepth);

    DeferredLightingResult lighting = ShadeDeferredSurfaceDetailed(
        surface,
        shadowMap,
        subsurfaceProfileTable,
        preintegratedSkinLutTable);
    vec4 finalColor = vec4(lighting.finalColor, surface.opacity);
    MaterialDebugLightingData debugLighting = CreateMaterialDebugLightingData(
        lighting.shadow,
        lighting.shadowCascadeIndex,
        lighting.directLighting,
        lighting.indirectDiffuse,
        lighting.indirectSpecular);
    if (surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN)
    {
        SetMaterialDebugSkinData(
            debugLighting,
            lighting.skinDirectDiffuse,
            lighting.skinTransmission,
            lighting.skinShadowVisibility,
            lighting.skinIblDiffuse,
            lighting.skinIblSpecular,
            lighting.skinVirtualLight);
    }
    if (surface.shadingModel == SHADING_MODEL_EYE)
    {
        SetMaterialDebugEyeData(
            debugLighting,
            lighting.eyeCorneaSpecular,
            lighting.eyeIrisDirect,
            lighting.eyeScleraDirect,
            lighting.eyeInnerIbl,
            lighting.eyeRefractedViewDirection,
            lighting.eyeShadowCornea,
            lighting.eyeShadowInner,
            lighting.eyeCorneaFresnel,
            lighting.eyeTransmissionIn,
            lighting.eyeTransmissionOut,
            lighting.eyeIrisHitDistance,
            lighting.eyeIrisUv,
            lighting.eyeValidIrisHit,
            lighting.eyeIrisMask,
            lighting.eyePupilMask,
            lighting.eyeLimbusMask,
            lighting.eyeCausticGain);
    }
    if (surface.shadingModel == SHADING_MODEL_HAIR)
    {
        SetMaterialDebugHairData(
            debugLighting,
            lighting.hairRPath,
            lighting.hairTTPath,
            lighting.hairTRTPath,
            lighting.hairPathLength,
            lighting.hairAbsorption,
            lighting.hairLutCoordinates,
            lighting.hairIblFallback,
            lighting.hairMultipleScatteringFallback,
            lighting.hairTangent,
            lighting.hairBitangent,
            lighting.hairThetaI,
            lighting.hairThetaO,
            lighting.hairThetaH,
            lighting.hairThetaD,
            lighting.hairDeltaPhi,
            lighting.hairCoverage,
            lighting.hairDensity,
            lighting.hairShadowTransmittance);
    }
    if (surface.shadingModel == SHADING_MODEL_CLOTH)
    {
        SetMaterialDebugClothData(
            debugLighting,
            lighting.clothSheenColor,
            lighting.clothSheenRoughness,
            lighting.clothCharlieD,
            lighting.clothVisibility,
            lighting.clothModelVersion,
            lighting.clothWorldTangent,
            lighting.clothAnisotropy,
            lighting.clothAnisotropyCross,
            lighting.clothRoughnessAxes,
            lighting.clothDirectionalAlbedo,
            lighting.clothBaseEnergyScale,
            lighting.clothDirectSheen,
            lighting.clothIndirectSheen,
            lighting.clothIblFallback);
    }
    debugLighting.localSubsurfaceLighting =
        lighting.localSubsurfaceLighting;
    debugLighting.diffuseBeforeSubsurface =
        lighting.defaultDiffuseLighting;
    debugLighting.subsurfaceWeight = lighting.subsurfaceWeight;
    debugLighting.transmissionWeight = lighting.transmissionWeight;
    vec4 resolvedColor = ResolveMaterialDebugView(
        surface,
        debugLighting,
        finalColor);

    if ((uboVP.debugViewMode >= 1 &&
         uboVP.debugViewMode <= 17) ||
        (uboVP.debugViewMode >= 21 &&
         uboVP.debugViewMode <= 41) ||
        (uboVP.debugViewMode >= 64 &&
         uboVP.debugViewMode <= 89))
    {
        outDiffuseLighting = resolvedColor;
        outNonDiffuseLighting = vec4(0.0);
        outTransmissionLighting = vec4(0.0);
        outSssSource = vec4(0.0);
        return;
    }

    outDiffuseLighting = vec4(lighting.diffuseLighting, surface.opacity);
    outNonDiffuseLighting = vec4(lighting.nonDiffuseLighting, 0.0);
    outTransmissionLighting = vec4(lighting.transmissionLighting, 0.0);
    vec3 sssSource = vec3(0.0);
    if (surface.shadingModel == SHADING_MODEL_SUBSURFACE_PROFILE)
    {
        sssSource = lighting.diffuseLighting;
    }
    else if (surface.shadingModel == SHADING_MODEL_EYE)
    {
        sssSource = lighting.eyeScleraDirect +
            lighting.eyeInnerIbl *
                (1.0 - lighting.eyeIrisMask);
    }
    outSssSource = vec4(sssSource, 1.0);
}

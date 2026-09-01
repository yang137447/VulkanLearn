#ifndef VL_ENGINE_DEFERRED_LIGHTING_GLSL
#define VL_ENGINE_DEFERRED_LIGHTING_GLSL

#include "../common/commonUbo.glsl"
layout(set = 3, binding = 9) uniform sampler2DArrayShadow shadowMap;
layout(set = 3, binding = 10) uniform sampler2DArray hairAzimuthalLut;
layout(set = 3, binding = 11) uniform sampler2DArray eyeCausticLut;
layout(set = 3, binding = 12) uniform sampler2D clothDirectionalAlbedoLut;
layout(set = 3, binding = 13) uniform sampler2DArray clothAnisotropicDirectionalAlbedoLut;
#include "materialSurface.glsl"
#include "../common/lighting.glsl"
#include "subsurfaceLighting.glsl"
#include "preintegratedSkinLighting.glsl"
#include "subsurfaceProfileLighting.glsl"
#include "hairLighting.glsl"
#include "eyeLighting.glsl"
#include "clothLighting.glsl"

vec3 ReconstructWorldPositionFromSceneDepth(vec2 uv, float deviceDepth)
{
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clipPosition = vec4(ndc, deviceDepth, 1.0);
    vec4 worldPosition = uboVP.invViewProjection * clipPosition;
    return worldPosition.xyz / worldPosition.w;
}

struct DeferredLightingResult
{
    vec3 directDiffuse;
    vec3 directSpecular;
    vec3 directLighting;
    float shadow;
    float shadowCascadeIndex;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 indirectLighting;
    vec3 diffuseLighting;
    vec3 nonDiffuseLighting;
    vec3 transmissionLighting;
    vec3 localSubsurfaceLighting;
    vec3 defaultDiffuseLighting;
    float subsurfaceWeight;
    float transmissionWeight;
    // Hair path decomposition is kept in the same result snapshot for Forward/Deferred debug views.
    vec3 hairRPath;
    vec3 hairTTPath;
    vec3 hairTRTPath;
    float hairPathLength;
    vec3 hairAbsorption;
    vec2 hairLutCoordinates;
    float hairIblFallback;
    float hairMultipleScatteringFallback;
    vec3 hairTangent;
    vec3 hairBitangent;
    float hairThetaI;
    float hairThetaO;
    float hairThetaH;
    float hairThetaD;
    float hairDeltaPhi;
    float hairCoverage;
    float hairDensity;
    float hairShadowTransmittance;
    vec3 eyeCorneaSpecular;
    vec3 eyeIrisDirect;
    vec3 eyeScleraDirect;
    vec3 eyeInnerIbl;
    vec3 eyeRefractedViewDirection;
    float eyeShadowCornea;
    float eyeShadowInner;
    float eyeCorneaFresnel;
    float eyeTransmissionIn;
    float eyeTransmissionOut;
    float eyeIrisHitDistance;
    vec2 eyeIrisUv;
    float eyeValidIrisHit;
    float eyeIrisMask;
    float eyePupilMask;
    float eyeLimbusMask;
    float eyeCausticGain;
    vec3 clothDirectSheen;
    vec3 clothIndirectSheen;
    vec3 clothBaseEnergyScale;
    float clothDirectionalAlbedo;
    float clothIblFallback;
    vec3 clothSheenColor;
    float clothSheenRoughness;
    float clothCharlieD;
    float clothVisibility;
    float clothModelVersion;
    vec3 clothWorldTangent;
    float clothAnisotropy;
    float clothAnisotropyCross;
    vec2 clothRoughnessAxes;
    vec3 finalColor;
};

DeferredLightingResult CreateDefaultDeferredLightingResult()
{
    DeferredLightingResult result;
    result.directDiffuse = vec3(0.0);
    result.directSpecular = vec3(0.0);
    result.directLighting = vec3(0.0);
    result.shadow = 1.0;
    result.shadowCascadeIndex = 0.0;
    result.indirectDiffuse = vec3(0.0);
    result.indirectSpecular = vec3(0.0);
    result.indirectLighting = vec3(0.0);
    result.diffuseLighting = vec3(0.0);
    result.nonDiffuseLighting = vec3(0.0);
    result.transmissionLighting = vec3(0.0);
    result.localSubsurfaceLighting = vec3(0.0);
    result.defaultDiffuseLighting = vec3(0.0);
    result.subsurfaceWeight = 0.0;
    result.transmissionWeight = 0.0;
    result.hairRPath = vec3(0.0);
    result.hairTTPath = vec3(0.0);
    result.hairTRTPath = vec3(0.0);
    result.hairPathLength = 0.0;
    result.hairAbsorption = vec3(0.0);
    result.hairLutCoordinates = vec2(0.0);
    result.hairIblFallback = 0.0;
    result.hairMultipleScatteringFallback = 0.0;
    result.hairTangent = vec3(1.0, 0.0, 0.0);
    result.hairBitangent = vec3(0.0, 1.0, 0.0);
    result.hairThetaI = 0.0;
    result.hairThetaO = 0.0;
    result.hairThetaH = 0.0;
    result.hairThetaD = 0.0;
    result.hairDeltaPhi = 0.0;
    result.hairCoverage = 1.0;
    result.hairDensity = 1.0;
    result.hairShadowTransmittance = 1.0;
    result.eyeCorneaSpecular = vec3(0.0);
    result.eyeIrisDirect = vec3(0.0);
    result.eyeScleraDirect = vec3(0.0);
    result.eyeInnerIbl = vec3(0.0);
    result.eyeRefractedViewDirection = vec3(0.0, 0.0, -1.0);
    result.eyeShadowCornea = 1.0;
    result.eyeShadowInner = 1.0;
    result.eyeCorneaFresnel = 0.0;
    result.eyeTransmissionIn = 1.0;
    result.eyeTransmissionOut = 1.0;
    result.eyeIrisHitDistance = 0.0;
    result.eyeIrisUv = vec2(0.0);
    result.eyeValidIrisHit = 0.0;
    result.eyeIrisMask = 0.0;
    result.eyePupilMask = 0.0;
    result.eyeLimbusMask = 0.0;
    result.eyeCausticGain = 1.0;
    result.clothDirectSheen = vec3(0.0);
    result.clothIndirectSheen = vec3(0.0);
    result.clothBaseEnergyScale = vec3(1.0);
    result.clothDirectionalAlbedo = 0.0;
    result.clothIblFallback = 1.0;
    result.clothSheenColor = vec3(0.0);
    result.clothSheenRoughness = 0.0;
    result.clothCharlieD = 0.0;
    result.clothVisibility = 0.0;
    result.clothModelVersion = 0.0;
    result.clothWorldTangent = vec3(0.0);
    result.clothAnisotropy = 0.0;
    result.clothAnisotropyCross = 0.0;
    result.clothRoughnessAxes = vec2(0.0);
    result.finalColor = vec3(0.0);
    return result;
}

void ResolveDeferredLightingComposition(
    in MaterialSurface surface,
    inout DeferredLightingResult result)
{
    result.directLighting =
        result.directDiffuse +
        result.directSpecular +
        result.transmissionLighting;
    result.indirectLighting =
        result.indirectDiffuse + result.indirectSpecular;
    result.diffuseLighting =
        result.directDiffuse +
        result.indirectDiffuse * surface.ambientOcclusion;
    result.nonDiffuseLighting =
        surface.emissiveColor +
        result.directSpecular +
        result.indirectSpecular * surface.ambientOcclusion;
    result.finalColor =
        result.diffuseLighting +
        result.nonDiffuseLighting +
        result.transmissionLighting;
}

DeferredLightingResult ShadeDefaultLitDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    DeferredLightingResult result =
        CreateDefaultDeferredLightingResult();
    vec3 viewDir = normalize(
        uboVP.cameraPosition - surface.worldPosition);
    LightingLobes directLobes = CalculateDirectLightingLobes(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic);

    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        inputShadowMap,
        surface.worldPosition,
        surface.worldNormal,
        cascadeIndex);
    result.shadowCascadeIndex =
        ShadowCascadeDebugValue(cascadeIndex);
    result.shadow *= surface.precomputedShadowFactors.r;
    result.directDiffuse = directLobes.diffuse * result.shadow;
    result.directSpecular = directLobes.specular * result.shadow;

    result.indirectDiffuse = CalculateDiffuseIbl(
        surface.worldNormal,
        surface.baseColor,
        surface.metallic);
    result.indirectSpecular = CalculateSpecularIbl(
        surface.worldNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        surface.metallic);
    ResolveDeferredLightingComposition(surface, result);
    result.defaultDiffuseLighting = result.diffuseLighting;
    return result;
}

DeferredLightingResult ShadeSubsurfaceDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    DeferredLightingResult result =
        ShadeDefaultLitDeferredSurfaceDetailed(
            surface,
            inputShadowMap);
    SubsurfaceLocalLighting localLighting =
        CalculateSubsurfaceLocalDirectLighting(surface);
    vec3 localDirectDiffuse =
        localLighting.diffuse * result.shadow;
    vec3 localIndirectDiffuse =
        CalculateSubsurfaceLocalIndirectDiffuse(surface);
    float weight = surface.modelInputs.subsurface.weight;
    float transmissionWeight =
        weight *
        surface.modelInputs.subsurface.transmissionWeight;
    // 先从 diffuse 反射能量中预留 transmission 份额，避免 profile/local response 重复计能。
    float reflectedFraction = 1.0 - transmissionWeight;

    result.localSubsurfaceLighting =
        localDirectDiffuse +
        localIndirectDiffuse * surface.ambientOcclusion;
    result.subsurfaceWeight = weight;
    result.transmissionWeight = transmissionWeight;
    result.directDiffuse = mix(
        result.directDiffuse,
        localDirectDiffuse,
        weight) * reflectedFraction;
    result.indirectDiffuse = mix(
        result.indirectDiffuse,
        localIndirectDiffuse,
        weight) * reflectedFraction;
    result.transmissionLighting =
        localLighting.transmission *
        result.shadow *
        transmissionWeight;
    ResolveDeferredLightingComposition(surface, result);
    return result;
}

DeferredLightingResult ShadePreintegratedSkinDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    in sampler2D skinLutTable)
{
    DeferredLightingResult result =
        ShadeDefaultLitDeferredSurfaceDetailed(
            surface,
            inputShadowMap);
    PreintegratedSkinLighting skinLighting =
        CalculatePreintegratedSkinDirectLighting(
            surface,
            skinLutTable);
    vec3 localDirectDiffuse =
        skinLighting.diffuse * result.shadow;
    vec3 localIndirectDiffuse =
        CalculatePreintegratedSkinIndirectDiffuse(
            surface,
            skinLutTable);
    float weight =
        surface.modelInputs.preintegratedSkin.weight;
    float transmissionWeight =
        weight *
        surface.modelInputs.preintegratedSkin.transmissionWeight;
    // transmission 与 LUT diffuse response 分路输出，最终 composition 只合成一次。
    float reflectedFraction = 1.0 - transmissionWeight;

    result.localSubsurfaceLighting =
        localDirectDiffuse +
        localIndirectDiffuse * surface.ambientOcclusion;
    result.subsurfaceWeight = weight;
    result.transmissionWeight = transmissionWeight;
    result.directDiffuse = mix(
        result.directDiffuse,
        localDirectDiffuse,
        weight) * reflectedFraction;
    result.indirectDiffuse = mix(
        result.indirectDiffuse,
        localIndirectDiffuse,
        weight) * reflectedFraction;
    result.transmissionLighting =
        skinLighting.transmission *
        result.shadow *
        transmissionWeight;
    ResolveDeferredLightingComposition(surface, result);
    return result;
}

DeferredLightingResult ShadeSubsurfaceProfileDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    in sampler2D profileTable)
{
    DeferredLightingResult result =
        ShadeDefaultLitDeferredSurfaceDetailed(
            surface,
            inputShadowMap);
    float weight =
        surface.modelInputs.subsurfaceProfile.weight;
    float transmissionWeight =
        weight *
        surface.modelInputs.subsurfaceProfile.transmissionWeight;
    // profile filter 只处理 diffuse；transmission 先扣除反射份额再独立输出。
    float reflectedFraction = 1.0 - transmissionWeight;
    result.directDiffuse *= reflectedFraction;
    result.indirectDiffuse *= reflectedFraction;
    result.subsurfaceWeight = weight;
    result.transmissionWeight = transmissionWeight;
    result.transmissionLighting =
        CalculateSubsurfaceProfileTransmission(
            surface,
            profileTable) *
        result.shadow *
        transmissionWeight;
    ResolveDeferredLightingComposition(surface, result);
    // ID 5 的空间 response 在后处理 profile filter 中产生；这里不把未过滤 diffuse
    // 冒充为 Debug View 16 的 local response，避免与 ID 2/3 的语义混淆。
    result.localSubsurfaceLighting = vec3(0.0);
    return result;
}

DeferredLightingResult ShadeClearCoatDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    DeferredLightingResult result =
        CreateDefaultDeferredLightingResult();
    vec3 viewDir = normalize(
        uboVP.cameraPosition - surface.worldPosition);
    result.directLighting = CalculateClearCoatDirectLighting(
        surface.worldNormal,
        surface.clearCoatBottomNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.customData.x,
        surface.customData.y);

    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        inputShadowMap,
        surface.worldPosition,
        surface.worldNormal,
        cascadeIndex);
    result.shadowCascadeIndex =
        ShadowCascadeDebugValue(cascadeIndex);
    result.shadow *= surface.precomputedShadowFactors.r;
    result.directLighting *= result.shadow;

    result.indirectDiffuse = CalculateClearCoatDiffuseIbl(
        surface.worldNormal,
        surface.clearCoatBottomNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.customData.x);
    result.indirectSpecular = CalculateClearCoatSpecularIbl(
        surface.worldNormal,
        surface.clearCoatBottomNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.customData.x,
        surface.customData.y);
    result.indirectLighting =
        result.indirectDiffuse + result.indirectSpecular;
    result.nonDiffuseLighting =
        surface.emissiveColor +
        result.directLighting +
        result.indirectLighting * surface.ambientOcclusion;
    result.finalColor = result.nonDiffuseLighting;
    return result;
}

DeferredLightingResult ShadeHairDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    // Deferred 只恢复 GBuffer V1 已冻结的 Hair 子集；所有路径公式仍由共享
    // evaluator 提供，避免 Forward 与 Deferred 演化出两套能量账本。
    HairLightingResult hair = ShadeHairSurface(
        surface,
        inputShadowMap,
        surface.precomputedShadowFactors.r);
    DeferredLightingResult result =
        CreateDefaultDeferredLightingResult();
    result.directSpecular = hair.directLighting;
    result.indirectDiffuse = hair.multipleScattering;
    result.indirectSpecular =
        hair.indirectR + hair.indirectTT + hair.indirectTRT;
    result.shadow = hair.shadow;
    result.shadowCascadeIndex = hair.shadowCascadeIndex;
    result.hairRPath = hair.directR;
    result.hairTTPath = hair.directTT;
    result.hairTRTPath = hair.directTRT;
    result.hairPathLength = hair.pathLength;
    result.hairAbsorption = hair.absorption;
    result.hairLutCoordinates = hair.lutCoordinates;
    result.hairIblFallback = hair.hairIblFallback;
    result.hairMultipleScatteringFallback = hair.multipleScatteringFallback;
    result.hairTangent = hair.tangent;
    result.hairBitangent = hair.bitangent;
    result.hairThetaI = hair.thetaI;
    result.hairThetaO = hair.thetaO;
    result.hairThetaH = hair.thetaH;
    result.hairThetaD = hair.thetaD;
    result.hairDeltaPhi = hair.deltaPhi;
    result.hairCoverage = hair.coverage;
    result.hairDensity = hair.density;
    result.hairShadowTransmittance = hair.shadowTransmittance;
    ResolveDeferredLightingComposition(surface, result);
    result.defaultDiffuseLighting = result.diffuseLighting;
    return result;
}
DeferredLightingResult ShadeEyeDeferredSurfaceDetailed(
    in MaterialSurface surface)
{
    // Deferred 只消费 GBuffer V1 快照；Eye evaluator 与 Forward 共用同一条账本。
    EyeLightingResult eye = ShadeEyeSurface(surface);
    DeferredLightingResult result = CreateDefaultDeferredLightingResult();
    result.directDiffuse = eye.directDiffuse;
    result.directSpecular = eye.directSpecular;
    result.indirectDiffuse = eye.indirectDiffuse;
    result.indirectSpecular = eye.indirectSpecular;
    result.shadow = eye.shadowCornea;
    result.eyeCorneaSpecular = eye.corneaSpecular;
    result.eyeIrisDirect = eye.irisDirect;
    result.eyeScleraDirect = eye.scleraDirect;
    result.eyeInnerIbl = eye.innerIbl;
    result.eyeRefractedViewDirection = eye.refractedViewDirection;
    result.eyeShadowCornea = eye.shadowCornea;
    result.eyeShadowInner = eye.shadowInner;
    result.eyeCorneaFresnel = eye.corneaFresnel;
    result.eyeTransmissionIn = eye.transmissionIn;
    result.eyeTransmissionOut = eye.transmissionOut;
    result.eyeIrisHitDistance = eye.irisHitDistance;
    result.eyeIrisUv = eye.irisUv;
    result.eyeValidIrisHit = eye.validIrisHit;
    result.eyeIrisMask = eye.irisMask;
    result.eyePupilMask = eye.pupilMask;
    result.eyeLimbusMask = eye.limbusMask;
    result.eyeCausticGain = eye.causticGain;
    ResolveDeferredLightingComposition(surface, result);
    result.defaultDiffuseLighting = result.diffuseLighting;
    return result;
}

DeferredLightingResult ShadeClothDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    ClothLightingResult cloth = ShadeClothSurface(
        surface,
        clothDirectionalAlbedoLut,
        clothAnisotropicDirectionalAlbedoLut,
        inputShadowMap);
    DeferredLightingResult result = CreateDefaultDeferredLightingResult();
    result.directDiffuse = cloth.directDiffuse;
    result.directSpecular = cloth.directSpecular + cloth.directSheen;
    result.indirectDiffuse = cloth.indirectDiffuse;
    result.indirectSpecular = cloth.indirectSpecular + cloth.indirectSheen;
    result.shadow = cloth.shadow;
    result.shadowCascadeIndex = cloth.shadowCascadeIndex;
    result.clothDirectSheen = cloth.directSheen;
    result.clothIndirectSheen = cloth.indirectSheen;
    result.clothBaseEnergyScale = cloth.baseEnergyScale;
    result.clothDirectionalAlbedo = cloth.directionalAlbedo;
    result.clothIblFallback = cloth.iblFallback;
    result.clothSheenColor = cloth.sheenColor;
    result.clothSheenRoughness = cloth.sheenRoughness;
    result.clothCharlieD = cloth.charlieD;
    result.clothVisibility = cloth.visibility;
    result.clothModelVersion = cloth.modelVersion;
    result.clothWorldTangent = cloth.worldTangent;
    result.clothAnisotropy = cloth.anisotropy;
    result.clothAnisotropyCross = cloth.anisotropyCross;
    result.clothRoughnessAxes = cloth.roughnessAxes;
    ResolveDeferredLightingComposition(surface, result);
    result.defaultDiffuseLighting = result.diffuseLighting;
    return result;
}

DeferredLightingResult ShadeUnlitDeferredSurfaceDetailed(
    in MaterialSurface surface)
{
    DeferredLightingResult result =
        CreateDefaultDeferredLightingResult();
    result.shadow = 1.0;
    result.nonDiffuseLighting =
        surface.baseColor + surface.emissiveColor;
    result.finalColor = result.nonDiffuseLighting;
    return result;
}

DeferredLightingResult ShadeDeferredSurfaceDetailed(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    in sampler2D profileTable,
    in sampler2D skinLutTable)
{
    switch (surface.shadingModel)
    {
        case SHADING_MODEL_UNLIT:
            return ShadeUnlitDeferredSurfaceDetailed(surface);
        case SHADING_MODEL_SUBSURFACE:
            return ShadeSubsurfaceDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
        case SHADING_MODEL_PREINTEGRATED_SKIN:
            return ShadePreintegratedSkinDeferredSurfaceDetailed(
                surface,
                inputShadowMap,
                skinLutTable);
        case SHADING_MODEL_CLEAR_COAT:
            return ShadeClearCoatDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
        case SHADING_MODEL_SUBSURFACE_PROFILE:
            return ShadeSubsurfaceProfileDeferredSurfaceDetailed(
                surface,
                inputShadowMap,
                profileTable);
        case SHADING_MODEL_HAIR:
            return ShadeHairDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
        case SHADING_MODEL_EYE:
            return ShadeEyeDeferredSurfaceDetailed(surface);
        case SHADING_MODEL_CLOTH:
            return ShadeClothDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
        default:
            return ShadeDefaultLitDeferredSurfaceDetailed(
                surface,
                inputShadowMap);
    }
}

vec3 ShadeDeferredSurface(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    in sampler2D profileTable,
    in sampler2D skinLutTable)
{
    return ShadeDeferredSurfaceDetailed(
        surface,
        inputShadowMap,
        profileTable,
        skinLutTable).finalColor;
}

#endif

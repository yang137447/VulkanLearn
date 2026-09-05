#ifndef VL_ENGINE_FORWARD_LIGHTING_GLSL
#define VL_ENGINE_FORWARD_LIGHTING_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"
#include "../common/lighting.glsl"
layout (set = 3, binding = 1) uniform sampler2DArray hairAzimuthalLut;
layout (set = 3, binding = 3) uniform sampler2D clothDirectionalAlbedoLut;
layout (set = 3, binding = 4) uniform sampler2DArray clothAnisotropicDirectionalAlbedoLut;
#include "hairLighting.glsl"
#include "clothLighting.glsl"
#include "twoSidedFoliageLighting.glsl"


// 前向 pass 如果需要阴影，由具体 pass shader 在 include 前定义
// VL_FORWARD_DECLARE_SHADOWMAP_INPUT。默认 include 不占用 Set 3，只有真正带阴影的
// forward pass 才声明 shadowMap 输入，避免材质布局被无关 pass 污染。
#if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
layout (set = 3, binding = 0) uniform sampler2DArrayShadow shadowMap;
#endif

#if MATERIAL_IS_EYE
layout (set = 3, binding = 2) uniform sampler2DArray eyeCausticLut;
#include "eyeLighting.glsl"
#endif


struct ForwardLightingResult
{
    vec3 directLighting;
    vec3 directDiffuse;
    vec3 directSpecular;
    vec3 transmissionLighting;
    float shadow;
    float shadowCascadeIndex;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 indirectLighting;
    vec3 finalColor;
    // Hair debug data 与普通光照结果同一份 snapshot，Forward/Deferred 使用同一 evaluator。
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
    // Eye debug/path ledger；只在 Eye variant 写入，其他模型保持零值。
    vec3 eyeCorneaSpecular;
    vec3 eyeIrisDirect;
    vec3 eyeScleraDirect;
    vec3 eyeInnerIbl;
    float eyeShadowCornea;
    float eyeShadowInner;
    float eyeCorneaFresnel;
    float eyeTransmissionIn;
    float eyeTransmissionOut;
    float eyeIrisHitDistance;
    vec2 eyeIrisUv;
    vec3 eyeRefractedViewDirection;
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
    vec3 foliageBacklitDirect;
    float foliageBacklightFactor;
};

ForwardLightingResult CreateDefaultForwardLightingResult()
{
    ForwardLightingResult result;
    result.directLighting = vec3(0.0);
    result.directDiffuse = vec3(0.0);
    result.directSpecular = vec3(0.0);
    result.transmissionLighting = vec3(0.0);
    result.shadow = 1.0;
    result.shadowCascadeIndex = 0.0;
    result.indirectDiffuse = vec3(0.0);
    result.indirectSpecular = vec3(0.0);
    result.indirectLighting = vec3(0.0);
    result.finalColor = vec3(0.0);
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
    result.eyeShadowCornea = 1.0;
    result.eyeShadowInner = 1.0;
    result.eyeCorneaFresnel = 0.0;
    result.eyeTransmissionIn = 1.0;
    result.eyeTransmissionOut = 1.0;
    result.eyeIrisHitDistance = 0.0;
    result.eyeIrisUv = vec2(0.0);
    result.eyeRefractedViewDirection = vec3(0.0, 0.0, -1.0);
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
    result.foliageBacklitDirect = vec3(0.0);
    result.foliageBacklightFactor = 0.0;
    return result;
}

ForwardLightingResult ShadeDefaultLitForwardSurface(in MaterialSurface surface)
{
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    vec3 viewDir = normalize(uboVP.cameraPosition - surface.worldPosition);

    result.directLighting = CalculateDirectLighting(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        0.5);
    #if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
        int cascadeIndex = 0;
        result.shadow = CalculateCsmShadow(
            shadowMap,
            surface.worldPosition,
            surface.worldNormal,
            cascadeIndex);
        result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    #else
        result.shadow = 1.0;
    #endif
    result.directLighting *= result.shadow;

    result.indirectDiffuse = CalculateDiffuseIbl(
        surface.worldNormal,
        surface.baseColor,
        surface.metallic);
    result.indirectSpecular = CalculateSpecularIbl(
        surface.worldNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        0.5);
    result.indirectLighting = result.indirectDiffuse + result.indirectSpecular;
    result.finalColor = surface.emissiveColor + result.directLighting + result.indirectLighting * surface.ambientOcclusion;
    return result;
}

ForwardLightingResult ShadeThinTranslucentForwardSurface(
    in MaterialSurface surface)
{
    // Thin Translucent 复用 Default Lit 的表面反射，但必须保留 diffuse/specular 分量，
    // 因为 RootOpacity 只覆盖 diffuse 与 emissive，表面高光不随根透明度衰减。
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    vec3 viewDir = normalize(
        uboVP.cameraPosition - surface.worldPosition);

    LightingLobes directLobes = CalculateDirectLightingLobes(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        0.5);
    #if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
        int cascadeIndex = 0;
        result.shadow = CalculateCsmShadow(
            shadowMap,
            surface.worldPosition,
            surface.worldNormal,
            cascadeIndex);
        result.shadowCascadeIndex =
            ShadowCascadeDebugValue(cascadeIndex);
    #else
        result.shadow = 1.0;
    #endif
    result.directDiffuse = directLobes.diffuse * result.shadow;
    result.directSpecular = directLobes.specular * result.shadow;
    result.directLighting =
        result.directDiffuse + result.directSpecular;

    result.indirectDiffuse = CalculateDiffuseIbl(
        surface.worldNormal,
        surface.baseColor,
        surface.metallic);
    result.indirectSpecular = CalculateSpecularIbl(
        surface.worldNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        0.5);
    result.indirectLighting =
        result.indirectDiffuse + result.indirectSpecular;
    result.finalColor =
        surface.emissiveColor +
        result.directLighting +
        result.indirectLighting * surface.ambientOcclusion;
    return result;
}

ForwardLightingResult ShadeClearCoatForwardSurface(in MaterialSurface surface)
{
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    vec3 viewDir = normalize(uboVP.cameraPosition - surface.worldPosition);

    // 顶层法线负责清漆高光，底层法线负责底漆响应；customData.xy 分别是
    // 清漆权重和清漆粗糙度，与 deferred 路径保持同一输入合同。
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
    #if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
        int cascadeIndex = 0;
        result.shadow = CalculateCsmShadow(
            shadowMap,
            surface.worldPosition,
            surface.worldNormal,
            cascadeIndex);
        result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    #else
        result.shadow = 1.0;
    #endif
    result.directLighting *= result.shadow;

    // Legacy Clear Coat 的间接光也按两层拆分：底层使用底层法线，
    // 顶层环境高光使用顶层法线，最后按 Fresnel 透射关系合成。
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
    result.indirectLighting = result.indirectDiffuse + result.indirectSpecular;
    result.finalColor =
        surface.emissiveColor +
        result.directLighting +
        result.indirectLighting * surface.ambientOcclusion;
    return result;
}

ForwardLightingResult ShadeHairForwardSurface(in MaterialSurface surface)
{
    HairLightingResult hair = ShadeHairSurface(surface, shadowMap);
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    result.directSpecular = hair.directLighting;
    result.directLighting = hair.directLighting;
    result.shadow = hair.shadow;
    result.shadowCascadeIndex = hair.shadowCascadeIndex;
    result.indirectDiffuse = hair.multipleScattering;
    result.indirectSpecular = hair.indirectR + hair.indirectTT +
        hair.indirectTRT + hair.indirectScatter;
    result.indirectLighting =
        result.indirectDiffuse + result.indirectSpecular;
    result.finalColor = hair.finalColor;
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
    return result;
}

ForwardLightingResult ShadeClothForwardSurface(in MaterialSurface surface)
{
    ClothLightingResult cloth = ShadeClothSurface(
        surface,
        clothDirectionalAlbedoLut,
        clothAnisotropicDirectionalAlbedoLut,
        shadowMap);
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    result.directDiffuse = cloth.directDiffuse;
    result.directSpecular = cloth.directSpecular;
    result.directLighting = cloth.directDiffuse +
        cloth.directSpecular + cloth.directSheen;
    result.indirectDiffuse = cloth.indirectDiffuse;
    result.indirectSpecular = cloth.indirectSpecular;
    result.indirectLighting = cloth.indirectDiffuse +
        cloth.indirectSpecular + cloth.indirectSheen;
    result.finalColor = surface.emissiveColor +
        result.directLighting + result.indirectLighting *
            surface.ambientOcclusion;
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
    return result;
}

ForwardLightingResult ShadeTwoSidedFoliageForwardSurface(
    in MaterialSurface surface)
{
    TwoSidedFoliageLightingResult foliage =
        ShadeTwoSidedFoliageSurface(surface);
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
#if defined(VL_FORWARD_DECLARE_SHADOWMAP_INPUT)
    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        shadowMap,
        surface.worldPosition,
        surface.worldNormal,
        cascadeIndex);
    result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
#else
    result.shadow = 1.0;
#endif
    result.directDiffuse = foliage.baseLighting.diffuse * result.shadow;
    result.directSpecular = foliage.baseLighting.specular * result.shadow;
    result.transmissionLighting = foliage.backlitDirect * result.shadow;
    result.directLighting =
        result.directDiffuse +
        result.directSpecular +
        result.transmissionLighting;
    result.indirectDiffuse = foliage.indirectDiffuse;
    result.indirectSpecular = foliage.indirectSpecular;
    result.foliageBacklitDirect = foliage.backlitDirect * result.shadow;
    result.foliageBacklightFactor = foliage.backlightFactor;
    result.indirectLighting = result.indirectDiffuse + result.indirectSpecular;
    result.finalColor = surface.emissiveColor + result.directLighting +
        result.indirectLighting * surface.ambientOcclusion;
    return result;
}
#if MATERIAL_IS_EYE
ForwardLightingResult ShadeEyeForwardSurface(in MaterialSurface surface)
{
    EyeLightingResult eye = ShadeEyeSurface(surface);
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    result.directLighting = eye.directLighting;
    result.directDiffuse = eye.directDiffuse;
    result.directSpecular = eye.directSpecular;
    result.indirectDiffuse = eye.indirectDiffuse;
    result.indirectSpecular = eye.indirectSpecular;
    result.indirectLighting = eye.indirectDiffuse + eye.indirectSpecular;
    result.finalColor = eye.finalColor;
    result.shadow = eye.shadowCornea;
    result.eyeCorneaSpecular = eye.corneaSpecular;
    result.eyeIrisDirect = eye.irisDirect;
    result.eyeScleraDirect = eye.scleraDirect;
    result.eyeInnerIbl = eye.innerIbl;
    result.eyeShadowCornea = eye.shadowCornea;
    result.eyeShadowInner = eye.shadowInner;
    result.eyeCorneaFresnel = eye.corneaFresnel;
    result.eyeTransmissionIn = eye.transmissionIn;
    result.eyeTransmissionOut = eye.transmissionOut;
    result.eyeIrisHitDistance = eye.irisHitDistance;
    result.eyeIrisUv = eye.irisUv;
    result.eyeRefractedViewDirection = eye.refractedViewDirection;
    result.eyeValidIrisHit = eye.validIrisHit;
    result.eyeIrisMask = eye.irisMask;
    result.eyePupilMask = eye.pupilMask;
    result.eyeLimbusMask = eye.limbusMask;
    result.eyeCausticGain = eye.causticGain;
    return result;
}
#endif
ForwardLightingResult ShadeUnlitForwardSurface(in MaterialSurface surface)
{
    ForwardLightingResult result = CreateDefaultForwardLightingResult();
    result.finalColor = surface.baseColor + surface.emissiveColor;
    return result;
}

ForwardLightingResult ShadeForwardSurfaceDetailed(in MaterialSurface surface)
{
    switch (surface.shadingModel)
    {
        case SHADING_MODEL_UNLIT:
            return ShadeUnlitForwardSurface(surface);
        case SHADING_MODEL_CLEAR_COAT:
            return ShadeClearCoatForwardSurface(surface);
        case SHADING_MODEL_THIN_TRANSLUCENT:
            return ShadeThinTranslucentForwardSurface(surface);
        case SHADING_MODEL_HAIR:
            return ShadeHairForwardSurface(surface);
        case SHADING_MODEL_CLOTH:
            return ShadeClothForwardSurface(surface);
        case SHADING_MODEL_TWOSIDED_FOLIAGE:
            return ShadeTwoSidedFoliageForwardSurface(surface);
#if MATERIAL_IS_EYE
        case SHADING_MODEL_EYE:
            return ShadeEyeForwardSurface(surface);
#endif
        default:
            return ShadeDefaultLitForwardSurface(surface);
    }
}

vec3 ShadeForwardSurface(in MaterialSurface surface)
{
    return ShadeForwardSurfaceDetailed(surface).finalColor;
}

#endif

#ifndef VL_ENGINE_MATERIAL_DEBUG_VIEW_GLSL
#define VL_ENGINE_MATERIAL_DEBUG_VIEW_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"

const float TWO_SIDED_FOLIAGE_DEBUG_GBUFFER_VERSION = 1.0;

struct MaterialDebugLightingData
{
    float shadow;
    float shadowCascadeIndex;
    vec3 directLighting;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 localSubsurfaceLighting;
    vec3 diffuseBeforeSubsurface;
    float subsurfaceWeight;
    float transmissionWeight;
    vec3 skinDirectDiffuse;
    vec3 skinTransmission;
    float skinShadowVisibility;
    vec3 skinIblDiffuse;
    vec3 skinIblSpecular;
    vec3 skinVirtualLight;
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
    vec3 clothSheenColor;
    float clothSheenRoughness;
    float clothCharlieD;
    float clothVisibility;
    float clothDirectionalAlbedo;
    vec3 clothBaseEnergyScale;
    vec3 clothDirectSheen;
    vec3 clothIndirectSheen;
    float clothIblFallback;
    float clothModelVersion;
    vec3 clothWorldTangent;
    float clothAnisotropy;
    float clothAnisotropyCross;
    vec2 clothRoughnessAxes;
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
    vec3 foliageSubsurfaceColor;
    vec3 foliageResolvedNormal;
    vec3 foliageBacklitDirect;
    float foliageBacklightFactor;
    float foliageShadowVisibility;
    float foliageFrontFacing;
    float foliageAlphaMask;
    float foliageGBufferCustomData;
    float foliageGBufferVersion;
};

MaterialDebugLightingData CreateMaterialDebugLightingData(
    float shadow,
    float shadowCascadeIndex,
    vec3 directLighting,
    vec3 indirectDiffuse,
    vec3 indirectSpecular)
{
    MaterialDebugLightingData data;
    data.shadow = shadow;
    data.shadowCascadeIndex = shadowCascadeIndex;
    data.directLighting = directLighting;
    data.indirectDiffuse = indirectDiffuse;
    data.indirectSpecular = indirectSpecular;
    data.localSubsurfaceLighting = vec3(0.0);
    data.diffuseBeforeSubsurface = vec3(0.0);
    data.subsurfaceWeight = 0.0;
    data.transmissionWeight = 0.0;
    data.skinDirectDiffuse = vec3(0.0);
    data.skinTransmission = vec3(0.0);
    data.skinShadowVisibility = 1.0;
    data.skinIblDiffuse = vec3(0.0);
    data.skinIblSpecular = vec3(0.0);
    data.skinVirtualLight = vec3(0.0);
    data.hairRPath = vec3(0.0);
    data.hairTTPath = vec3(0.0);
    data.hairTRTPath = vec3(0.0);
    data.hairPathLength = 0.0;
    data.hairAbsorption = vec3(0.0);
    data.hairLutCoordinates = vec2(0.0);
    data.hairIblFallback = 0.0;
    data.hairMultipleScatteringFallback = 0.0;
    data.hairTangent = vec3(1.0, 0.0, 0.0);
    data.hairBitangent = vec3(0.0, 1.0, 0.0);
    data.hairThetaI = 0.0;
    data.hairThetaO = 0.0;
    data.hairThetaH = 0.0;
    data.hairThetaD = 0.0;
    data.hairDeltaPhi = 0.0;
    data.hairCoverage = 1.0;
    data.hairDensity = 1.0;
    data.hairShadowTransmittance = 1.0;
    data.clothSheenColor = vec3(0.0);
    data.clothSheenRoughness = 0.0;
    data.clothCharlieD = 0.0;
    data.clothVisibility = 0.0;
    data.clothDirectionalAlbedo = 0.0;
    data.clothBaseEnergyScale = vec3(1.0);
    data.clothDirectSheen = vec3(0.0);
    data.clothIndirectSheen = vec3(0.0);
    data.clothIblFallback = 1.0;
    data.clothModelVersion = 0.0;
    data.clothWorldTangent = vec3(0.0);
    data.clothAnisotropy = 0.0;
    data.clothAnisotropyCross = 0.0;
    data.clothRoughnessAxes = vec2(0.0);
    data.eyeCorneaSpecular = vec3(0.0);
    data.eyeIrisDirect = vec3(0.0);
    data.eyeScleraDirect = vec3(0.0);
    data.eyeInnerIbl = vec3(0.0);
    data.eyeRefractedViewDirection = vec3(0.0, 0.0, -1.0);
    data.eyeShadowCornea = 1.0;
    data.eyeShadowInner = 1.0;
    data.eyeCorneaFresnel = 0.0;
    data.eyeTransmissionIn = 1.0;
    data.eyeTransmissionOut = 1.0;
    data.eyeIrisHitDistance = 0.0;
    data.eyeIrisUv = vec2(0.0);
    data.eyeValidIrisHit = 0.0;
    data.eyeIrisMask = 0.0;
    data.eyePupilMask = 0.0;
    data.eyeLimbusMask = 0.0;
    data.eyeCausticGain = 1.0;
    data.foliageSubsurfaceColor = vec3(0.0);
    data.foliageResolvedNormal = vec3(0.0, 0.0, 1.0);
    data.foliageBacklitDirect = vec3(0.0);
    data.foliageBacklightFactor = 0.0;
    data.foliageShadowVisibility = 1.0;
    data.foliageFrontFacing = 1.0;
    data.foliageAlphaMask = 1.0;
    data.foliageGBufferCustomData = 0.0;
    data.foliageGBufferVersion = 0.0;
    return data;
}

void SetMaterialDebugSkinData(
    inout MaterialDebugLightingData data,
    vec3 directDiffuse,
    vec3 transmission,
    float shadowVisibility,
    vec3 iblDiffuse,
    vec3 iblSpecular,
    vec3 virtualLight)
{
    data.skinDirectDiffuse = directDiffuse;
    data.skinTransmission = transmission;
    data.skinShadowVisibility = shadowVisibility;
    data.skinIblDiffuse = iblDiffuse;
    data.skinIblSpecular = iblSpecular;
    data.skinVirtualLight = virtualLight;
}

void SetMaterialDebugClothData(
    inout MaterialDebugLightingData data,
    vec3 sheenColor,
    float sheenRoughness,
    float charlieD,
    float visibility,
    float modelVersion,
    vec3 worldTangent,
    float anisotropy,
    float anisotropyCross,
    vec2 roughnessAxes,
    float directionalAlbedo,
    vec3 baseEnergyScale,
    vec3 directSheen,
    vec3 indirectSheen,
    float iblFallback)
{
    data.clothSheenColor = sheenColor;
    data.clothSheenRoughness = sheenRoughness;
    data.clothCharlieD = charlieD;
    data.clothVisibility = visibility;
    data.clothDirectionalAlbedo = directionalAlbedo;
    data.clothBaseEnergyScale = baseEnergyScale;
    data.clothDirectSheen = directSheen;
    data.clothIndirectSheen = indirectSheen;
    data.clothIblFallback = iblFallback;
    data.clothModelVersion = modelVersion;
    data.clothWorldTangent = worldTangent;
    data.clothAnisotropy = anisotropy;
    data.clothAnisotropyCross = anisotropyCross;
    data.clothRoughnessAxes = roughnessAxes;
}

void SetMaterialDebugHairData(
    inout MaterialDebugLightingData data,
    vec3 rPath,
    vec3 ttPath,
    vec3 trtPath,
    float pathLength,
    vec3 absorption,
    vec2 lutCoordinates,
    float iblFallback,
    float multipleScatteringFallback,
    vec3 tangent,
    vec3 bitangent,
    float thetaI,
    float thetaO,
    float thetaH,
    float thetaD,
    float deltaPhi,
    float coverage,
    float density,
    float shadowTransmittance)
{
    data.hairRPath = rPath;
    data.hairTTPath = ttPath;
    data.hairTRTPath = trtPath;
    data.hairPathLength = pathLength;
    data.hairAbsorption = absorption;
    data.hairLutCoordinates = lutCoordinates;
    data.hairIblFallback = iblFallback;
    data.hairMultipleScatteringFallback = multipleScatteringFallback;
    data.hairTangent = tangent;
    data.hairBitangent = bitangent;
    data.hairThetaI = thetaI;
    data.hairThetaO = thetaO;
    data.hairThetaH = thetaH;
    data.hairThetaD = thetaD;
    data.hairDeltaPhi = deltaPhi;
    data.hairCoverage = coverage;
    data.hairDensity = density;
    data.hairShadowTransmittance = shadowTransmittance;
}
void SetMaterialDebugEyeData(
    inout MaterialDebugLightingData data,
    vec3 corneaSpecular,
    vec3 irisDirect,
    vec3 scleraDirect,
    vec3 innerIbl,
    vec3 refractedViewDirection,
    float shadowCornea,
    float shadowInner,
    float corneaFresnel,
    float transmissionIn,
    float transmissionOut,
    float irisHitDistance,
    vec2 irisUv,
    float validIrisHit,
    float irisMask,
    float pupilMask,
    float limbusMask,
    float causticGain)
{
    data.eyeCorneaSpecular = corneaSpecular;
    data.eyeIrisDirect = irisDirect;
    data.eyeScleraDirect = scleraDirect;
    data.eyeInnerIbl = innerIbl;
    data.eyeRefractedViewDirection = refractedViewDirection;
    data.eyeShadowCornea = shadowCornea;
    data.eyeShadowInner = shadowInner;
    data.eyeCorneaFresnel = corneaFresnel;
    data.eyeTransmissionIn = transmissionIn;
    data.eyeTransmissionOut = transmissionOut;
    data.eyeIrisHitDistance = irisHitDistance;
    data.eyeIrisUv = irisUv;
    data.eyeValidIrisHit = validIrisHit;
    data.eyeIrisMask = irisMask;
    data.eyePupilMask = pupilMask;
    data.eyeLimbusMask = limbusMask;
    data.eyeCausticGain = causticGain;
}

void SetMaterialDebugFoliageData(
    inout MaterialDebugLightingData data,
    vec3 subsurfaceColor,
    vec3 resolvedNormal,
    vec3 backlitDirect,
    float backlightFactor,
    float shadowVisibility,
    float frontFacing,
    float alphaMask,
    float gbufferCustomData,
    float gbufferVersion)
{
    data.foliageSubsurfaceColor = subsurfaceColor;
    data.foliageResolvedNormal = resolvedNormal;
    data.foliageBacklitDirect = backlitDirect;
    data.foliageBacklightFactor = backlightFactor;
    data.foliageShadowVisibility = shadowVisibility;
    data.foliageFrontFacing = frontFacing;
    data.foliageAlphaMask = alphaMask;
    data.foliageGBufferCustomData = gbufferCustomData;
    data.foliageGBufferVersion = gbufferVersion;
}

float MaterialDebugViewModeMask(int mode)
{
    return 1.0 - min(abs(float(uboVP.debugViewMode - mode)), 1.0);
}

// Debug view 只依赖材质表面和一份中性的 lighting 调试数据。
// Forward / Deferred 可以分别把自己的 lighting result 转成 MaterialDebugLightingData，
// 这样 debug view 不需要 include forwardLighting 或 deferredLighting，避免路径互相缠住。
vec4 ResolveMaterialDebugView(
    in MaterialSurface surface,
    in MaterialDebugLightingData lighting,
    in vec4 defaultColor)
{
#if defined(ENABLE_DEBUG_VIEW)
    float baseColorMask = MaterialDebugViewModeMask(1);
    float emissiveMask = MaterialDebugViewModeMask(2);
    float normalMask = MaterialDebugViewModeMask(3);
    float roughnessMask = MaterialDebugViewModeMask(4);
    float metallicMask = MaterialDebugViewModeMask(5);
    float aoMask = MaterialDebugViewModeMask(6);
    float shadowMask = MaterialDebugViewModeMask(7);
    float directLightingMask = MaterialDebugViewModeMask(8);
    float indirectDiffuseMask = MaterialDebugViewModeMask(9);
    float indirectSpecularMask = MaterialDebugViewModeMask(10);
    float shadowCascadeMask = MaterialDebugViewModeMask(11);
    float shadingModelMask = MaterialDebugViewModeMask(12);
    float subsurfaceWeightMask = MaterialDebugViewModeMask(13);
    float transmissionWeightMask = MaterialDebugViewModeMask(14);
    float subsurfaceAssetIdMask = MaterialDebugViewModeMask(15);
    float localSubsurfaceMask = MaterialDebugViewModeMask(16);
    float diffuseBeforeSubsurfaceMask = MaterialDebugViewModeMask(17);
    float hairFrameMask = MaterialDebugViewModeMask(21);
    float hairTangentMask = MaterialDebugViewModeMask(22);
    float hairThetaMask = MaterialDebugViewModeMask(23);
    float hairDeltaPhiMask = MaterialDebugViewModeMask(24);
    float hairRMask = MaterialDebugViewModeMask(25);
    float hairTTMask = MaterialDebugViewModeMask(26);
    float hairTRTMask = MaterialDebugViewModeMask(27);
    float hairPathLengthMask = MaterialDebugViewModeMask(28);
    float hairAbsorptionMask = MaterialDebugViewModeMask(29);
    float hairCoverageMask = MaterialDebugViewModeMask(30);
    float hairShadowMask = MaterialDebugViewModeMask(31);
    float hairLutMask = MaterialDebugViewModeMask(32);
    float hairPrimaryMask = MaterialDebugViewModeMask(33);
    float hairSecondaryMask = MaterialDebugViewModeMask(34);
    float hairScatterMask = MaterialDebugViewModeMask(35);
    float hairBacklitMask = MaterialDebugViewModeMask(36);
    float hairRPathColorMask = MaterialDebugViewModeMask(37);
    float hairTTPathColorMask = MaterialDebugViewModeMask(38);
    float hairTRTPathColorMask = MaterialDebugViewModeMask(39);
    float hairIblFallbackMask = MaterialDebugViewModeMask(40);
    float hairMsFallbackMask = MaterialDebugViewModeMask(41);
    float eyeFrameMask = MaterialDebugViewModeMask(42);
    float eyeCorneaNormalMask = MaterialDebugViewModeMask(43);
    float eyeIrisNormalMask = MaterialDebugViewModeMask(44);
    float eyeIrisPlaneNormalMask = MaterialDebugViewModeMask(45);
    float eyeFresnelMask = MaterialDebugViewModeMask(46);
    float eyeCorneaSpecularMask = MaterialDebugViewModeMask(47);
    float eyeRefractedViewMask = MaterialDebugViewModeMask(48);
    float eyeHitDistanceMask = MaterialDebugViewModeMask(49);
    float eyeUvMask = MaterialDebugViewModeMask(50);
    float eyeValidHitMask = MaterialDebugViewModeMask(51);
    float eyeIrisMask = MaterialDebugViewModeMask(52);
    float eyePupilMask = MaterialDebugViewModeMask(53);
    float eyeLimbusMask = MaterialDebugViewModeMask(54);
    float eyeTransmissionInMask = MaterialDebugViewModeMask(55);
    float eyeTransmissionOutMask = MaterialDebugViewModeMask(56);
    float eyeIrisDirectMask = MaterialDebugViewModeMask(57);
    float eyeScleraDirectMask = MaterialDebugViewModeMask(58);
    float eyeInnerIblMask = MaterialDebugViewModeMask(59);
    float eyeCausticMask = MaterialDebugViewModeMask(60);
    float eyeInnerShadowMask = MaterialDebugViewModeMask(61);
    float eyeCorneaShadowMask = MaterialDebugViewModeMask(62);
    float eyeProfileMask = MaterialDebugViewModeMask(63);
    float clothModelMask = MaterialDebugViewModeMask(64);
    float clothSheenColorMask = MaterialDebugViewModeMask(65);
    float clothSheenRoughnessMask = MaterialDebugViewModeMask(66);
    float clothCharlieDMask = MaterialDebugViewModeMask(67);
    float clothNeubeltVisibilityMask = MaterialDebugViewModeMask(68);
    float clothDirectionalAlbedoMask = MaterialDebugViewModeMask(69);
    float clothBaseEnergyScaleMask = MaterialDebugViewModeMask(70);
    float clothDirectSheenMask = MaterialDebugViewModeMask(71);
    float clothIndirectSheenMask = MaterialDebugViewModeMask(72);
    float clothIblFallbackMask = MaterialDebugViewModeMask(73);
    float clothV2ModelMask = MaterialDebugViewModeMask(80);
    float clothWorldTangentMask = MaterialDebugViewModeMask(81);
    float clothAnisotropyMask = MaterialDebugViewModeMask(82);
    float clothAnisotropyCrossMask = MaterialDebugViewModeMask(83);
    float clothRoughnessAxesMask = MaterialDebugViewModeMask(84);
    float clothAnisotropicCharlieDMask = MaterialDebugViewModeMask(85);
    float clothVisibilityMask = MaterialDebugViewModeMask(86);
    float clothV2DirectionalAlbedoMask = MaterialDebugViewModeMask(87);
    float clothV2BaseEnergyScaleMask = MaterialDebugViewModeMask(88);
    float clothV2IblFallbackMask = MaterialDebugViewModeMask(89);
    float skinDirectDiffuseMask = MaterialDebugViewModeMask(74);
    float skinTransmissionMask = MaterialDebugViewModeMask(75);
    float skinShadowMask = MaterialDebugViewModeMask(76);
    float skinIblDiffuseMask = MaterialDebugViewModeMask(77);
    float skinIblSpecularMask = MaterialDebugViewModeMask(78);
    float skinVirtualLightMask = MaterialDebugViewModeMask(79);
    float foliageModelMask = MaterialDebugViewModeMask(90);
    float foliageSubsurfaceColorMask = MaterialDebugViewModeMask(91);
    float foliageFrontFacingMask = MaterialDebugViewModeMask(92);
    float foliageNormalMask = MaterialDebugViewModeMask(93);
    float foliageBacklightFactorMask = MaterialDebugViewModeMask(94);
    float foliageShadowMask = MaterialDebugViewModeMask(95);
    float foliageLobeMask = MaterialDebugViewModeMask(96);
    float foliageAlphaMask = MaterialDebugViewModeMask(97);
    float foliageCustomDataMask = MaterialDebugViewModeMask(98);
    float foliageGBufferVersionMask = MaterialDebugViewModeMask(99);

    float subsurfaceAssetId = 0.0;
    if (surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN)
    {
        subsurfaceAssetId =
            surface.modelInputs.preintegratedSkin.skinLutId / 15.0;
    }
    else if (surface.shadingModel == SHADING_MODEL_SUBSURFACE_PROFILE)
    {
        subsurfaceAssetId =
            surface.modelInputs.subsurfaceProfile.profileId / 255.0;
    }

    float debugMask = min(
        baseColorMask +
        emissiveMask +
        normalMask +
        roughnessMask +
        metallicMask +
        aoMask +
        shadowMask +
        directLightingMask +
        indirectDiffuseMask +
        indirectSpecularMask +
        shadowCascadeMask +
        shadingModelMask +
        subsurfaceWeightMask +
        transmissionWeightMask +
        subsurfaceAssetIdMask +
        localSubsurfaceMask +
        diffuseBeforeSubsurfaceMask +
        hairFrameMask +
        hairTangentMask +
        hairThetaMask +
        hairDeltaPhiMask +
        hairRMask +
        hairTTMask +
        hairTRTMask +
        hairPathLengthMask +
        hairAbsorptionMask +
        hairCoverageMask +
        hairShadowMask +
        hairLutMask +
        hairPrimaryMask +
        hairSecondaryMask +
        hairScatterMask +
        hairBacklitMask +
        hairRPathColorMask +
        hairTTPathColorMask +
        hairTRTPathColorMask +
        hairIblFallbackMask +
        hairMsFallbackMask +
        eyeFrameMask +
        eyeCorneaNormalMask +
        eyeIrisNormalMask +
        eyeIrisPlaneNormalMask +
        eyeFresnelMask +
        eyeCorneaSpecularMask +
        eyeRefractedViewMask +
        eyeHitDistanceMask +
        eyeUvMask +
        eyeValidHitMask +
        eyeIrisMask +
        eyePupilMask +
        eyeLimbusMask +
        eyeTransmissionInMask +
        eyeTransmissionOutMask +
        eyeIrisDirectMask +
        eyeScleraDirectMask +
        eyeInnerIblMask +
        eyeCausticMask +
        eyeInnerShadowMask +
        eyeCorneaShadowMask +
        eyeProfileMask +
        clothModelMask +
        clothSheenColorMask +
        clothSheenRoughnessMask +
        clothCharlieDMask +
        clothNeubeltVisibilityMask +
        clothDirectionalAlbedoMask +
        clothBaseEnergyScaleMask +
        clothDirectSheenMask +
        clothIndirectSheenMask +
        clothIblFallbackMask +
        clothV2ModelMask +
        clothWorldTangentMask +
        clothAnisotropyMask +
        clothAnisotropyCrossMask +
        clothRoughnessAxesMask +
        clothAnisotropicCharlieDMask +
        clothVisibilityMask +
        clothV2DirectionalAlbedoMask +
        clothV2BaseEnergyScaleMask +
        clothV2IblFallbackMask +
         skinDirectDiffuseMask +
         skinTransmissionMask +
         skinShadowMask +
         skinIblDiffuseMask +
         skinIblSpecularMask +
         skinVirtualLightMask +
         foliageModelMask +
         foliageSubsurfaceColorMask +
         foliageFrontFacingMask +
         foliageNormalMask +
         foliageBacklightFactorMask +
         foliageShadowMask +
         foliageLobeMask +
         foliageAlphaMask +
         foliageCustomDataMask +
         foliageGBufferVersionMask,
        1.0);

    float clothModel = surface.shadingModel == SHADING_MODEL_CLOTH ? 1.0 : 0.0;
    float foliageModel = surface.shadingModel == SHADING_MODEL_TWOSIDED_FOLIAGE ? 1.0 : 0.0;
    vec4 debugColor =
        baseColorMask * vec4(surface.baseColor, 1.0) +
        emissiveMask * vec4(surface.emissiveColor, 1.0) +
        normalMask * vec4(surface.worldNormal * 0.5 + 0.5, 1.0) +
        roughnessMask * vec4(vec3(surface.roughness), 1.0) +
        metallicMask * vec4(vec3(surface.metallic), 1.0) +
        aoMask * vec4(vec3(surface.ambientOcclusion), 1.0) +
        shadowMask * vec4(vec3(lighting.shadow), 1.0) +
        directLightingMask * vec4(lighting.directLighting, 1.0) +
        indirectDiffuseMask * vec4(lighting.indirectDiffuse, 1.0) +
        indirectSpecularMask * vec4(lighting.indirectSpecular, 1.0) +
        shadowCascadeMask * vec4(vec3(lighting.shadowCascadeIndex), 1.0) +
        // ShadingModelID 占用 packed byte 的低 4 bit，按完整 nibble 映射到调试颜色。
        shadingModelMask * vec4(vec3(float(surface.shadingModel) / 15.0), 1.0) +
        subsurfaceWeightMask * vec4(vec3(lighting.subsurfaceWeight), 1.0) +
        transmissionWeightMask * vec4(vec3(lighting.transmissionWeight), 1.0) +
        subsurfaceAssetIdMask * vec4(vec3(subsurfaceAssetId), 1.0) +
        localSubsurfaceMask * vec4(lighting.localSubsurfaceLighting, 1.0) +
        diffuseBeforeSubsurfaceMask * vec4(lighting.diffuseBeforeSubsurface, 1.0)
         + skinDirectDiffuseMask * vec4(lighting.skinDirectDiffuse, 1.0)
         + skinTransmissionMask * vec4(lighting.skinTransmission, 1.0)
         + skinShadowMask * vec4(vec3(lighting.skinShadowVisibility), 1.0)
         + skinIblDiffuseMask * vec4(lighting.skinIblDiffuse, 1.0)
        + skinIblSpecularMask * vec4(lighting.skinIblSpecular, 1.0)
        + skinVirtualLightMask * vec4(lighting.skinVirtualLight, 1.0)
        + foliageModelMask * vec4(vec3(foliageModel), 1.0)
        + foliageSubsurfaceColorMask * vec4(lighting.foliageSubsurfaceColor, 1.0)
        + foliageFrontFacingMask * vec4(vec3(lighting.foliageFrontFacing), 1.0)
        + foliageNormalMask * vec4(lighting.foliageResolvedNormal * 0.5 + 0.5, 1.0)
        + foliageBacklightFactorMask * vec4(vec3(lighting.foliageBacklightFactor), 1.0)
        + foliageShadowMask * vec4(vec3(lighting.foliageShadowVisibility), 1.0)
        + foliageLobeMask * vec4(lighting.foliageBacklitDirect, 1.0)
        + foliageAlphaMask * vec4(vec3(lighting.foliageAlphaMask), 1.0)
        + foliageCustomDataMask * vec4(vec3(lighting.foliageGBufferCustomData), 1.0)
        + foliageGBufferVersionMask * vec4(vec3(lighting.foliageGBufferVersion), 1.0)
        + hairFrameMask * vec4(lighting.hairBitangent * 0.5 + 0.5, 1.0)
        + hairTangentMask * vec4(lighting.hairTangent * 0.5 + 0.5, 1.0)
        + hairThetaMask * vec4(vec3(
            lighting.hairThetaI / 3.14159265 + 0.5,
            lighting.hairThetaO / 3.14159265 + 0.5,
            lighting.hairThetaH / 3.14159265 + 0.5), 1.0)
        + hairDeltaPhiMask * vec4(vec3(
            lighting.hairDeltaPhi / (2.0 * 3.14159265) + 0.5), 1.0)
        + hairRMask * vec4(lighting.hairRPath, 1.0)
        + hairTTMask * vec4(lighting.hairTTPath, 1.0)
        + hairTRTMask * vec4(lighting.hairTRTPath, 1.0)
        + hairPathLengthMask * vec4(vec3(
            1.0 - exp(-lighting.hairPathLength * 1000.0)), 1.0)
        + hairAbsorptionMask * vec4(
            1.0 - exp(-lighting.hairAbsorption * 0.01), 1.0)
        + hairCoverageMask * vec4(vec3(lighting.hairCoverage), 1.0)
        + hairShadowMask * vec4(vec3(lighting.hairShadowTransmittance), 1.0)
        + hairLutMask * vec4(vec3(
            lighting.hairLutCoordinates, 0.0), 1.0)
        + hairPrimaryMask * vec4(lighting.hairRPath, 1.0)
        + hairSecondaryMask * vec4(lighting.hairTTPath + lighting.hairTRTPath, 1.0)
        + hairScatterMask * vec4(vec3(surface.modelInputs.hair.scatter), 1.0)
        + hairBacklitMask * vec4(vec3(surface.modelInputs.hair.backlit), 1.0)
        + hairRPathColorMask * vec4(lighting.hairRPath, 1.0)
        + hairTTPathColorMask * vec4(lighting.hairTTPath, 1.0)
        + hairTRTPathColorMask * vec4(lighting.hairTRTPath, 1.0)
        + hairIblFallbackMask * vec4(vec3(lighting.hairIblFallback), 1.0)
        + hairMsFallbackMask * vec4(vec3(lighting.hairMultipleScatteringFallback), 1.0)
        + eyeFrameMask * vec4(surface.modelInputs.eye.corneaNormal * 0.5 + 0.5, 1.0)
        + eyeCorneaNormalMask * vec4(surface.modelInputs.eye.corneaNormal * 0.5 + 0.5, 1.0)
        + eyeIrisNormalMask * vec4(surface.modelInputs.eye.irisNormal * 0.5 + 0.5, 1.0)
        + eyeIrisPlaneNormalMask * vec4(surface.modelInputs.eye.irisPlaneNormal * 0.5 + 0.5, 1.0)
        + eyeFresnelMask * vec4(vec3(lighting.eyeCorneaFresnel), 1.0)
        + eyeCorneaSpecularMask * vec4(lighting.eyeCorneaSpecular, 1.0)
        + eyeRefractedViewMask * vec4(lighting.eyeRefractedViewDirection * 0.5 + 0.5, 1.0)
        + eyeHitDistanceMask * vec4(vec3(1.0 - exp(-lighting.eyeIrisHitDistance * 1000.0)), 1.0)
        + eyeUvMask * vec4(lighting.eyeIrisUv, 0.0, 1.0)
        + eyeValidHitMask * vec4(vec3(lighting.eyeValidIrisHit), 1.0)
        + eyeIrisMask * vec4(vec3(lighting.eyeIrisMask), 1.0)
        + eyePupilMask * vec4(vec3(lighting.eyePupilMask), 1.0)
        + eyeLimbusMask * vec4(vec3(lighting.eyeLimbusMask), 1.0)
        + eyeTransmissionInMask * vec4(vec3(lighting.eyeTransmissionIn), 1.0)
        + eyeTransmissionOutMask * vec4(vec3(lighting.eyeTransmissionOut), 1.0)
        + eyeIrisDirectMask * vec4(lighting.eyeIrisDirect, 1.0)
        + eyeScleraDirectMask * vec4(lighting.eyeScleraDirect, 1.0)
        + eyeInnerIblMask * vec4(lighting.eyeInnerIbl, 1.0)
        + eyeCausticMask * vec4(vec3(lighting.eyeCausticGain), 1.0)
        + eyeInnerShadowMask * vec4(vec3(lighting.eyeShadowInner), 1.0)
        + eyeCorneaShadowMask * vec4(vec3(lighting.eyeShadowCornea), 1.0)
        + eyeProfileMask * vec4(vec3(surface.modelInputs.eye.causticProfileId / 15.0), 1.0)
        + clothModelMask * vec4(vec3(clothModel), 1.0)
        + clothSheenColorMask * vec4(lighting.clothSheenColor, 1.0)
        + clothSheenRoughnessMask * vec4(vec3(lighting.clothSheenRoughness), 1.0)
        + clothCharlieDMask * vec4(vec3(lighting.clothCharlieD), 1.0)
        + clothNeubeltVisibilityMask * vec4(vec3(lighting.clothVisibility), 1.0)
        + clothDirectionalAlbedoMask * vec4(vec3(lighting.clothDirectionalAlbedo), 1.0)
        + clothBaseEnergyScaleMask * vec4(lighting.clothBaseEnergyScale, 1.0)
        + clothDirectSheenMask * vec4(lighting.clothDirectSheen, 1.0)
        + clothIndirectSheenMask * vec4(lighting.clothIndirectSheen, 1.0)
        + clothIblFallbackMask * vec4(vec3(lighting.clothIblFallback), 1.0)
        + clothV2ModelMask * vec4(vec3(lighting.clothModelVersion / 2.0), 1.0)
        + clothWorldTangentMask * vec4(lighting.clothWorldTangent * 0.5 + 0.5, 1.0)
        + clothAnisotropyMask * vec4(vec3(lighting.clothAnisotropy * 0.5 + 0.5), 1.0)
        + clothAnisotropyCrossMask * vec4(vec3(lighting.clothAnisotropyCross), 1.0)
        + clothRoughnessAxesMask * vec4(lighting.clothRoughnessAxes, 0.0, 1.0)
        + clothAnisotropicCharlieDMask * vec4(vec3(lighting.clothCharlieD), 1.0)
        + clothVisibilityMask * vec4(vec3(lighting.clothVisibility), 1.0)
        + clothV2DirectionalAlbedoMask * vec4(vec3(lighting.clothDirectionalAlbedo), 1.0)
        + clothV2BaseEnergyScaleMask * vec4(lighting.clothBaseEnergyScale, 1.0)
        + clothV2IblFallbackMask * vec4(vec3(lighting.clothIblFallback), 1.0);

    return mix(defaultColor, debugColor, debugMask);
#else
    return defaultColor;
#endif
}

#endif

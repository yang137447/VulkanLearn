#ifndef VL_ENGINE_MATERIAL_DEBUG_VIEW_GLSL
#define VL_ENGINE_MATERIAL_DEBUG_VIEW_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"

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
    return data;
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
        eyeProfileMask,
        1.0);

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
        shadingModelMask * vec4(vec3(float(surface.shadingModel) / 10.0), 1.0) +
        subsurfaceWeightMask * vec4(vec3(lighting.subsurfaceWeight), 1.0) +
        transmissionWeightMask * vec4(vec3(lighting.transmissionWeight), 1.0) +
        subsurfaceAssetIdMask * vec4(vec3(subsurfaceAssetId), 1.0) +
        localSubsurfaceMask * vec4(lighting.localSubsurfaceLighting, 1.0) +
        diffuseBeforeSubsurfaceMask * vec4(lighting.diffuseBeforeSubsurface, 1.0)
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
        + eyeProfileMask * vec4(vec3(surface.modelInputs.eye.causticProfileId / 15.0), 1.0);

    return mix(defaultColor, debugColor, debugMask);
#else
    return defaultColor;
#endif
}

#endif

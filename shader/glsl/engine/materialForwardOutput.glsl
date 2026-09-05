#ifndef VL_ENGINE_MATERIAL_FORWARD_OUTPUT_GLSL
#define VL_ENGINE_MATERIAL_FORWARD_OUTPUT_GLSL

#include "materialSurface.glsl"
#include "forwardLighting.glsl"
#include "materialDebugView.glsl"

struct ThinTranslucentOutput
{
    // 双源路径分别写入 Add 和目标颜色乘数；降级路径会把 multiplier 压成 alpha。
    vec4 add;
    vec4 multiplier;
};

vec4 BuildMaterialForwardOutput(in MaterialSurface surface)
{
    ForwardLightingResult lighting = ShadeForwardSurfaceDetailed(surface);
    float resolvedOpacity = surface.opacity;
    // Hair MF 已按 NeoX mode 8 将 Tex0 alpha/clipValue 写入 surface.opacity；
    // coverage 不再在输出层重复乘入混合 alpha。
    vec3 resolvedLightingColor = lighting.finalColor;
#if defined(RENDER_MODE_FORWARD_EYE_INNER)
#if MATERIAL_IS_EYE
    // Inner shell 只写 tissue/iris ledger；cornea shell 随后以 additive composition 加入。
    resolvedLightingColor =
        lighting.eyeIrisDirect +
        lighting.eyeScleraDirect +
        lighting.eyeInnerIbl * surface.ambientOcclusion +
        surface.emissiveColor;
#endif
#elif defined(RENDER_MODE_FORWARD_EYE_CORNEA)
#if MATERIAL_IS_EYE
    // Cornea shell 不覆盖 inner shell，只输出顶层 reflection/highlight。
    resolvedLightingColor =
        lighting.eyeCorneaSpecular +
        lighting.indirectSpecular * surface.ambientOcclusion;
#endif
#endif
    vec4 color = vec4(resolvedLightingColor, resolvedOpacity);
#if defined(ENABLE_DEBUG_VIEW)
    // Skin 专项 Debug View 只观察 Deferred Skin；眼缘等前向材质不能覆盖 Skin
    // 快照，否则透明眼缘会把脸部调试结果误染成 DefaultLit 黑块。
    if (uboVP.debugViewMode >= 74 &&
        uboVP.debugViewMode <= 79 &&
        surface.shadingModel != SHADING_MODEL_PREINTEGRATED_SKIN)
    {
        return vec4(0.0);
    }
#endif
    MaterialDebugLightingData debugLighting = CreateMaterialDebugLightingData(
        lighting.shadow,
        lighting.shadowCascadeIndex,
        lighting.directLighting,
        lighting.indirectDiffuse,
        lighting.indirectSpecular);
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
    if (surface.shadingModel == SHADING_MODEL_TWOSIDED_FOLIAGE)
    {
        SetMaterialDebugFoliageData(
            debugLighting,
            surface.modelInputs.twoSidedFoliage.subsurfaceColor,
            surface.worldNormal,
            lighting.foliageBacklitDirect,
            lighting.foliageBacklightFactor,
            lighting.shadow,
            surface.opacityMask,
            1.0);
    }
    return ResolveMaterialDebugView(surface, debugLighting, color);
}

ThinTranslucentOutput BuildThinTranslucentForwardOutput(
    in MaterialSurface surface)
{
    // 目标公式：Framebuffer = Add + Multiplier * Destination。
    // 这里先完成光照分层，再把 RootOpacity 和 SurfaceCoverage 放到固定的合成位置。
    ForwardLightingResult lighting =
        ShadeThinTranslucentForwardSurface(surface);
    vec3 diffuseLighting =
        lighting.directDiffuse +
        lighting.indirectDiffuse * surface.ambientOcclusion;
    vec3 specularLighting =
        lighting.directSpecular +
        lighting.indirectSpecular * surface.ambientOcclusion;
    vec3 surfaceLighting =
        (diffuseLighting + surface.emissiveColor) * surface.opacity +
        specularLighting;

    MaterialDebugLightingData debugLighting = CreateMaterialDebugLightingData(
        lighting.shadow,
        lighting.shadowCascadeIndex,
        lighting.directLighting,
        lighting.indirectDiffuse,
        lighting.indirectSpecular);
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
    surfaceLighting = ResolveMaterialDebugView(
        surface,
        debugLighting,
        vec4(surfaceLighting, 1.0)).rgb;

    vec3 viewDirection = normalize(
        uboVP.cameraPosition - surface.worldPosition);
    float NdotV = clamp(
        abs(dot(normalize(surface.worldNormal), viewDirection)) + 1e-5,
        0.0,
        1.0);
    vec3 specularColor = mix(
        vec3(0.08 * surface.specular),
        surface.baseColor,
        surface.metallic);
    vec3 fresnel = FresnelSchlickUe(specularColor, NdotV);
    // UE Legacy Thin Translucent 只做零次内部往返：进入和离开界面各乘一次透射 Fresnel。
    vec3 interfaceTransmission = vec3(1.0) - fresnel;
    interfaceTransmission *= interfaceTransmission;
    vec3 absorption = pow(
        surface.transmittanceColor,
        vec3(1.0 / NdotV));
    vec3 legacyTransmission =
        interfaceTransmission * absorption * (1.0 - surface.opacity);

    ThinTranslucentOutput outputValue;
    // Add 承载 SurfaceCoverage * Surface，Multiplier 承载未覆盖区域与透射后的目标调制。
    outputValue.add = vec4(
        surface.surfaceCoverage * surfaceLighting,
        0.0);
    outputValue.multiplier = vec4(
        vec3(1.0 - surface.surfaceCoverage) +
            surface.surfaceCoverage * legacyTransmission,
        1.0);
    return outputValue;
}

vec4 BuildThinTranslucentFallbackOutput(
    in ThinTranslucentOutput thinOutput)
{
    // 普通单源 alpha blend 无法保存 RGB multiplier，只用三个通道的平均值近似目标衰减。
    float destinationMultiplier =
        dot(thinOutput.multiplier.rgb, vec3(1.0 / 3.0));
    return vec4(
        thinOutput.add.rgb,
        1.0 - destinationMultiplier);
}

#endif

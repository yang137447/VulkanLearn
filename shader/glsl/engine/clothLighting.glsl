#ifndef VL_ENGINE_CLOTH_LIGHTING_GLSL
#define VL_ENGINE_CLOTH_LIGHTING_GLSL

#include "materialSurface.glsl"
#include "../common/clothBrdf.glsl"
#include "../common/lighting.glsl"

struct ClothLightingResult
{
    vec3 directDiffuse;
    vec3 directSpecular;
    vec3 directSheen;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
    vec3 indirectSheen;
    vec3 baseEnergyScale;
    float directionalAlbedo;
    float shadow;
    float shadowCascadeIndex;
    float iblFallback;
    vec3 sheenColor;
    float sheenRoughness;
    float charlieD;
    float neubeltVisibility;
};

ClothLightingResult CreateDefaultClothLightingResult()
{
    ClothLightingResult result;
    result.directDiffuse = vec3(0.0);
    result.directSpecular = vec3(0.0);
    result.directSheen = vec3(0.0);
    result.indirectDiffuse = vec3(0.0);
    result.indirectSpecular = vec3(0.0);
    result.indirectSheen = vec3(0.0);
    result.baseEnergyScale = vec3(1.0);
    result.directionalAlbedo = 0.0;
    result.shadow = 1.0;
    result.shadowCascadeIndex = 0.0;
    result.iblFallback = 1.0;
    result.sheenColor = vec3(0.0);
    result.sheenRoughness = 0.0;
    result.charlieD = 0.0;
    result.neubeltVisibility = 0.0;
    return result;
}

float SampleClothDirectionalAlbedo(
    in sampler2D clothDirectionalAlbedoLut,
    float NdotV,
    float alpha)
{
    return texture(
        clothDirectionalAlbedoLut,
        vec2(NdotV, alpha)).r;
}

struct ClothDirectLighting
{
    vec3 diffuse;
    vec3 specular;
    vec3 sheen;
};

ClothDirectLighting CreateDefaultClothDirectLighting()
{
    ClothDirectLighting result;
    result.diffuse = vec3(0.0);
    result.specular = vec3(0.0);
    result.sheen = vec3(0.0);
    return result;
}

ClothDirectLighting AccumulateClothLight(
    in MaterialSurface surface,
    vec3 lightDirection_WS,
    vec3 radiance,
    in sampler2D clothDirectionalAlbedoLut)
{
    vec3 N = normalize(surface.worldNormal);
    vec3 L = normalize(lightDirection_WS);
    vec3 V = normalize(uboVP.cameraPosition - surface.worldPosition);
    vec3 H = normalize(L + V);
    float NdotL = dot(N, L);
    float NdotV = dot(N, V);
    ClothDirectLighting result = CreateDefaultClothDirectLighting();
    if (NdotL <= 0.0 || NdotV <= 0.0)
    {
        return result;
    }

    float alpha = ClothSheenRoughnessToAlpha(
        surface.modelInputs.cloth.sheenRoughness);
    float directionalAlbedo = SampleClothDirectionalAlbedo(
        clothDirectionalAlbedoLut,
        NdotV,
        alpha);
    vec3 baseEnergyScale = vec3(1.0) -
        surface.modelInputs.cloth.sheenColor * directionalAlbedo;
    LightingLobes baseLobes = EvaluateDefaultPbrLightLobes(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        0.0,
        L,
        radiance);
    float sheenResponse = ClothSheenUnitResponse(
        alpha,
        NdotL,
        NdotV,
        dot(N, H));
    result.diffuse = baseLobes.diffuse * baseEnergyScale;
    result.specular = baseLobes.specular * baseEnergyScale;
    result.sheen = surface.modelInputs.cloth.sheenColor *
        sheenResponse * radiance * NdotL;
    return result;
}

ClothDirectLighting CalculateClothDirectLighting(
    in MaterialSurface surface,
    in sampler2D clothDirectionalAlbedoLut)
{
    ClothDirectLighting lighting = CreateDefaultClothDirectLighting();
    int offset = uboLight.directionalLightOffset;
    int end = offset + uboLight.directionalLightCount;
    for (int i = offset; i < end; ++i)
    {
        Light light = uboLight.lights[i];
        ClothDirectLighting lobe = AccumulateClothLight(
            surface,
            normalize(-light.directionPad.xyz),
            light.colorIntensity.xyz * light.colorIntensity.w,
            clothDirectionalAlbedoLut);
        lighting.diffuse += lobe.diffuse;
        lighting.specular += lobe.specular;
        lighting.sheen += lobe.sheen;
    }

    offset = uboLight.pointLightOffset;
    end = offset + uboLight.pointLightCount;
    for (int i = offset; i < end; ++i)
    {
        Light light = uboLight.lights[i];
        vec3 offsetToLight = light.positionRadius.xyz - surface.worldPosition;
        float distance = length(offsetToLight);
        ClothDirectLighting lobe = AccumulateClothLight(
            surface,
            normalize(offsetToLight),
            light.colorIntensity.xyz * light.colorIntensity.w /
                (distance * distance + 1e-4),
            clothDirectionalAlbedoLut);
        lighting.diffuse += lobe.diffuse;
        lighting.specular += lobe.specular;
        lighting.sheen += lobe.sheen;
    }

    offset = uboLight.spotLightOffset;
    end = offset + uboLight.spotLightCount;
    for (int i = offset; i < end; ++i)
    {
        Light light = uboLight.lights[i];
        vec3 offsetToLight = light.positionRadius.xyz - surface.worldPosition;
        float distance = length(offsetToLight);
        vec3 lightDirection = normalize(offsetToLight);
        float lightAngle = acos(dot(lightDirection, -light.directionPad.xyz));
        float angleRange =
            light.coneAngleOuterInnerPadPad.y -
            light.coneAngleOuterInnerPadPad.x;
        float angleIntensity = clamp(
            (lightAngle - light.coneAngleOuterInnerPadPad.x) /
                angleRange,
            0.0,
            1.0);
        ClothDirectLighting lobe = AccumulateClothLight(
            surface,
            lightDirection,
            light.colorIntensity.xyz * light.colorIntensity.w *
                angleIntensity / (distance * distance + 1e-4),
            clothDirectionalAlbedoLut);
        lighting.diffuse += lobe.diffuse;
        lighting.specular += lobe.specular;
        lighting.sheen += lobe.sheen;
    }
    return lighting;
}

ClothLightingResult ShadeClothSurface(
    in MaterialSurface surface,
    in sampler2D clothDirectionalAlbedoLut,
    in sampler2DArrayShadow inputShadowMap)
{
    ClothLightingResult result = CreateDefaultClothLightingResult();
    vec3 viewDir = normalize(uboVP.cameraPosition - surface.worldPosition);
    float alpha = ClothSheenRoughnessToAlpha(
        surface.modelInputs.cloth.sheenRoughness);
    float NdotV = dot(normalize(surface.worldNormal), viewDir);
    if (NdotV <= 0.0)
    {
        return result;
    }
    result.directionalAlbedo = SampleClothDirectionalAlbedo(
        clothDirectionalAlbedoLut,
        NdotV,
        alpha);
    result.baseEnergyScale = vec3(1.0) -
        surface.modelInputs.cloth.sheenColor * result.directionalAlbedo;

    result.sheenColor = surface.modelInputs.cloth.sheenColor;
    result.sheenRoughness = surface.modelInputs.cloth.sheenRoughness;
    // Debug 值使用视线方向作为代表半角，避免把某一盏灯的结果误认为材质常量。
    vec3 debugHalf = normalize(normalize(surface.worldNormal) + viewDir);
    result.charlieD = ClothCharlieDistribution(
        alpha,
        dot(normalize(surface.worldNormal), debugHalf));
    result.neubeltVisibility = ClothNeubeltVisibility(NdotV, NdotV);

    ClothDirectLighting direct = CalculateClothDirectLighting(
        surface,
        clothDirectionalAlbedoLut);
    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        inputShadowMap,
        surface.worldPosition,
        surface.worldNormal,
        cascadeIndex);
    result.shadow *= surface.precomputedShadowFactors.r;
    result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    result.directDiffuse = direct.diffuse * result.shadow;
    result.directSpecular = direct.specular * result.shadow;
    result.directSheen = direct.sheen * result.shadow;

    result.indirectDiffuse = CalculateDiffuseIbl(
        surface.worldNormal,
        surface.baseColor,
        0.0) * result.baseEnergyScale;
    result.indirectSpecular = CalculateSpecularIbl(
        surface.worldNormal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        0.0) * result.baseEnergyScale;
    // sheenIblVersion=0：明确标记的低频 fallback，不复用 GGX split-sum sheen。
    result.indirectSheen = CalculateDiffuseIbl(
        surface.worldNormal,
        surface.modelInputs.cloth.sheenColor,
        0.0) * result.directionalAlbedo;
    return result;
}

#endif

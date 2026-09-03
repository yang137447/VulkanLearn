#ifndef VL_ENGINE_TWO_SIDED_FOLIAGE_LIGHTING_GLSL
#define VL_ENGINE_TWO_SIDED_FOLIAGE_LIGHTING_GLSL

#include "../common/lighting.glsl"

struct TwoSidedFoliageLightingResult
{
    LightingLobes baseLighting;
    vec3 backlitDirect;
    float backlightFactor;
    vec3 indirectDiffuse;
    vec3 indirectSpecular;
};

TwoSidedFoliageLightingResult CreateDefaultTwoSidedFoliageLightingResult()
{
    TwoSidedFoliageLightingResult result;
    result.baseLighting = CreateLightingLobes();
    result.backlitDirect = vec3(0.0);
    result.backlightFactor = 0.0;
    result.indirectDiffuse = vec3(0.0);
    result.indirectSpecular = vec3(0.0);
    return result;
}

vec3 EvaluateTwoSidedFoliageBacklight(
    in vec3 normal_WS,
    in vec3 lightDirection_WS,
    in vec3 radiance,
    in vec3 subsurfaceColor)
{
    // MVP foliage closure：背光项只由 -N 与入射光方向的余弦驱动，
    // 不把 BaseColor 改写成透光颜色，也不引入未版本化的厚度/LUT参数。
    float backlight = max(
        dot(-normalize(normal_WS), normalize(lightDirection_WS)),
        0.0);
    return radiance * backlight * subsurfaceColor;
}

float EvaluateTwoSidedFoliageBacklightFactor(
    in vec3 normal_WS,
    in vec3 lightDirection_WS)
{
    return max(
        dot(-normalize(normal_WS), normalize(lightDirection_WS)),
        0.0);
}

TwoSidedFoliageLightingResult ShadeTwoSidedFoliageSurface(
    in MaterialSurface surface)
{
    TwoSidedFoliageLightingResult result =
        CreateDefaultTwoSidedFoliageLightingResult();
    // shading normal 服务于当前可见面；背光法线由同一份面向快照还原几何面，
    // 避免把 gl_FrontFacing 的局部翻转误当成 foliage 透光模型本身。
    vec3 backlightNormal = surface.foliageFrontFacing > 0.5
        ? surface.worldNormal
        : -surface.worldNormal;
    result.baseLighting = CalculateDirectLightingLobes(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic);
    result.indirectDiffuse = CalculateDiffuseIbl(
        surface.worldNormal,
        surface.baseColor,
        surface.metallic);
    result.indirectSpecular = CalculateSpecularIbl(
        surface.worldNormal,
        normalize(uboVP.cameraPosition - surface.worldPosition),
        surface.baseColor,
        surface.roughness,
        surface.metallic);

    int offset = uboLight.directionalLightOffset;
    int end = offset + uboLight.directionalLightCount;
    for (int i = offset; i < end; ++i)
    {
        Light light = uboLight.lights[i];
        result.backlightFactor = max(
            result.backlightFactor,
            EvaluateTwoSidedFoliageBacklightFactor(
                backlightNormal,
                normalize(-light.directionPad.xyz)));
        result.backlitDirect += EvaluateTwoSidedFoliageBacklight(
            backlightNormal,
            normalize(-light.directionPad.xyz),
            light.colorIntensity.xyz * light.colorIntensity.w,
            surface.modelInputs.twoSidedFoliage.subsurfaceColor);
    }

    offset = uboLight.pointLightOffset;
    end = offset + uboLight.pointLightCount;
    for (int i = offset; i < end; ++i)
    {
        Light light = uboLight.lights[i];
        vec3 lightOffset = light.positionRadius.xyz - surface.worldPosition;
        float distance = length(lightOffset);
        result.backlightFactor = max(
            result.backlightFactor,
            EvaluateTwoSidedFoliageBacklightFactor(
                backlightNormal,
                normalize(lightOffset)));
        result.backlitDirect += EvaluateTwoSidedFoliageBacklight(
            backlightNormal,
            normalize(lightOffset),
            light.colorIntensity.xyz * light.colorIntensity.w /
                (distance * distance + 1e-4),
            surface.modelInputs.twoSidedFoliage.subsurfaceColor);
    }

    offset = uboLight.spotLightOffset;
    end = offset + uboLight.spotLightCount;
    for (int i = offset; i < end; ++i)
    {
        Light light = uboLight.lights[i];
        vec3 lightOffset = light.positionRadius.xyz - surface.worldPosition;
        vec3 lightDirection = normalize(lightOffset);
        float lightAngle = acos(dot(
            lightDirection,
            -light.directionPad.xyz));
        float angleRange =
            light.coneAngleOuterInnerPadPad.y -
            light.coneAngleOuterInnerPadPad.x;
        float angleIntensity = clamp(
            (lightAngle - light.coneAngleOuterInnerPadPad.x) /
                angleRange,
            0.0,
            1.0);
        float distance = length(lightOffset);
        result.backlightFactor = max(
            result.backlightFactor,
            EvaluateTwoSidedFoliageBacklightFactor(
                backlightNormal,
                lightDirection) * angleIntensity);
        result.backlitDirect += EvaluateTwoSidedFoliageBacklight(
            backlightNormal,
            lightDirection,
            angleIntensity * light.colorIntensity.xyz *
                light.colorIntensity.w /
                (distance * distance + 1e-4),
            surface.modelInputs.twoSidedFoliage.subsurfaceColor);
    }
    return result;
}

#endif

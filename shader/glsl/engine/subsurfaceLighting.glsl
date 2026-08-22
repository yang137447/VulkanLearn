#ifndef VL_ENGINE_SUBSURFACE_LIGHTING_GLSL
#define VL_ENGINE_SUBSURFACE_LIGHTING_GLSL

#include "../common/lighting.glsl"
#include "materialSurface.glsl"

struct SubsurfaceLocalLighting
{
    vec3 diffuse;
    vec3 transmission;
};

SubsurfaceLocalLighting CreateSubsurfaceLocalLighting()
{
    SubsurfaceLocalLighting lighting;
    lighting.diffuse = vec3(0.0);
    lighting.transmission = vec3(0.0);
    return lighting;
}

void AccumulateSubsurfaceLight(
    in MaterialSurface surface,
    in vec3 lightDirection,
    in vec3 radiance,
    inout SubsurfaceLocalLighting lighting)
{
    // ID 2 是当前像素的局部 closure：wrap/backscatter 只改变 diffuse，
    // 不接入 ID 5 的邻域 profile filter。
    vec3 normal = normalize(surface.worldNormal);
    float nDotL = dot(normal, lightDirection);
    float wrappedNoL = max(
        (nDotL + surface.modelInputs.subsurface.wrapWidth) /
            (1.0 + surface.modelInputs.subsurface.wrapWidth),
        0.0);
    float backNoL = max(dot(-normal, lightDirection), 0.0);
    float backscatter =
        pow(
            backNoL,
            surface.modelInputs.subsurface.backscatterPower) *
        surface.modelInputs.subsurface.backscatterWeight;
    vec3 diffuseColor =
        surface.baseColor *
        (1.0 - surface.metallic) *
        surface.modelInputs.subsurface.color;
    lighting.diffuse +=
        diffuseColor * radiance *
        (wrappedNoL + backscatter) / PI;

    // transmission 单独输出，最终由 deferred lighting 按显式权重预留能量。
    float thicknessTransmission = exp(
        -surface.modelInputs.subsurface.thickness * 100.0);
    lighting.transmission +=
        surface.baseColor *
        surface.modelInputs.subsurface.color *
        radiance * backscatter * thicknessTransmission;
}

SubsurfaceLocalLighting CalculateSubsurfaceLocalDirectLighting(
    in MaterialSurface surface)
{
    SubsurfaceLocalLighting lighting =
        CreateSubsurfaceLocalLighting();
    int offset = uboLight.directionalLightOffset;
    int end = offset + uboLight.directionalLightCount;
    for (int lightIndex = offset; lightIndex < end; ++lightIndex)
    {
        Light light = uboLight.lights[lightIndex];
        AccumulateSubsurfaceLight(
            surface,
            normalize(-light.directionPad.xyz),
            light.colorIntensity.xyz * light.colorIntensity.w,
            lighting);
    }

    offset = uboLight.pointLightOffset;
    end = offset + uboLight.pointLightCount;
    for (int lightIndex = offset; lightIndex < end; ++lightIndex)
    {
        Light light = uboLight.lights[lightIndex];
        vec3 lightOffset = light.positionRadius.xyz - surface.worldPosition;
        float distance = length(lightOffset);
        vec3 radiance =
            light.colorIntensity.xyz * light.colorIntensity.w /
            (distance * distance + 1e-4);
        AccumulateSubsurfaceLight(
            surface,
            normalize(lightOffset),
            radiance,
            lighting);
    }

    offset = uboLight.spotLightOffset;
    end = offset + uboLight.spotLightCount;
    for (int lightIndex = offset; lightIndex < end; ++lightIndex)
    {
        Light light = uboLight.lights[lightIndex];
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
        vec3 radiance =
            light.colorIntensity.xyz * light.colorIntensity.w *
            angleIntensity /
            (distance * distance + 1e-4);
        AccumulateSubsurfaceLight(
            surface,
            lightDirection,
            radiance,
            lighting);
    }
    return lighting;
}

vec3 CalculateSubsurfaceLocalIndirectDiffuse(
    in MaterialSurface surface)
{
    return CalculateDiffuseIbl(
        surface.worldNormal,
        surface.baseColor,
        surface.metallic) *
        surface.modelInputs.subsurface.color;
}

#endif

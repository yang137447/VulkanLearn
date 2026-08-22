#ifndef VL_ENGINE_SUBSURFACE_PROFILE_LIGHTING_GLSL
#define VL_ENGINE_SUBSURFACE_PROFILE_LIGHTING_GLSL

#include "../common/lighting.glsl"
#include "materialSurface.glsl"

vec4 ReadSubsurfaceProfileMetadata(
    in sampler2D profileTable,
    int profileId)
{
    return texelFetch(profileTable, ivec2(0, profileId), 0);
}

vec3 EvaluateProfileTransmissionLight(
    in MaterialSurface surface,
    in vec4 metadata,
    in vec3 lightDirection,
    in vec3 radiance)
{
    // ID 5 的 transmission 使用 profile metadata，但不进入 screen-space diffuse blur。
    float backNoL = max(
        dot(-normalize(surface.worldNormal), lightDirection),
        0.0);
    float normalizedThickness =
        surface.modelInputs.subsurfaceProfile.thickness /
        metadata.w;
    return
        surface.baseColor * metadata.rgb * radiance * backNoL *
        exp(-3.0 * normalizedThickness);
}

vec3 CalculateSubsurfaceProfileTransmission(
    in MaterialSurface surface,
    in sampler2D profileTable)
{
    int profileId = int(
        surface.modelInputs.subsurfaceProfile.profileId + 0.5);
    vec4 metadata = ReadSubsurfaceProfileMetadata(
        profileTable,
        profileId);
    vec3 transmission = vec3(0.0);

    int offset = uboLight.directionalLightOffset;
    int end = offset + uboLight.directionalLightCount;
    for (int lightIndex = offset; lightIndex < end; ++lightIndex)
    {
        Light light = uboLight.lights[lightIndex];
        transmission += EvaluateProfileTransmissionLight(
            surface,
            metadata,
            normalize(-light.directionPad.xyz),
            light.colorIntensity.xyz * light.colorIntensity.w);
    }

    offset = uboLight.pointLightOffset;
    end = offset + uboLight.pointLightCount;
    for (int lightIndex = offset; lightIndex < end; ++lightIndex)
    {
        Light light = uboLight.lights[lightIndex];
        vec3 lightOffset = light.positionRadius.xyz - surface.worldPosition;
        float distance = length(lightOffset);
        transmission += EvaluateProfileTransmissionLight(
            surface,
            metadata,
            normalize(lightOffset),
            light.colorIntensity.xyz * light.colorIntensity.w /
                (distance * distance + 1e-4));
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
        transmission += EvaluateProfileTransmissionLight(
            surface,
            metadata,
            lightDirection,
            light.colorIntensity.xyz * light.colorIntensity.w *
                angleIntensity /
                (distance * distance + 1e-4));
    }
    return transmission;
}

#endif

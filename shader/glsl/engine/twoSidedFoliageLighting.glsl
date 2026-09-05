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

float EvaluateTwoSidedFoliageTransmissionFactor(
    in vec3 normal_WS,
    in vec3 viewDirection_WS,
    in vec3 lightDirection_WS)
{
    // 对齐 UE Legacy TwoSidedBxDF：固定 Wrap=0.5，并用 -V·L 驱动
    // roughness=0.6 的 GGX scatter。D_GGX 已经包含 1/PI。
    vec3 normal = normalize(normal_WS);
    vec3 viewDirection = normalize(viewDirection_WS);
    vec3 lightDirection = normalize(lightDirection_WS);
    const float wrap = 0.5;
    float wrapNoL = clamp(
        (-dot(normal, lightDirection) + wrap) /
            ((1.0 + wrap) * (1.0 + wrap)),
        0.0,
        1.0);
    float scatterArgument = clamp(
        -dot(viewDirection, lightDirection),
        0.0,
        1.0);
    const float scatterAlphaSquared = 0.6 * 0.6;
    float scatterDenominator =
        (scatterArgument * scatterAlphaSquared - scatterArgument) *
            scatterArgument + 1.0;
    float scatter = scatterAlphaSquared /
        (PI * scatterDenominator * scatterDenominator);
    return wrapNoL * scatter;
}

vec3 EvaluateTwoSidedFoliageBacklight(
    in vec3 normal_WS,
    in vec3 viewDirection_WS,
    in vec3 lightDirection_WS,
    in vec3 radiance,
    in vec3 subsurfaceColor)
{
    return radiance *
        EvaluateTwoSidedFoliageTransmissionFactor(
            normal_WS,
            viewDirection_WS,
            lightDirection_WS) *
        subsurfaceColor;
}

TwoSidedFoliageLightingResult ShadeTwoSidedFoliageSurface(
    in MaterialSurface surface)
{
    TwoSidedFoliageLightingResult result =
        CreateDefaultTwoSidedFoliageLightingResult();
    // 背光项以 surface.worldNormal（已由 MATERIAL_TWO_SIDED 翻到当前可见面，即 camera-facing）
    // 为基准，EvaluateTwoSidedFoliageBacklight 内部再做 -N·L。这样在“光源位于叶片背面、正对
    // 观察者”时透光最强，正反面响应对称。若把 gl_FrontFacing 翻转还原成几何法线，背面片元在
    // 经典背光场景下会得到 dot(-N_geom,L)<=0，导致透光为 0、背面发黑，与 UE 双面透光表现不一致。
    vec3 backlightNormal = surface.worldNormal;
    vec3 viewDirection = normalize(
        uboVP.cameraPosition - surface.worldPosition);
    result.baseLighting = CalculateDirectLightingLobes(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.specular);
    result.indirectDiffuse = CalculateDiffuseIbl(
        surface.worldNormal,
        surface.baseColor,
        surface.metallic);
    result.indirectSpecular = CalculateSpecularIbl(
        surface.worldNormal,
        normalize(uboVP.cameraPosition - surface.worldPosition),
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.specular);

    int offset = uboLight.directionalLightOffset;
    int end = offset + uboLight.directionalLightCount;
    for (int i = offset; i < end; ++i)
    {
        Light light = uboLight.lights[i];
        result.backlightFactor = max(
            result.backlightFactor,
            EvaluateTwoSidedFoliageTransmissionFactor(
                backlightNormal,
                viewDirection,
                normalize(-light.directionPad.xyz)));
        result.backlitDirect += EvaluateTwoSidedFoliageBacklight(
            backlightNormal,
            viewDirection,
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
            EvaluateTwoSidedFoliageTransmissionFactor(
                backlightNormal,
                viewDirection,
                normalize(lightOffset)));
        result.backlitDirect += EvaluateTwoSidedFoliageBacklight(
            backlightNormal,
            viewDirection,
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
            EvaluateTwoSidedFoliageTransmissionFactor(
                backlightNormal,
                viewDirection,
                lightDirection) * angleIntensity);
        result.backlitDirect += EvaluateTwoSidedFoliageBacklight(
            backlightNormal,
            viewDirection,
            lightDirection,
            angleIntensity * light.colorIntensity.xyz *
                light.colorIntensity.w /
                (distance * distance + 1e-4),
            surface.modelInputs.twoSidedFoliage.subsurfaceColor);
    }
    return result;
}

#endif

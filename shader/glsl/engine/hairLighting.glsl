#ifndef VL_ENGINE_HAIR_LIGHTING_GLSL
#define VL_ENGINE_HAIR_LIGHTING_GLSL

#include "hairScattering.glsl"
#include "../common/lighting.glsl"

struct HairLightingResult
{
    vec3 directLighting;
    vec3 directR;
    vec3 directTT;
    vec3 directTRT;
    vec3 indirectR;
    vec3 indirectTT;
    vec3 indirectTRT;
    vec3 multipleScattering;
    float shadow;
    float shadowCascadeIndex;
    float pathLength;
    vec3 absorption;
    vec2 lutCoordinates;
    float hairIblFallback;
    float multipleScatteringFallback;
    vec3 tangent;
    vec3 bitangent;
    float thetaI;
    float thetaO;
    float thetaH;
    float thetaD;
    float deltaPhi;
    float coverage;
    float density;
    float shadowTransmittance;
    vec3 finalColor;
};

HairLightingResult CreateDefaultHairLightingResult()
{
    HairLightingResult result;
    result.directLighting = vec3(0.0);
    result.directR = vec3(0.0);
    result.directTT = vec3(0.0);
    result.directTRT = vec3(0.0);
    result.indirectR = vec3(0.0);
    result.indirectTT = vec3(0.0);
    result.indirectTRT = vec3(0.0);
    result.multipleScattering = vec3(0.0);
    result.shadow = 1.0;
    result.shadowCascadeIndex = 0.0;
    result.pathLength = 0.0;
    result.absorption = vec3(0.0);
    result.lutCoordinates = vec2(0.0);
    result.hairIblFallback = 1.0;
    result.multipleScatteringFallback = 1.0;
    result.tangent = vec3(1.0, 0.0, 0.0);
    result.bitangent = vec3(0.0, 1.0, 0.0);
    result.thetaI = 0.0;
    result.thetaO = 0.0;
    result.thetaH = 0.0;
    result.thetaD = 0.0;
    result.deltaPhi = 0.0;
    result.coverage = 1.0;
    result.density = 1.0;
    result.shadowTransmittance = 1.0;
    result.finalColor = vec3(0.0);
    return result;
}

void AccumulateHairLightPath(
    in MaterialSurface surface,
    in HairAngles angles,
    in HairMaterialInputs hair,
    in HairTangentFrame frame,
    vec3 lightDirection,
    vec3 radiance,
    float visibility,
    inout HairLightingResult result)
{
    vec3 viewDirection = normalize(uboVP.cameraPosition - surface.worldPosition);
    float crossSection = abs(dot(frame.normal, lightDirection));
    float backLight = max(dot(frame.normal, -lightDirection), 0.0);
    HairAngles lightAngles = angles;
    result.thetaI = lightAngles.thetaI;
    result.thetaO = lightAngles.thetaO;
    result.thetaH = lightAngles.thetaH;
    result.thetaD = lightAngles.thetaD;
    result.deltaPhi = lightAngles.deltaPhi;

    for (uint path = HAIR_PATH_R; path <= HAIR_PATH_TRT; ++path)
    {
        HairAzimuthalSample azimuthal = SampleHairAzimuthalLut(
            lightAngles,
            hair,
            path);
        float pathWeight = EvaluateHairPathWeight(
            path,
            lightAngles,
            hair,
            azimuthal);
        vec3 transmittance = EvaluateHairPathTransmittance(
            path,
            azimuthal,
            hair);
        if (path == HAIR_PATH_TT)
        {
            // Backlit 只改变 TT body response，不改变 R 或 absorption 本身。
            pathWeight *= 1.0 + hair.backlit * backLight;
        }
        vec3 contribution =
            radiance * crossSection * pathWeight * transmittance *
            surface.specular * visibility;
        if (path == HAIR_PATH_R)
        {
            result.directR += contribution;
        }
        else if (path == HAIR_PATH_TT)
        {
            result.directTT += contribution;
        }
        else
        {
            result.directTRT += contribution;
        }
        result.pathLength += azimuthal.pathLength / 3.0;
        result.lutCoordinates = azimuthal.coordinates;
    }
    result.absorption = hair.absorption;
}

HairLightingResult EvaluateHairDirectLighting(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    float precomputedShadowFactor)
{
    HairLightingResult result = CreateDefaultHairLightingResult();
    HairTangentFrame frame = BuildHairTangentFrame(surface);
    HairMaterialInputs hair = surface.modelInputs.hair;
    result.tangent = frame.tangent;
    result.bitangent = frame.bitangent;
    result.coverage = hair.coverage;
    result.density = hair.density;
    vec3 viewDirection = normalize(uboVP.cameraPosition - surface.worldPosition);

    // 当前只有 directional 使用 CSM；point/spot 的 visibility 保持各自路径的
    // 显式默认值 1，不能把 directional shadow 错套到局部光源。
    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        inputShadowMap,
        surface.worldPosition,
        surface.worldNormal,
        cascadeIndex) *
        precomputedShadowFactor;
    result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    result.shadowTransmittance = result.shadow;

    int offset = uboLight.directionalLightOffset;
    int end = offset + uboLight.directionalLightCount;
    for (int index = offset; index < end; ++index)
    {
        Light light = uboLight.lights[index];
        vec3 lightDirection = normalize(-light.directionPad.xyz);
        vec3 radiance = light.colorIntensity.xyz * light.colorIntensity.w;
        AccumulateHairLightPath(
            surface,
            ComputeHairAngles(frame, lightDirection, viewDirection),
            hair,
            frame,
            lightDirection,
            radiance,
            result.shadow,
            result);
    }

    offset = uboLight.pointLightOffset;
    end = offset + uboLight.pointLightCount;
    for (int index = offset; index < end; ++index)
    {
        Light light = uboLight.lights[index];
        vec3 lightOffset = light.positionRadius.xyz - surface.worldPosition;
        float distance = length(lightOffset);
        vec3 lightDirection = normalize(lightOffset);
        vec3 radiance =
            light.colorIntensity.xyz * light.colorIntensity.w /
            (distance * distance + 1.0e-4);
        AccumulateHairLightPath(
            surface,
            ComputeHairAngles(frame, lightDirection, viewDirection),
            hair,
            frame,
            lightDirection,
            radiance,
            1.0,
            result);
    }

    offset = uboLight.spotLightOffset;
    end = offset + uboLight.spotLightCount;
    for (int index = offset; index < end; ++index)
    {
        Light light = uboLight.lights[index];
        vec3 lightOffset = light.positionRadius.xyz - surface.worldPosition;
        float distance = length(lightOffset);
        vec3 lightDirection = normalize(lightOffset);
        float spotAngle = acos(dot(lightDirection, -light.directionPad.xyz));
        float angleRange =
            light.coneAngleOuterInnerPadPad.y -
            light.coneAngleOuterInnerPadPad.x;
        float angleIntensity = clamp(
            (spotAngle - light.coneAngleOuterInnerPadPad.x) /
                angleRange,
            0.0,
            1.0);
        vec3 radiance =
            light.colorIntensity.xyz * light.colorIntensity.w * angleIntensity /
            (distance * distance + 1.0e-4);
        AccumulateHairLightPath(
            surface,
            ComputeHairAngles(frame, lightDirection, viewDirection),
            hair,
            frame,
            lightDirection,
            radiance,
            1.0,
            result);
    }

    result.directLighting =
        result.directR + result.directTT + result.directTRT;
    return result;
}
HairLightingResult EvaluateHairIndirectLighting(
    in MaterialSurface surface,
    inout HairLightingResult result)
{
    vec3 viewDirection = normalize(uboVP.cameraPosition - surface.worldPosition);
    HairTangentFrame frame = BuildHairTangentFrame(surface);
    HairVisibilityInputs visibility = BuildHairVisibilityInputs(
        surface.modelInputs.hair);
    float f0 = EvaluateHairDielectricFresnel(
        abs(dot(normalize(surface.worldNormal), viewDirection)),
        surface.modelInputs.hair.ior);
    float rDirectionBasis =
        0.5 + 0.5 * abs(dot(frame.tangent, viewDirection));
    float indirectVisibility =
        visibility.selfShadow * visibility.transmittance;
    // R 使用低频 tangent-direction basis；TT/TRT 没有对应环境 basis 时保持零，
    // 并通过 fallback 标记告知 Debug/验收矩阵，不能把普通 GGX 反射冒充完整 Hair IBL。
    result.indirectR = CalculateSpecularIblWithF0(
        surface.worldNormal,
        viewDirection,
        vec3(f0 * surface.specular),
        surface.modelInputs.hair.azimuthalRoughness) *
        rDirectionBasis *
        indirectVisibility;
    // TT/TRT 环境 basis 尚未提供，显式保持为零并通过 debug 标记缺失路径。
    result.indirectTT = vec3(0.0);
    result.indirectTRT = vec3(0.0);
    result.hairIblFallback = 1.0;

    float singleScattering =
        min(dot(result.directLighting, vec3(0.3333333)), 1.0);
    float remainingEnergy = max(1.0 - singleScattering, 0.0);
    float visibilityBudget = EvaluateHairVisibility(visibility) * visibility.density;
    float msWeight = min(
        remainingEnergy * visibilityBudget,
        surface.modelInputs.hair.multipleScatteringWeight *
            surface.modelInputs.hair.scatter);
    result.multipleScattering =
        CalculateDiffuseIbl(
            surface.worldNormal,
            vec3(1.0),
            0.0) *
        msWeight;
    result.multipleScatteringFallback = 1.0;
    result.finalColor =
        surface.emissiveColor +
        result.directLighting +
        (result.indirectR + result.indirectTT + result.indirectTRT +
            result.multipleScattering) *
            surface.ambientOcclusion;
    return result;
}

HairLightingResult ShadeHairSurface(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap,
    float precomputedShadowFactor)
{
    HairLightingResult result = EvaluateHairDirectLighting(
        surface,
        inputShadowMap,
        precomputedShadowFactor);
    return EvaluateHairIndirectLighting(surface, result);
}

HairLightingResult ShadeHairSurface(
    in MaterialSurface surface,
    in sampler2DArrayShadow inputShadowMap)
{
    return ShadeHairSurface(surface, inputShadowMap, 1.0);
}

#endif
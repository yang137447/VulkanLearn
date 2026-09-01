#ifndef VL_ENGINE_HAIR_LIGHTING_GLSL
#define VL_ENGINE_HAIR_LIGHTING_GLSL

#include "hairScattering.glsl"
#include "../common/lighting.glsl"
#include "virtualLight.glsl"

struct HairLightingResult
{
    vec3 directLighting;
    vec3 directR;
    vec3 directTT;
    vec3 directTRT;
    vec3 directScatter;
    vec3 indirectR;
    vec3 indirectTT;
    vec3 indirectTRT;
    vec3 indirectScatter;
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
    result.directScatter = vec3(0.0);
    result.indirectR = vec3(0.0);
    result.indirectTT = vec3(0.0);
    result.indirectTRT = vec3(0.0);
    result.indirectScatter = vec3(0.0);
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

float EvaluateNeoXHairSpecular(
    in MaterialSurface surface,
    in HairTangentFrame frame,
    vec3 viewDirection)
{
    // 源 CalcSpecularAmbientOcclusionHair 在材质求值末尾修改 specular，
    // 因而 Direct 与 Ambient 的 R 路径必须消费同一结果。
    float noVFullrange = dot(frame.geometricNormal, viewDirection);
    float specularOcclusionMask = clamp(
        noVFullrange * noVFullrange - 0.3,
        0.0,
        1.0);
    return surface.specular * mix(
        surface.ambientOcclusion,
        1.0,
        specularOcclusionMask);
}

float EvaluateNeoXHairDirectionalVisibility(
    in MaterialSurface surface,
    in HairTangentFrame frame,
    vec3 lightDirection,
    float shadow)
{
    // 源方向光在 CSM 后额外乘几何法线半兰伯特和半强度 Material AO。
    float normalShadow = dot(lightDirection, frame.geometricNormal) * 0.5 + 0.5;
    float ambientOcclusionShadow = surface.ambientOcclusion * 0.5 + 0.5;
    return shadow * normalShadow * ambientOcclusionShadow;
}

float EvaluateNeoXHairLocalVisibility(
    in HairTangentFrame frame,
    vec3 lightDirection)
{
    // NeoX 的 point/spot light 先用卡片几何法线抑制背面漏光；Virtual Light
    // 使用共享模块，但仍保留 Hair 自己的几何可见性规则。
    return max(dot(lightDirection, frame.geometricNormal), 0.0);
}

void AccumulateHairLightPath(
    in MaterialSurface surface,
    in HairAngles angles,
    in HairMaterialInputs hair,
    in HairTangentFrame frame,
    vec3 lightDirection,
    vec3 radiance,
    float scatteringShadow,
    float lightVisibility,
    inout HairLightingResult result)
{
    vec3 viewDirection = normalize(uboVP.cameraPosition - surface.worldPosition);
    HairAngles lightAngles = angles;
    result.thetaI = lightAngles.thetaI;
    result.thetaO = lightAngles.thetaO;
    result.thetaH = lightAngles.thetaH;
    result.thetaD = lightAngles.thetaD;
    result.deltaPhi = lightAngles.deltaPhi;

    HairUeScatteringContext ueContext = BuildHairUeScatteringContext(
        frame.tangent,
        lightDirection,
        viewDirection);
    vec3 pathColor = EvaluateHairUePathColor(hair);
    float directVisibility = lightVisibility * hair.coverage;
    vec3 scatterColorResponse = EvaluateHairBaseColorResponse(
        surface,
        scatteringShadow);
    float hairSpecular = EvaluateNeoXHairSpecular(
        surface,
        frame,
        viewDirection);
    // NeoX HairBxDF 直接输出 PI * HairShading；VL 的 radiance 是原始灯光
    // 辐亮度，1/PI 只存在于 Default Lit BRDF 内部，不能用经验 0.5 替代。
    float sourceEnergyScale = HAIR_PI;

    // 角色路径对齐 UE 的实时 R/TT/TRT 近似；Reference LUT 保留给离线验证与
    // Debug View，不能再把求根焦散峰直接当作 UE 生产 ShadingModel 的能量。
    // 各路径已经按当前 Vulkan 光源 radiance 合同归一化；不能把源
    // forward 的最终 2*pi 再整体乘到这里，否则发色会被冲白。
    result.directR += radiance *
        EvaluateHairUeR(
            ueContext,
            hair,
            hairSpecular,
            0.0) *
        sourceEnergyScale *
        directVisibility;
    result.directTT += radiance *
        EvaluateHairUeTT(
            ueContext,
            hair,
            pathColor,
            0.0) *
        sourceEnergyScale *
        directVisibility;
    result.directTRT += radiance *
        EvaluateHairUeTRT(
            ueContext,
            hair,
            pathColor,
            0.0) *
        sourceEnergyScale *
        directVisibility;
    result.directScatter += radiance *
        (scatterColorResponse * EvaluateHairUeScatterBase(
            ueContext,
            hair,
            frame.tangent,
            frame.geometricNormal,
            lightDirection,
            viewDirection)) *
        sourceEnergyScale *
        directVisibility;

    if (uboVP.debugViewMode == 28 || uboVP.debugViewMode == 32)
    {
        // Reference LUT 只在 PathLength/LUTCoordinates 调试时采样；正常角色光照不支付这三次纹理读取。
        for (uint path = HAIR_PATH_R; path <= HAIR_PATH_TRT; ++path)
        {
            HairAzimuthalSample azimuthal = SampleHairAzimuthalLut(
                lightAngles,
                hair,
                path);
            result.pathLength += azimuthal.pathLength / 3.0;
            result.lutCoordinates = azimuthal.coordinates;
        }
    }
    result.absorption = hair.absorption;
}

void AccumulateHairVirtualLight(
    in MaterialSurface surface,
    in HairMaterialInputs hair,
    in HairTangentFrame frame,
    vec3 viewDirection,
    inout HairLightingResult result)
{
    if (hair.characterLighting.w == 0.0)
    {
        return;
    }

    // 源 Hair Virtual Light 使用 camera_vector 同时作为 L，并复用完整 HairBxDF；
    // 当前捕获的颜色是无色预乘强度，因此先以标量构造 achromatic radiance。
    VirtualLight virtualLight = CreateCameraVirtualLight(
        surface,
        vec3(hair.characterLighting.w));
    float lightVisibility = EvaluateVirtualLightVisibility(
        frame.geometricNormal,
        virtualLight);
    AccumulateHairLightPath(
        surface,
        ComputeHairAngles(
            frame,
            virtualLight.direction,
            viewDirection),
        hair,
        frame,
        virtualLight.direction,
        virtualLight.radiance,
        lightVisibility,
        lightVisibility,
        result);
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
        vec3 radiance = light.colorIntensity.xyz * light.colorIntensity.w *
            hair.characterLighting.y;
        float lightVisibility = EvaluateNeoXHairDirectionalVisibility(
            surface,
            frame,
            lightDirection,
            result.shadow);
        AccumulateHairLightPath(
            surface,
            ComputeHairAngles(frame, lightDirection, viewDirection),
            hair,
            frame,
            lightDirection,
            radiance,
            result.shadow,
            lightVisibility,
            result);
        result.shadowTransmittance = lightVisibility;
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
            (distance * distance + 1.0e-4) * hair.characterLighting.z;
        float lightVisibility = EvaluateNeoXHairLocalVisibility(
            frame,
            lightDirection);
        AccumulateHairLightPath(
            surface,
            ComputeHairAngles(frame, lightDirection, viewDirection),
            hair,
            frame,
            lightDirection,
            radiance,
            lightVisibility,
            lightVisibility,
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
            (distance * distance + 1.0e-4) * hair.characterLighting.z;
        float lightVisibility = EvaluateNeoXHairLocalVisibility(
            frame,
            lightDirection);
        AccumulateHairLightPath(
            surface,
            ComputeHairAngles(frame, lightDirection, viewDirection),
            hair,
            frame,
            lightDirection,
            radiance,
            lightVisibility,
            lightVisibility,
            result);
    }

    AccumulateHairVirtualLight(
        surface,
        hair,
        frame,
        viewDirection,
        result);

    result.directLighting =
        result.directR + result.directTT + result.directTRT +
        result.directScatter;
    return result;
}
HairLightingResult EvaluateHairIndirectLighting(
    in MaterialSurface surface,
    inout HairLightingResult result)
{
    HairVisibilityInputs visibility = BuildHairVisibilityInputs(
        surface.modelInputs.hair);
    HairTangentFrame frame = BuildHairTangentFrame(surface);
    vec3 viewDirection = normalize(
        uboVP.cameraPosition - surface.worldPosition);
    // 源环境方向是视线在 fiber axis 法截面上的投影，不是 fiber axis 本身，
    // 也不是普通表面的 reflection vector。
    vec3 projectedView = viewDirection - frame.tangent *
        dot(viewDirection, frame.tangent);
    float projectedViewLengthSquared = dot(projectedView, projectedView);
    vec3 hairEnvironmentDirection = projectedViewLengthSquared > 1.0e-8
        ? projectedView * inversesqrt(projectedViewLengthSquared)
        : frame.normal;
    HairUeScatteringContext environmentContext =
        BuildHairUeScatteringContext(
            frame.tangent,
            hairEnvironmentDirection,
            viewDirection);
    vec3 hairEnvironment = EvaluateIrradianceSH(hairEnvironmentDirection) *
        surface.modelInputs.hair.characterLighting.x;
    float hairEnvironmentVisibility = EvaluateHairVisibility(visibility) *
        visibility.density;
    float hairSpecular = EvaluateNeoXHairSpecular(
        surface,
        frame,
        viewDirection);
    vec3 environmentPathColor = EvaluateHairUePathColor(
        surface.modelInputs.hair);
    float sourceAmbientScale = 2.0 * HAIR_PI * hairEnvironmentVisibility;
    result.indirectR = hairEnvironment *
        EvaluateHairUeR(
            environmentContext,
            surface.modelInputs.hair,
            hairSpecular,
            HAIR_UE_AMBIENT_AREA) *
        sourceAmbientScale;
    // 源 HairShadingAmbient 明确不计算 TT，只组合 R、TRT 与 Scatter。
    result.indirectTT = vec3(0.0);
    result.indirectTRT = hairEnvironment *
        EvaluateHairUeTRT(
            environmentContext,
            surface.modelInputs.hair,
            environmentPathColor,
            HAIR_UE_AMBIENT_AREA) *
        sourceAmbientScale;
    vec3 environmentScatterColor = EvaluateHairBaseColorResponse(
        surface,
        1.0);
    result.indirectScatter = hairEnvironment *
        environmentScatterColor *
        EvaluateHairUeScatterBase(
            environmentContext,
            surface.modelInputs.hair,
            frame.tangent,
            frame.geometricNormal,
            hairEnvironmentDirection,
            viewDirection) *
        sourceAmbientScale;
    result.hairIblFallback = 0.0;

    float singleScattering =
        min(dot(result.directLighting, vec3(0.3333333)), 1.0);
    float remainingEnergy = max(1.0 - singleScattering, 0.0);
    float visibilityBudget = EvaluateHairVisibility(visibility) * visibility.density;
    float msWeight = min(
        remainingEnergy * visibilityBudget,
        surface.modelInputs.hair.multipleScatteringWeight *
            surface.modelInputs.hair.scatter);
    vec3 multipleScatteringTint = EvaluateHairUePathColor(
        surface.modelInputs.hair);
    result.multipleScattering =
        CalculateDiffuseIbl(
            surface.worldNormal,
            multipleScatteringTint,
            0.0) *
        msWeight;
    result.multipleScatteringFallback = 1.0;
    result.finalColor =
        surface.emissiveColor +
        result.directLighting +
        (result.indirectR + result.indirectTT + result.indirectTRT +
            result.indirectScatter +
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

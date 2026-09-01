#ifndef VL_ENGINE_PREINTEGRATED_SKIN_LIGHTING_GLSL
#define VL_ENGINE_PREINTEGRATED_SKIN_LIGHTING_GLSL

#include "../common/lighting.glsl"
#include "materialSurface.glsl"
#include "virtualLight.glsl"

const float PREINTEGRATED_SKIN_LUT_WIDTH = 128.0;
const float PREINTEGRATED_SKIN_TILE_HEIGHT = 64.0;

struct PreintegratedSkinLighting
{
    vec3 diffuse;
    vec3 specular;
    vec3 transmission;
    vec3 virtualDiffuse;
    vec3 virtualSpecular;
};

PreintegratedSkinLighting CreatePreintegratedSkinLighting()
{
    PreintegratedSkinLighting lighting;
    lighting.diffuse = vec3(0.0);
    lighting.specular = vec3(0.0);
    lighting.transmission = vec3(0.0);
    lighting.virtualDiffuse = vec3(0.0);
    lighting.virtualSpecular = vec3(0.0);
    return lighting;
}

vec4 ReadPreintegratedSkinMetadata(
    in sampler2D lutTable,
    int skinLutId,
    int texelIndex)
{
    // 每个 128x64 tile 的第 0 行保存 IBL average、outputMode 和 transmission metadata。
    return texelFetch(
        lutTable,
        ivec2(
            texelIndex,
            skinLutId * int(PREINTEGRATED_SKIN_TILE_HEIGHT)),
        0);
}

vec3 SamplePreintegratedSkinResponse(
    in sampler2D lutTable,
    int skinLutId,
    float nDotL,
    float normalizedThickness)
{
    // 第 1..62 行才是普通 response；thickness 域由加载期校验保证有效。
    ivec2 tableSize = textureSize(lutTable, 0);
    float responseRow =
        float(skinLutId) * PREINTEGRATED_SKIN_TILE_HEIGHT +
        1.0 +
        normalizedThickness *
            (PREINTEGRATED_SKIN_TILE_HEIGHT - 2.0);
    vec2 uv = vec2(
        nDotL * 0.5 + 0.5,
        (responseRow + 0.5) / float(tableSize.y));
    return texture(lutTable, uv).rgb;
}

void AccumulatePreintegratedSkinLight(
    in MaterialSurface surface,
    in sampler2D lutTable,
    in vec3 lightDirection,
    in vec3 radiance,
    in float lightMultiplier,
    in float transmissionMultiplier,
    in float virtualLight,
    inout PreintegratedSkinLighting lighting)
{
    vec3 scaledRadiance = radiance * lightMultiplier;
    int skinLutId = int(
        surface.modelInputs.preintegratedSkin.skinLutId + 0.5);
    vec4 responseMetadata = ReadPreintegratedSkinMetadata(
        lutTable,
        skinLutId,
        0);
    vec4 transmissionMetadata = ReadPreintegratedSkinMetadata(
        lutTable,
        skinLutId,
        1);
    float normalizedThickness =
        surface.modelInputs.preintegratedSkin.thickness *
        surface.modelInputs.preintegratedSkin.thicknessScale /
        transmissionMetadata.w;
    // NeoX/UE Skin 的底层模糊法线只负责 LUT 漫反射方向；顶层法线仍保留给
    // 高光和背光透射，避免把两个 lobe 压成同一份法线响应。
    vec3 diffuseNormal = normalize(
        surface.modelInputs.preintegratedSkin.bottomNormal);
    float nDotL = dot(diffuseNormal, lightDirection);
    vec3 response = SamplePreintegratedSkinResponse(
        lutTable,
        skinLutId,
        nDotL,
        normalizedThickness);
    // scatteringMultiplier 模式不含 Lambert 项，finalDiffuseResponse 模式已经包含。
    if (responseMetadata.w < 0.5)
    {
        response *= max(nDotL, 0.0) / PI;
    }
    vec3 diffuseColor =
        surface.baseColor * (1.0 - surface.metallic);
    vec3 diffuseLighting = diffuseColor * scaledRadiance * response;
    vec3 specularLighting = EvaluateNeoXSkinDualSpecularLight(
        surface.worldNormal,
        surface.worldPosition,
        uboVP.cameraPosition,
        surface.baseColor,
        surface.roughness,
        surface.metallic,
        surface.specular,
        lightDirection,
        scaledRadiance);
    // Virtual Light 是摄像机绑定补光，不应继承真实方向光的 CSM 阴影；分账后由
    // Deferred composition 单独合成，避免阴影区域把角色补光一并抹掉。
    if (virtualLight > 0.5)
    {
        lighting.virtualDiffuse += diffuseLighting;
        lighting.virtualSpecular += specularLighting;
    }
    else
    {
        lighting.diffuse += diffuseLighting;
        lighting.specular += specularLighting;
    }

    float backNoL = max(
        dot(-normalize(surface.worldNormal), lightDirection),
        0.0);
    lighting.transmission +=
        surface.baseColor * transmissionMetadata.rgb * scaledRadiance *
        backNoL * exp(-2.0 * normalizedThickness) *
        transmissionMultiplier;
}

PreintegratedSkinLighting CalculatePreintegratedSkinDirectLighting(
    in MaterialSurface surface,
    in sampler2D lutTable)
{
    PreintegratedSkinLighting lighting =
        CreatePreintegratedSkinLighting();
    int offset = uboLight.directionalLightOffset;
    int end = offset + uboLight.directionalLightCount;
    for (int lightIndex = offset; lightIndex < end; ++lightIndex)
    {
        Light light = uboLight.lights[lightIndex];
        AccumulatePreintegratedSkinLight(
            surface,
            lutTable,
            normalize(-light.directionPad.xyz),
            light.colorIntensity.xyz * light.colorIntensity.w,
            surface.modelInputs.preintegratedSkin.characterLighting.y,
            1.0,
            0.0,
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
        AccumulatePreintegratedSkinLight(
            surface,
            lutTable,
            normalize(lightOffset),
            radiance,
            1.0,
            1.0,
            0.0,
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
        AccumulatePreintegratedSkinLight(
            surface,
            lutTable,
            lightDirection,
            radiance,
            1.0,
            1.0,
            0.0,
            lighting);
    }

    if (surface.modelInputs.preintegratedSkin.characterLighting.w > 0.0)
    {
        VirtualLight virtualLight = CreateCameraVirtualLight(
            surface,
            vec3(surface.modelInputs.preintegratedSkin.characterLighting.w));
        // Source Virtual Light 只把补光的 diffuse/specular 写入当前 Mesh/lighting
        // 结果，不额外制造背光 transmission；因此这里复用 Skin response，但关闭
        // transmission 分支。Deferred 使用该调用是 VulkanLearn 的显式兼容扩展，
        // 因为当前 NeoX Skin 默认走 GBuffer + Deferred，而源实现没有 Deferred 版本。
        AccumulatePreintegratedSkinLight(
            surface,
            lutTable,
            virtualLight.direction,
            virtualLight.radiance * EvaluateVirtualLightVisibility(
                surface.worldNormal,
                virtualLight),
            1.0,
            0.0,
            1.0,
            lighting);
    }
    return lighting;
}

vec3 CalculatePreintegratedSkinIndirectDiffuse(
    in MaterialSurface surface,
    in sampler2D lutTable)
{
    int skinLutId = int(
        surface.modelInputs.preintegratedSkin.skinLutId + 0.5);
    vec3 iblAverage = ReadPreintegratedSkinMetadata(
        lutTable,
        skinLutId,
        0).rgb;
    return CalculateDiffuseIbl(
        normalize(surface.modelInputs.preintegratedSkin.bottomNormal),
        surface.baseColor,
        surface.metallic) *
        surface.modelInputs.preintegratedSkin.characterLighting.x *
        iblAverage;
}

#endif

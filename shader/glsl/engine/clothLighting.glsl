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
    float visibility;
    float modelVersion;
    vec3 worldTangent;
    float anisotropy;
    float anisotropyCross;
    vec2 roughnessAxes;
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
    result.visibility = 0.0;
    result.modelVersion = 2.0;
    result.worldTangent = vec3(1.0, 0.0, 0.0);
    result.anisotropy = 0.0;
    result.anisotropyCross = 0.0;
    result.roughnessAxes = vec2(0.0);
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

vec3 ClothWorldTangent(in MaterialSurface surface)
{
    vec3 normal = normalize(surface.worldNormal);
    return normalize(
        surface.worldTangent.xyz -
        normal * dot(normal, surface.worldTangent.xyz));
}

vec3 ClothWorldBitangent(
    in MaterialSurface surface,
    vec3 normal,
    vec3 tangent)
{
    // Cloth 椭圆瓣只依赖轴向平方项，但仍保留 handedness 以便 debug 和
    // 后续扩展使用一致的 TBN；镜像 UV 不会在 v2 中产生离散跳变。
    return normalize(cross(normal, tangent) * surface.worldTangent.w);
}

vec3 ClothWorldToLocal(
    vec3 direction,
    vec3 tangent,
    vec3 bitangent,
    vec3 normal)
{
    return vec3(
        dot(tangent, direction),
        dot(bitangent, direction),
        dot(normal, direction));
}

float SampleClothAnisotropicDirectionalAlbedo(
    in sampler2DArray clothAnisotropicDirectionalAlbedoLut,
    float NdotV,
    float alpha,
    float anisotropy,
    float anisotropyCross,
    vec3 localViewDirection)
{
    const float anisotropyLayerCount = 33.0;
    float viewTangentSquared = localViewDirection.x * localViewDirection.x;
    float viewBitangentSquared = localViewDirection.y * localViewDirection.y;
    float viewTangentPlaneSquared =
        viewTangentSquared + viewBitangentSquared;
    float phiCosine2 = viewTangentPlaneSquared <= 0.0
        ? 0.0
        : (viewTangentSquared - viewBitangentSquared) /
            viewTangentPlaneSquared;
    float axisWeight = 0.5 + 0.5 * phiCosine2;
    // sampler2DArray 的第三坐标是数组层索引而不是 0..1 UV；这里显式
    // 映射到 33 层并手动插值，避免不同驱动把层坐标离散取整造成能量跳变。
    float primaryLayer = (anisotropy * 0.5 + 0.5) *
        (anisotropyLayerCount - 1.0);
    float oppositeLayer = (-anisotropy * 0.5 + 0.5) *
        (anisotropyLayerCount - 1.0);
    float primaryLayerFloor = floor(primaryLayer);
    float oppositeLayerFloor = floor(oppositeLayer);
    float primaryLayerFraction = primaryLayer - primaryLayerFloor;
    float oppositeLayerFraction = oppositeLayer - oppositeLayerFloor;
    vec4 primary = mix(
        texture(
            clothAnisotropicDirectionalAlbedoLut,
            vec3(NdotV, alpha, primaryLayerFloor)),
        texture(
            clothAnisotropicDirectionalAlbedoLut,
            vec3(NdotV, alpha, min(
                primaryLayerFloor + 1.0,
                anisotropyLayerCount - 1.0))),
        primaryLayerFraction);
    vec4 opposite = mix(
        texture(
            clothAnisotropicDirectionalAlbedoLut,
            vec3(NdotV, alpha, oppositeLayerFloor)),
        texture(
            clothAnisotropicDirectionalAlbedoLut,
            vec3(NdotV, alpha, min(
                oppositeLayerFloor + 1.0,
                anisotropyLayerCount - 1.0))),
        oppositeLayerFraction);
    float primaryResponse = mix(primary.g, primary.r, axisWeight);
    float oppositeResponse = mix(opposite.g, opposite.r, axisWeight);
    // LUT 的 R/G 分别保存 phi=0 与 phi=pi/2；cos(2phi) 插值覆盖完整
    // 旋转，而不是把方向离散到两个轴上。
    return mix(
        primaryResponse,
        0.5 * (primaryResponse + oppositeResponse),
        anisotropyCross);
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
    in sampler2D clothDirectionalAlbedoLut,
    in sampler2DArray clothAnisotropicDirectionalAlbedoLut)
{
    vec3 N = normalize(surface.worldNormal);
    vec3 L = normalize(lightDirection_WS);
    vec3 V = normalize(uboVP.cameraPosition - surface.worldPosition);
    float NdotL = dot(N, L);
    float NdotV = dot(N, V);
    ClothDirectLighting result = CreateDefaultClothDirectLighting();
    if (NdotL <= 0.0 || NdotV <= 0.0)
    {
        return result;
    }

    float alpha = ClothSheenRoughnessToAlpha(
        surface.modelInputs.cloth.sheenRoughness);
    vec3 T = ClothWorldTangent(surface);
    vec3 B = ClothWorldBitangent(surface, N, T);
    vec3 localLight = ClothWorldToLocal(L, T, B, N);
    vec3 localView = ClothWorldToLocal(V, T, B, N);
    float anisotropy = surface.modelInputs.cloth.anisotropy;
    float anisotropyCross = surface.modelInputs.cloth.anisotropyCross;
    float directionalAlbedo = anisotropy == 0.0
        ? SampleClothDirectionalAlbedo(
            clothDirectionalAlbedoLut,
            NdotV,
            alpha)
        : SampleClothAnisotropicDirectionalAlbedo(
            clothAnisotropicDirectionalAlbedoLut,
            NdotV,
            alpha,
            anisotropy,
            anisotropyCross,
            localView);
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
    float sheenResponse = anisotropy == 0.0
        ? ClothSheenUnitResponse(
            alpha,
            NdotL,
            NdotV,
            dot(N, normalize(L + V)))
        : ClothAnisotropicSheenUnitResponse(
            alpha,
            anisotropy,
            anisotropyCross,
            localLight,
            localView);
    result.diffuse = baseLobes.diffuse * baseEnergyScale;
    result.specular = baseLobes.specular * baseEnergyScale;
    result.sheen = surface.modelInputs.cloth.sheenColor *
        sheenResponse * radiance * NdotL;
    return result;
}

ClothDirectLighting CalculateClothDirectLighting(
    in MaterialSurface surface,
    in sampler2D clothDirectionalAlbedoLut,
    in sampler2DArray clothAnisotropicDirectionalAlbedoLut)
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
            clothDirectionalAlbedoLut,
            clothAnisotropicDirectionalAlbedoLut);
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
            clothDirectionalAlbedoLut,
            clothAnisotropicDirectionalAlbedoLut);
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
            clothDirectionalAlbedoLut,
            clothAnisotropicDirectionalAlbedoLut);
        lighting.diffuse += lobe.diffuse;
        lighting.specular += lobe.specular;
        lighting.sheen += lobe.sheen;
    }
    return lighting;
}

ClothLightingResult ShadeClothSurface(
    in MaterialSurface surface,
    in sampler2D clothDirectionalAlbedoLut,
    in sampler2DArray clothAnisotropicDirectionalAlbedoLut,
    in sampler2DArrayShadow inputShadowMap)
{
    ClothLightingResult result = CreateDefaultClothLightingResult();
    vec3 viewDir = normalize(uboVP.cameraPosition - surface.worldPosition);
    vec3 normal = normalize(surface.worldNormal);
    vec3 tangent = ClothWorldTangent(surface);
    vec3 bitangent = ClothWorldBitangent(surface, normal, tangent);
    vec3 localView = ClothWorldToLocal(
        viewDir,
        tangent,
        bitangent,
        normal);
    float alpha = ClothSheenRoughnessToAlpha(
        surface.modelInputs.cloth.sheenRoughness);
    float NdotV = dot(normal, viewDir);
    if (NdotV <= 0.0)
    {
        return result;
    }
    result.directionalAlbedo = surface.modelInputs.cloth.anisotropy == 0.0
        ? SampleClothDirectionalAlbedo(
            clothDirectionalAlbedoLut,
            NdotV,
            alpha)
        : SampleClothAnisotropicDirectionalAlbedo(
            clothAnisotropicDirectionalAlbedoLut,
            NdotV,
            alpha,
            surface.modelInputs.cloth.anisotropy,
            surface.modelInputs.cloth.anisotropyCross,
            localView);
    result.baseEnergyScale = vec3(1.0) -
        surface.modelInputs.cloth.sheenColor * result.directionalAlbedo;

    result.sheenColor = surface.modelInputs.cloth.sheenColor;
    result.sheenRoughness = surface.modelInputs.cloth.sheenRoughness;
    result.worldTangent = tangent;
    result.anisotropy = surface.modelInputs.cloth.anisotropy;
    result.anisotropyCross = surface.modelInputs.cloth.anisotropyCross;
    float aspect = ClothAnisotropyToAspect(result.anisotropy);
    result.roughnessAxes = vec2(alpha * aspect, alpha / aspect);
    // Debug 值使用视线方向作为代表半角，避免把某一盏灯的结果误认为材质常量。
    vec3 debugHalf = normalize(localView + localView);
    if (result.anisotropy == 0.0)
    {
        result.charlieD = ClothCharlieDistribution(alpha, debugHalf.z);
        result.visibility = ClothNeubeltVisibility(NdotV, NdotV);
    }
    else
    {
        result.charlieD = ClothAnisotropicCharlieDistribution(
            alpha,
            result.anisotropy,
            debugHalf);
        result.visibility = ClothAnisotropicVisibility(
            alpha,
            result.anisotropy,
            localView,
            localView);
    }

    ClothDirectLighting direct = CalculateClothDirectLighting(
        surface,
        clothDirectionalAlbedoLut,
        clothAnisotropicDirectionalAlbedoLut);
    int cascadeIndex = 0;
    result.shadow = CalculateCsmShadow(
        inputShadowMap,
        surface.worldPosition,
        normal,
        cascadeIndex);
    result.shadow *= surface.precomputedShadowFactors.r;
    result.shadowCascadeIndex = ShadowCascadeDebugValue(cascadeIndex);
    result.directDiffuse = direct.diffuse * result.shadow;
    result.directSpecular = direct.specular * result.shadow;
    result.directSheen = direct.sheen * result.shadow;

    result.indirectDiffuse = CalculateDiffuseIbl(
        normal,
        surface.baseColor,
        0.0) * result.baseEnergyScale;
    result.indirectSpecular = CalculateSpecularIbl(
        normal,
        viewDir,
        surface.baseColor,
        surface.roughness,
        0.0) * result.baseEnergyScale;
    // sheenIblVersion=0：明确标记的低频 fallback，不复用 GGX split-sum sheen。
    result.indirectSheen = CalculateDiffuseIbl(
        normal,
        surface.modelInputs.cloth.sheenColor,
        0.0) * result.directionalAlbedo;
    return result;
}

#endif

#ifndef PARTICLE_SCENE_LIGHTING_GLSL
#define PARTICLE_SCENE_LIGHTING_GLSL

// 粒子专用场景光照。
//
// 为什么不能直接 include common/lighting.glsl：
//   1. 它把场景灯与 IBL 固定放在 set 0 binding 1/2/3/4，而粒子 set 0
//      binding 1 已被粒子池、binding 2 已被纹理占用，两条管线无法共享布局。
//   2. simulate/emit 等 compute 也会 include particleCommon.glsl，若把
//      sampler 声明放进公共头，compute descriptor layout 会多出未写入的
//      采样器。
// 因此这里只被 graphics fragment shader include，绑定从 binding 6 开始，
// 公式与 common/lighting.glsl 保持一致，避免同一套灯出现两种亮度语义。
//
// 数据所有权：场景 Light 数组与 GPU 粒子灯 SSBO 都由 RendererFrameResources
// 按 swapchain image 持有；本文件只读，不写回。

#include "../common/defines.glsl"
#include "../common/microfacetDistribution.glsl"
// 局部光阴影采样与 common/lighting.glsl 共用同一份纯函数；sampler 由四个
// particle fragment shader 各自声明 set 3 binding 2 后传入。
#include "../common/localShadowSampling.glsl"

// 与 common/lighting.glsl 的 LightGPU 逐字段一致（4 × vec4 = 64 字节）。
// colorIntensity.xyz 是线性 RGB tint，w 是方向光 lux 或点光/聚光 cd * 100^2；
// positionRadius 的 w 是作者半径（cm），0 表示无限半径。
struct ParticleSceneLightGPU
{
    vec4 colorIntensity;
    vec4 positionRadius;
    vec4 directionPad;
    vec4 coneAngleOuterInnerPadPad;
    vec4 shadowParams;
};

// 与 common/lighting.glsl 的 ParticleLightGPU 逐字段一致（2 × vec4）。
struct ParticlePointLightGPU
{
    vec4 positionRadius;
    vec4 colorIntensity;
};

// CPU 侧 LightSSBOHeader 是 6 个有效 int + 2 个 padding（共 32 字节）；
// Light 的 std430 对齐是 16，所以数组从 32 字节开始。这里显式写出 padding，
// 不能简化成 "uvec4 header"，否则数组会提前到 16 字节并读错第一盏灯。
layout(std430, set = 0, binding = 6) readonly buffer ParticleSceneLightSSBO
{
    int directionalLightOffset;
    int directionalLightCount;
    int pointLightOffset;
    int pointLightCount;
    int spotLightOffset;
    int spotLightCount;
    int reservedHeader0;
    int reservedHeader1;
    ParticleSceneLightGPU lights[];
} particleSceneLights;

// x = high-water count（<= 64），空槽用零强度跳过；容量由 GPU 侧固定。
layout(std430, set = 0, binding = 7) readonly buffer ParticleLightSSBO
{
    uvec4 header;
    ParticlePointLightGPU lights[];
} particleLightSSBO;

layout(set = 0, binding = 8) uniform samplerCube prefilteredEnvironmentCube;
layout(set = 0, binding = 9) uniform sampler2D brdfLut;

struct ParticleLightingLobes
{
    vec3 diffuse;
    vec3 specular;
};

ParticleLightingLobes CreateParticleLightingLobes()
{
    ParticleLightingLobes lobes;
    lobes.diffuse = vec3(0.0);
    lobes.specular = vec3(0.0);
    return lobes;
}

// 点光/聚光作者半径窗。核心衰减仍是 1/(d^2+1)，与 common/lighting.glsl
// 相同的厘米约定；半径 > 0 时再乘 UE 风格的四次窗函数，
// 让 "Attenuation Radius" 真正在 GPU 上生效。radius <= 0 保持旧的无界行为，
// 避免已有资产因为缺省值突然变暗。
float ParticleLightRadiusWindow(float distanceSquared, float radius)
{
    if (radius <= 0.0)
    {
        return 1.0;
    }
    float normalizedSquared =
        distanceSquared / max(radius * radius, 1.0e-6);
    float window = clamp(
        1.0 - normalizedSquared * normalizedSquared,
        0.0,
        1.0);
    return window * window;
}

float ParticleAttenuation(float distanceSquared, float radius)
{
    return
        ParticleLightRadiusWindow(distanceSquared, radius) /
        max(distanceSquared + 1.0, 1.0e-6);
}

vec3 ParticleFresnelSchlickVec(float cosTheta, vec3 f0)
{
    return f0 + (vec3(1.0) - f0) *
        pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float ParticleGeometrySmith(
    vec3 normal,
    vec3 viewDirection,
    vec3 lightDirection,
    float roughness)
{
    float normalView = max(dot(normal, viewDirection), 0.0);
    float normalLight = max(dot(normal, lightDirection), 0.0);
    return
        GeometrySchlickGGX(normalView, roughness) *
        GeometrySchlickGGX(normalLight, roughness);
}

// DefaultLit 的直接光 lobe：与 common/lighting.glsl 的
// EvaluateDefaultPbrLightLobes 同公式，但接受调用方已算好的光源方向/辐射度，
// 以便方向光、点光、聚光和 GPU 粒子灯共用同一条 BRDF 路径。
ParticleLightingLobes EvaluateParticleDefaultPbrLight(
    vec3 normal,
    vec3 pixelPosition,
    vec3 cameraPosition,
    vec3 baseColor,
    float roughness,
    float metallic,
    vec3 lightDirection,
    vec3 radiance)
{
    vec3 n = normalize(normal);
    vec3 l = normalize(lightDirection);
    vec3 v = normalize(cameraPosition - pixelPosition);
    vec3 h = normalize(l + v);

    float normalLight = max(dot(n, l), 0.0);
    float normalView = max(dot(n, v), 0.0);
    vec3 f0 = mix(vec3(0.04), baseColor, metallic);
    vec3 fresnel = ParticleFresnelSchlickVec(max(dot(h, v), 0.0), f0);
    // 漫反射保留 (1 - F) * (1 - metallic) 的能量分配，与主渲染路径一致。
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    float distribution = DistributionGGX(n, h, roughness);
    float geometry = ParticleGeometrySmith(n, v, l, roughness);

    ParticleLightingLobes lobes;
    lobes.diffuse =
        diffuseWeight * baseColor / PI * radiance * normalLight;
    lobes.specular =
        fresnel * distribution * geometry /
        max(4.0 * normalLight * normalView, 1.0e-4) *
        radiance * normalLight;
    return lobes;
}

// billboard 没有几何法线，只有面向相机的法线；完全 Lambert 会让侧后方光
// 完全丢失，因此用 wrap = 0.5 的 wrapped diffuse 把明暗交界推后半个半球。
float ParticleWrappedDiffuse(float normalLight, float wrap)
{
    return clamp(
        (normalLight + wrap) / (1.0 + wrap),
        0.0,
        1.0);
}

// 环境漫反射近似：粒子 UBO 不含 GPU 写的 SH 系数，无法在 CPU 侧复制；
// 预过滤 cubemap 的最高 mip 已接近全粗糙方向平均，直接作为 irradiance
// 近似可保持与场景 IBL 同量级，且不需要额外 UBO 通道。
vec3 ParticleSampleDiffuseIbl(vec3 normal, vec3 baseColor)
{
    float maxLod = float(max(textureQueryLevels(prefilteredEnvironmentCube) - 1, 0));
    vec3 irradianceApprox = textureLod(
        prefilteredEnvironmentCube,
        normalize(normal),
        maxLod).rgb;
    return irradianceApprox * baseColor;
}

vec3 ParticleSamplePrefilteredEnvironment(
    vec3 normal,
    vec3 viewDirection,
    float roughness)
{
    vec3 n = normalize(normal);
    vec3 v = normalize(viewDirection);
    vec3 reflected = reflect(-v, n);
    float maxLod = float(max(textureQueryLevels(prefilteredEnvironmentCube) - 1, 0));
    return textureLod(
        prefilteredEnvironmentCube,
        reflected,
        roughness * maxLod).rgb;
}

vec3 ParticleEvaluateSpecularIbl(
    vec3 normal,
    vec3 viewDirection,
    vec3 f0,
    float roughness)
{
    vec3 n = normalize(normal);
    vec3 v = normalize(viewDirection);
    float normalView = max(dot(n, v), 0.0);
    vec3 prefiltered = ParticleSamplePrefilteredEnvironment(
        n,
        v,
        roughness);
    vec2 brdf = texture(brdfLut, vec2(normalView, roughness)).rg;
    return prefiltered * (f0 * brdf.x + brdf.y);
}

// 单盏场景灯的方向光贡献（不含 CSM；阴影由调用方乘到 radiance 上）。
ParticleLightingLobes AccumulateParticleDirectionalLight(
    ParticleSceneLightGPU light,
    vec3 normal,
    vec3 pixelPosition,
    vec3 cameraPosition,
    vec3 baseColor,
    float roughness,
    float metallic,
    float shadow)
{
    vec3 radiance =
        light.colorIntensity.xyz *
        light.colorIntensity.w *
        shadow;
    return EvaluateParticleDefaultPbrLight(
        normal,
        pixelPosition,
        cameraPosition,
        baseColor,
        roughness,
        metallic,
        -light.directionPad.xyz,
        radiance);
}

ParticleLightingLobes AccumulateParticlePointLight(
    in sampler2DArrayShadow localShadowMap,
    in vec4 shadowParams,
    vec3 lightPosition,
    float lightRadius,
    vec3 lightColorIntensity,
    vec3 normal,
    vec3 pixelPosition,
    vec3 cameraPosition,
    vec3 baseColor,
    float roughness,
    float metallic)
{
    vec3 offset = lightPosition - pixelPosition;
    float distanceSquared = dot(offset, offset);
    float attenuation = ParticleAttenuation(
        distanceSquared,
        lightRadius);
    if (attenuation <= 0.0)
    {
        return CreateParticleLightingLobes();
    }
    vec3 radiance =
        attenuation *
        lightColorIntensity *
        SampleLocalLightShadow(
            localShadowMap,
            shadowParams,
            lightPosition,
            vec3(0.0),
            0.0,
            pixelPosition);
    return EvaluateParticleDefaultPbrLight(
        normal,
        pixelPosition,
        cameraPosition,
        baseColor,
        roughness,
        metallic,
        offset,
        radiance);
}

ParticleLightingLobes AccumulateParticleSpotLight(
    in sampler2DArrayShadow localShadowMap,
    ParticleSceneLightGPU light,
    vec3 normal,
    vec3 pixelPosition,
    vec3 cameraPosition,
    vec3 baseColor,
    float roughness,
    float metallic)
{
    vec3 offset = light.positionRadius.xyz - pixelPosition;
    vec3 surfaceToLight = normalize(offset);
    float spotAngle = acos(clamp(dot(
        surfaceToLight,
        -light.directionPad.xyz), -1.0, 1.0));
    float angleIntensity = 1.0 - smoothstep(
        light.coneAngleOuterInnerPadPad.y,
        light.coneAngleOuterInnerPadPad.x,
        spotAngle);
    if (angleIntensity <= 0.0)
    {
        return CreateParticleLightingLobes();
    }
    float distanceSquared = dot(offset, offset);
    float attenuation =
        ParticleAttenuation(
            distanceSquared,
            light.positionRadius.w) *
        angleIntensity;
    vec3 radiance =
        attenuation *
        light.colorIntensity.xyz *
        light.colorIntensity.w *
        SampleLocalLightShadow(
            localShadowMap,
            light.shadowParams,
            light.positionRadius.xyz,
            light.directionPad.xyz,
            light.coneAngleOuterInnerPadPad.x,
            pixelPosition);
    return EvaluateParticleDefaultPbrLight(
        normal,
        pixelPosition,
        cameraPosition,
        baseColor,
        roughness,
        metallic,
        surfaceToLight,
        radiance);
}

// 全部场景灯 + GPU 粒子灯聚合。只有 mesh 粒子需要 specular lobe；
// billboard 走下面的 wrapped Lambert。
ParticleLightingLobes CalculateParticleMeshSceneLighting(
    sampler2DArrayShadow localShadowMap,
    vec3 normal,
    vec3 pixelPosition,
    vec3 cameraPosition,
    vec3 baseColor,
    float roughness,
    float metallic,
    float directionalShadow)
{
    ParticleLightingLobes lighting = CreateParticleLightingLobes();

    int offset = particleSceneLights.directionalLightOffset;
    int end = offset + particleSceneLights.directionalLightCount;
    for (int index = offset; index < end; ++index)
    {
        ParticleLightingLobes lobes = AccumulateParticleDirectionalLight(
            particleSceneLights.lights[index],
            normal,
            pixelPosition,
            cameraPosition,
            baseColor,
            roughness,
            metallic,
            directionalShadow);
        lighting.diffuse += lobes.diffuse;
        lighting.specular += lobes.specular;
    }

    offset = particleSceneLights.pointLightOffset;
    end = offset + particleSceneLights.pointLightCount;
    for (int index = offset; index < end; ++index)
    {
        ParticleSceneLightGPU light = particleSceneLights.lights[index];
        ParticleLightingLobes lobes = AccumulateParticlePointLight(
            localShadowMap,
            light.shadowParams,
            light.positionRadius.xyz,
            light.positionRadius.w,
            light.colorIntensity.xyz * light.colorIntensity.w,
            normal,
            pixelPosition,
            cameraPosition,
            baseColor,
            roughness,
            metallic);
        lighting.diffuse += lobes.diffuse;
        lighting.specular += lobes.specular;
    }

    offset = particleSceneLights.spotLightOffset;
    end = offset + particleSceneLights.spotLightCount;
    for (int index = offset; index < end; ++index)
    {
        ParticleLightingLobes lobes = AccumulateParticleSpotLight(
            localShadowMap,
            particleSceneLights.lights[index],
            normal,
            pixelPosition,
            cameraPosition,
            baseColor,
            roughness,
            metallic);
        lighting.diffuse += lobes.diffuse;
        lighting.specular += lobes.specular;
    }

    uint particleLightCount = min(particleLightSSBO.header.x, 64u);
    for (uint index = 0u; index < particleLightCount; ++index)
    {
        ParticlePointLightGPU light = particleLightSSBO.lights[index];
        if (light.colorIntensity.w <= 0.0)
        {
            continue;
        }
        ParticleLightingLobes lobes = AccumulateParticlePointLight(
            localShadowMap,
            vec4(-1.0),
            light.positionRadius.xyz,
            light.positionRadius.w,
            light.colorIntensity.xyz * light.colorIntensity.w,
            normal,
            pixelPosition,
            cameraPosition,
            baseColor,
            roughness,
            metallic);
        lighting.diffuse += lobes.diffuse;
        lighting.specular += lobes.specular;
    }
    return lighting;
}

// billboard/ribbon 的完整场景光照：漫反射 wrapped Lambert，无镜面 lobe。
// directionalShadow 只作用于方向光；点光/聚光走各自的 localShadowMap slice，
// GPU 粒子灯仍是无阴影的裸点光（见 CalculateParticleLightLobes 的合同）。
// lightingStrength 是“场景光照替换 authored 颜色的比例”：0 = 完全保留
// unlit 作者颜色，1 = 完全使用场景光照。粒子资产通常把颜色作者化成最终
// 观感（偏 emissive），直接相乘会在暗场景掉亮度、在强光附近过曝；因此
// 用插值而不是纯乘法，让美术可以按效果调节被灯染色的程度。
vec3 CalculateParticleBillboardSceneLighting(
    sampler2DArrayShadow localShadowMap,
    vec3 baseColor,
    vec3 pixelPosition,
    vec3 lightingNormal,
    vec3 cameraPosition,
    float ambientScale,
    float lightingStrength,
    float directionalShadow)
{
    vec3 normal = normalize(lightingNormal);
    vec3 lighting = vec3(0.0);
    const float wrap = 0.5;

    int offset = particleSceneLights.directionalLightOffset;
    int end = offset + particleSceneLights.directionalLightCount;
    for (int index = offset; index < end; ++index)
    {
        ParticleSceneLightGPU light = particleSceneLights.lights[index];
        vec3 lightDirection = normalize(-light.directionPad.xyz);
        float wrapped = ParticleWrappedDiffuse(
            dot(normal, lightDirection),
            wrap);
        vec3 radiance =
            light.colorIntensity.xyz *
            light.colorIntensity.w *
            directionalShadow;
        lighting += baseColor / PI * radiance * wrapped;
    }

    offset = particleSceneLights.pointLightOffset;
    end = offset + particleSceneLights.pointLightCount;
    for (int index = offset; index < end; ++index)
    {
        ParticleSceneLightGPU light = particleSceneLights.lights[index];
        vec3 offsetToLight = light.positionRadius.xyz - pixelPosition;
        float distanceSquared = dot(offsetToLight, offsetToLight);
        float attenuation = ParticleAttenuation(
            distanceSquared,
            light.positionRadius.w);
        if (attenuation <= 0.0)
        {
            continue;
        }
        float wrapped = ParticleWrappedDiffuse(
            dot(normal, normalize(offsetToLight)),
            wrap);
        vec3 radiance =
            attenuation *
            light.colorIntensity.xyz *
            light.colorIntensity.w *
            SampleLocalLightShadow(
                localShadowMap,
                light.shadowParams,
                light.positionRadius.xyz,
                light.directionPad.xyz,
                light.coneAngleOuterInnerPadPad.x,
                pixelPosition);
        lighting += baseColor / PI * radiance * wrapped;
    }

    offset = particleSceneLights.spotLightOffset;
    end = offset + particleSceneLights.spotLightCount;
    for (int index = offset; index < end; ++index)
    {
        ParticleSceneLightGPU light = particleSceneLights.lights[index];
        vec3 offsetToLight = light.positionRadius.xyz - pixelPosition;
        vec3 surfaceToLight = normalize(offsetToLight);
        float spotAngle = acos(clamp(dot(
            surfaceToLight,
            -light.directionPad.xyz), -1.0, 1.0));
        float angleIntensity = 1.0 - smoothstep(
            light.coneAngleOuterInnerPadPad.y,
            light.coneAngleOuterInnerPadPad.x,
            spotAngle);
        if (angleIntensity <= 0.0)
        {
            continue;
        }
        float distanceSquared = dot(offsetToLight, offsetToLight);
        float attenuation =
            ParticleAttenuation(
                distanceSquared,
                light.positionRadius.w) *
            angleIntensity;
        float wrapped = ParticleWrappedDiffuse(
            dot(normal, surfaceToLight),
            wrap);
        vec3 radiance =
            attenuation *
            light.colorIntensity.xyz *
            light.colorIntensity.w *
            SampleLocalLightShadow(
                localShadowMap,
                light.shadowParams,
                light.positionRadius.xyz,
                light.directionPad.xyz,
                light.coneAngleOuterInnerPadPad.x,
                pixelPosition);
        lighting += baseColor / PI * radiance * wrapped;
    }

    uint particleLightCount = min(particleLightSSBO.header.x, 64u);
    for (uint index = 0u; index < particleLightCount; ++index)
    {
        ParticlePointLightGPU light = particleLightSSBO.lights[index];
        if (light.colorIntensity.w <= 0.0)
        {
            continue;
        }
        vec3 offsetToLight = light.positionRadius.xyz - pixelPosition;
        float distanceSquared = dot(offsetToLight, offsetToLight);
        float attenuation = ParticleAttenuation(
            distanceSquared,
            light.positionRadius.w);
        if (attenuation <= 0.0)
        {
            continue;
        }
        float wrapped = ParticleWrappedDiffuse(
            dot(normal, normalize(offsetToLight)),
            wrap);
        vec3 radiance =
            attenuation *
            light.colorIntensity.xyz *
            light.colorIntensity.w;
        lighting += baseColor / PI * radiance * wrapped;
    }

    // ambientScale 沿用作者的 billboardLightParams.y。常量 ambient 保留为
    // 环境缺失（黑 cubemap）时的下限，IBL 作为额外的环境项；两者都乘同一
    // 缩放，因此亮度语义与原实现连续。
    lighting +=
        (baseColor + ParticleSampleDiffuseIbl(normal, baseColor)) *
        ambientScale;
    return mix(
        baseColor,
        lighting,
        clamp(lightingStrength, 0.0, 1.0));
}

#endif

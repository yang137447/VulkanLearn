#include "particleCommon.glsl"
#include "particleSceneLighting.glsl"
#include "particleSoftDepth.glsl"
#include "particleDissolve.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vSoftness;
layout(location = 2) in float vViewDepth;
layout(location = 4) in vec3 vNormal;
layout(location = 5) in vec3 vWorldPosition;
layout(location = 8) in vec2 vUv;
layout(location = 9) in float vLifeT;
layout(location = 10) in float vSeed;

layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform sampler2D sceneDepth;
layout(set = 3, binding = 1) uniform sampler2DArrayShadow shadowMap;
layout(set = 3, binding = 2) uniform sampler2DArrayShadow localShadowMap;
layout(set = 3, binding = 3) uniform sampler2D softDepth;

layout(std140, set = 1, binding = 0) uniform ParticleMaterialUniform
{
    vec4 uvScaleOffset;
    vec4 textureFlags;
    vec4 materialParams;
} material;
layout(set = 1, binding = 1) uniform sampler2D albedoMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
layout(set = 1, binding = 3) uniform sampler2D ormMap;
layout(set = 1, binding = 4) uniform sampler2D emissiveMap;

layout(std430, set = 0, binding = 5) buffer ParticleDebug
{
    uint fragments;
    uint reserved;
} particleDebug;

void main()
{
    float sceneViewDepth = SampleParticleSoftViewDepth(
        softDepth,
        sceneDepth,
        gl_FragCoord.xy,
        frame.softParams.z,
        frame.invProjection,
        frame.softParams.w);
    if (vViewDepth >= sceneViewDepth)
    {
        discard;
    }
    float softFade = 1.0;
    if (frame.softParams.y > 0.5)
    {
        softFade = ParticleSoftFade(
            sceneViewDepth - vViewDepth,
            frame.softParams.x,
            frame.softParams.w);
    }

    vec3 baseColor = vColor.rgb;
    if (frame.materialPbr.z > 0.5)
    {
        baseColor *= frame.materialBaseColor.rgb;
    }
    vec2 uv = vUv * material.uvScaleOffset.xy +
        material.uvScaleOffset.zw;
    vec4 albedo = vec4(1.0);
    if (material.textureFlags.x > 0.5)
    {
        albedo = texture(albedoMap, uv);
        baseColor *= albedo.rgb;
    }
    float opacity = vColor.a * softFade * albedo.a;
    if (frame.materialPbr.z > 0.5)
    {
        opacity *= clamp(frame.materialBaseColor.a, 0.0, 1.0);
    }
    float dissolveVisibility = 1.0;
    float dissolveEdge = 0.0;
    EvaluateParticleDissolve(
        vWorldPosition,
        vLifeT,
        vSeed,
        dissolveVisibility,
        dissolveEdge);
    opacity *= dissolveVisibility;
    if (opacity <= 1.0e-4 ||
        (material.materialParams.x > 1.0e-3 &&
         opacity < material.materialParams.x))
    {
        discard;
    }

    vec3 n = normalize(vNormal);
    vec3 v = normalize(frame.cameraPosition.xyz - vWorldPosition);
    float roughness = clamp(
        frame.materialPbr.z > 0.5
            ? frame.materialPbr.x : 0.55,
        0.04,
        1.0);
    float metallic = clamp(
        frame.materialPbr.z > 0.5
            ? frame.materialPbr.y : 0.0,
        0.0,
        1.0);
    float ambientOcclusion =
        clamp(material.materialParams.y, 0.0, 1.0);
    if (material.textureFlags.z > 0.5)
    {
        vec4 orm = texture(ormMap, uv);
        roughness = clamp(roughness * orm.r, 0.04, 1.0);
        metallic = clamp(metallic * orm.g, 0.0, 1.0);
        ambientOcclusion *= clamp(orm.b, 0.0, 1.0);
    }
    float shadow = SampleParticleCsmShadow(
        shadowMap,
        vWorldPosition,
        n);
    ParticleLightingLobes lobes = CalculateParticleMeshSceneLighting(
        localShadowMap,
        n,
        vWorldPosition,
        frame.cameraPosition.xyz,
        baseColor,
        roughness,
        metallic,
        shadow);
    vec3 f0 = mix(vec3(0.04), baseColor, metallic);
    float ambientScale = clamp(frame.meshLightParams.y, 0.0, 1.0);
    vec3 diffuseIbl = ParticleSampleDiffuseIbl(n, baseColor);
    vec3 specularIbl = ParticleEvaluateSpecularIbl(
        n, v, f0, roughness);
    vec3 lit =
        lobes.diffuse +
        lobes.specular +
        (baseColor + diffuseIbl) * ambientScale * ambientOcclusion +
        specularIbl * ambientScale +
        ParticleSampleTranslucencyVolume(vWorldPosition) * ambientScale;
    if (frame.materialPbr.z > 0.5)
    {
        vec3 emissive = frame.materialEmissiveStrength.rgb;
        if (material.textureFlags.w > 0.5)
        {
            emissive *= texture(emissiveMap, uv).rgb;
        }
        lit += emissive * frame.materialEmissiveStrength.w;
    }
    lit += frame.dissolveEdgeColor.rgb *
        frame.dissolveParams2.x *
        dissolveEdge;
    outColor = vec4(lit, opacity) * frame.colorTint;
    if (outColor.a > 0.0)
        atomicAdd(particleDebug.fragments, 1u);
}

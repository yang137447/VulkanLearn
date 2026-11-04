#version 450

#include "particleCommon.glsl"
#include "particleSceneLighting.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 2) in float vViewDepth;
layout(location = 4) in vec3 vNormal;
layout(location = 5) in vec3 vWorldPosition;

layout(location = 0) out vec4 outColor;

// particle pass 只写 sceneColor；sceneDepth 是图输入，不能同时作为附件。
layout(set = 3, binding = 0) uniform sampler2D sceneDepth;
// CSM shadowMap 是 particle pass 的第二个图输入（binding 1）。
layout(set = 3, binding = 1) uniform sampler2DArrayShadow shadowMap;
// 局部光阴影图集是 particle pass 的第三个图输入（binding 2）。
layout(set = 3, binding = 2) uniform sampler2DArrayShadow localShadowMap;
// 与 billboard/ribbon 共用 set 0 binding 2；v1 不采样，只用尺寸门控
// 让反射出的 stageFlags 保持 Vertex|Fragment，descriptor set 可复用。
layout(set = 0, binding = 2) uniform sampler2D particleTexture;
layout(std430, set = 0, binding = 5) buffer ParticleDebug
{
    uint fragments;
    uint reserved;
} particleDebug;

void main()
{
    float deviceDepth = texture(
        sceneDepth,
        gl_FragCoord.xy / vec2(textureSize(sceneDepth, 0))).r;
    vec4 sceneView = frame.invProjection * vec4(0.0, 0.0, deviceDepth, 1.0);
    float sceneViewDepth = -sceneView.z / sceneView.w;
    if (vViewDepth >= sceneViewDepth)
    {
        discard;
    }

    float softFade = 1.0;
    if (frame.softParams.y > 0.5)
    {
        softFade = clamp(
            (sceneViewDepth - vViewDepth) / max(frame.softParams.x, 0.001),
            0.0,
            1.0);
    }

    // 材质 v1 子集：baseColor 乘粒子颜色曲线；roughness/metallic 驱动
    // 完整场景光照下的 GGX 高光，但仍不接入 Material 图/纹理/宏。
    vec3 baseColor = vColor.rgb;
    if (frame.materialPbr.z > 0.5)
    {
        baseColor *= frame.materialBaseColor.rgb;
    }
    outColor = vec4(baseColor, vColor.a);
    outColor.a *= softFade;
    if (frame.materialPbr.z > 0.5)
    {
        outColor.a *= clamp(frame.materialBaseColor.a, 0.0, 1.0);
    }
    outColor.rgb *= clamp(
        float(textureSize(particleTexture, 0).x),
        0.0,
        1.0);

    if (frame.meshLightParams.x > 0.5)
    {
        vec3 n = normalize(vNormal);
        vec3 v = normalize(frame.cameraPosition.xyz - vWorldPosition);
        float roughness = clamp(frame.materialPbr.x, 0.04, 1.0);
        float metallic = clamp(frame.materialPbr.y, 0.0, 1.0);
        float shadow = SampleParticleCsmShadow(
            shadowMap,
            vWorldPosition,
            n);
        // 与主 DefaultLit 路径同公式：全部方向光/点光/聚光 + GPU 粒子灯，
        // CSM 只作用于方向光（当前只烘焙主方向光的级联）。
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
            n,
            v,
            f0,
            roughness);
        // 常量 ambient 保留为环境缺失（黑 cubemap）时的下限，IBL 作为额外
        // 环境项；两者共用 meshLightParams.y，与原 ambient 语义连续。
        outColor.rgb =
            lobes.diffuse +
            lobes.specular +
            (baseColor + diffuseIbl + specularIbl) * ambientScale;
    }
    if (frame.materialPbr.z > 0.5)
    {
        outColor.rgb +=
            frame.materialEmissiveStrength.rgb *
            frame.materialEmissiveStrength.w;
    }
    outColor *= frame.colorTint;
    if (outColor.a > 0.0)
        atomicAdd(particleDebug.fragments, 1u);
}

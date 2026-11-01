#version 450

#include "particleCommon.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 2) in float vViewDepth;
layout(location = 4) in vec3 vNormal;
layout(location = 5) in vec3 vWorldPosition;

layout(location = 0) out vec4 outColor;

// particle pass 只写 sceneColor；sceneDepth 是图输入，不能同时作为附件。
layout(set = 3, binding = 0) uniform sampler2D sceneDepth;
// CSM shadowMap 是 particle pass 的第二个图输入（binding 1）。
layout(set = 3, binding = 1) uniform sampler2DArrayShadow shadowMap;
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

    // 材质 v1 子集：baseColor 乘粒子颜色曲线；roughness/metallic 只驱动
    // 一个方向光的 GGX 高光，不接入完整 Material 图/纹理/宏。
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
        vec3 l = normalize(frame.meshLightDirection.xyz);
        vec3 v = normalize(frame.cameraPosition.xyz - vWorldPosition);
        float ndl = max(dot(n, l), 0.0);
        float ndv = max(dot(n, v), 1.0e-4);
        float roughness = clamp(frame.materialPbr.x, 0.04, 1.0);
        float metallic = clamp(frame.materialPbr.y, 0.0, 1.0);
        float alpha = roughness * roughness;
        vec3 halfVector = normalize(l + v);
        float ndh = max(dot(n, halfVector), 0.0);
        float vdh = max(dot(v, halfVector), 0.0);
        float alphaSquared = alpha * alpha;
        float denominator =
            ndh * ndh * (alphaSquared - 1.0) + 1.0;
        float distribution = alphaSquared /
            max(3.14159265 * denominator * denominator, 1.0e-6);
        float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
        float geometry =
            (ndl / (ndl * (1.0 - k) + k)) *
            (ndv / (ndv * (1.0 - k) + k));
        vec3 f0 = mix(vec3(0.04), baseColor, metallic);
        vec3 fresnel =
            f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
        vec3 diffuse = baseColor * (1.0 - metallic);
        vec3 specular = distribution * geometry * fresnel /
            max(4.0 * ndl * ndv, 1.0e-5);
        float shadow = SampleParticleCsmShadow(
            shadowMap,
            vWorldPosition,
            n);
        vec3 direct = (diffuse / 3.14159265 + specular) *
            frame.meshLightColor.rgb * ndl * shadow;
        vec3 ambient = baseColor * clamp(frame.meshLightParams.y, 0.0, 1.0);
        outColor.rgb = direct + ambient;
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

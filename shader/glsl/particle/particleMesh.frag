#version 450

#include "particleCommon.glsl"
#include "particleSceneLighting.glsl"
#include "particleSoftDepth.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 2) in float vViewDepth;
layout(location = 4) in vec3 vNormal;
layout(location = 5) in vec3 vWorldPosition;
layout(location = 6) in vec3 vTangent;
layout(location = 7) in float vTangentSign;
layout(location = 8) in vec2 vUv;

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
// mesh 专用材质 set：albedo / 切线空间法线 / ORM / emissive。没有 MI 时
// CPU 仍绑定默认纹理与恒等 UV，数值材质路径保持原样。
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

// 顶点切线缺失时用屏幕导数重建切线；UV 退化则回退到法线的任意正交轴，
// 保证 TBN 始终是有限正交基。handedness 由 UV 行列式符号给出，与
// MikkTSpace 的 tangent.w 语义一致。
vec3 ReconstructParticleTangent(
    vec3 worldNormal,
    vec3 worldPosition,
    out float tangentSign)
{
    vec3 dpdx = dFdx(worldPosition);
    vec3 dpdy = dFdy(worldPosition);
    vec2 duvdx = dFdx(vUv);
    vec2 duvdy = dFdy(vUv);
    float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(determinant) > 1.0e-8)
    {
        vec3 tangent = (dpdx * duvdy.y - dpdy * duvdx.y) *
            sign(determinant);
        if (dot(tangent, tangent) > 1.0e-12)
        {
            tangentSign = determinant < 0.0 ? -1.0 : 1.0;
            return normalize(tangent);
        }
    }
    vec3 axis = abs(worldNormal.y) < 0.99
        ? vec3(0.0, 1.0, 0.0)
        : vec3(1.0, 0.0, 0.0);
    tangentSign = 1.0;
    return normalize(cross(axis, worldNormal));
}

void main()
{
    float sceneViewDepth = SampleParticleSceneViewDepth(
        sceneDepth,
        gl_FragCoord.xy,
        frame.softParams.z,
        frame.invProjection);
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

    // 材质 v1 子集：baseColor 乘粒子颜色曲线与 albedo 贴图；
    // roughness/metallic 驱动完整场景光照下的 GGX 高光。
    vec3 baseColor = vColor.rgb;
    if (frame.materialPbr.z > 0.5)
    {
        baseColor *= frame.materialBaseColor.rgb;
    }
    vec4 albedoSample = vec4(1.0);
    bool hasAlbedoMap = material.textureFlags.x > 0.5;
    if (hasAlbedoMap)
    {
        albedoSample = texture(albedoMap, vUv);
        baseColor *= albedoSample.rgb;
    }

    float opacity = vColor.a * softFade;
    if (frame.materialPbr.z > 0.5)
    {
        opacity *= clamp(frame.materialBaseColor.a, 0.0, 1.0);
    }
    if (hasAlbedoMap)
    {
        opacity *= albedoSample.a;
    }
    // u_alphaClipThreshold=0 关闭裁剪；大于 0 时对最终覆盖率做 alpha test。
    float alphaClipThreshold = material.materialParams.x;
    if (alphaClipThreshold > 1.0e-3 && opacity < alphaClipThreshold)
    {
        discard;
    }
    outColor = vec4(baseColor, opacity);
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
        float ambientOcclusion =
            clamp(material.materialParams.y, 0.0, 1.0);
        if (material.textureFlags.z > 0.5)
        {
            vec4 orm = texture(ormMap, vUv);
            roughness = clamp(roughness * orm.r, 0.04, 1.0);
            metallic = clamp(metallic * orm.g, 0.0, 1.0);
            ambientOcclusion *= clamp(orm.b, 0.0, 1.0);
        }
        if (material.textureFlags.y > 0.5)
        {
            vec3 tangent = vTangent;
            float tangentSign = vTangentSign;
            if (dot(tangent, tangent) < 1.0e-10)
            {
                tangent = ReconstructParticleTangent(
                    n,
                    vWorldPosition,
                    tangentSign);
            }
            tangent = normalize(tangent - n * dot(n, tangent));
            vec3 bitangent = normalize(cross(n, tangent)) *
                (tangentSign < 0.0 ? -1.0 : 1.0);
            vec3 normalTs = texture(normalMap, vUv).xyz * 2.0 - 1.0;
            normalTs = normalize(mix(
                vec3(0.0, 0.0, 1.0),
                normalTs,
                clamp(material.materialParams.z, 0.0, 1.0)));
            n = normalize(mat3(tangent, bitangent, n) * normalTs);
        }
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
        // AO 只作用于间接漫反射，直接光与镜面 IBL 保持能量。
        outColor.rgb =
            lobes.diffuse +
            lobes.specular +
            (baseColor + diffuseIbl) * ambientScale * ambientOcclusion +
            specularIbl * ambientScale;
    }
    if (frame.materialPbr.z > 0.5)
    {
        vec3 emissive = frame.materialEmissiveStrength.rgb;
        if (material.textureFlags.w > 0.5)
        {
            emissive *= texture(emissiveMap, vUv).rgb;
        }
        outColor.rgb += emissive * frame.materialEmissiveStrength.w;
    }
    outColor *= frame.colorTint;
    if (outColor.a > 0.0)
        atomicAdd(particleDebug.fragments, 1u);
}

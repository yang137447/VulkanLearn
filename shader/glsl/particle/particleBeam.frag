#version 450

#include "particleCommon.glsl"
#include "particleSprite.glsl"
#include "particleSoftDepth.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vSoftness;
layout(location = 3) in float vSeed;
layout(location = 4) in float vTime;
layout(location = 5) in vec2 vAtlasUV;
layout(location = 6) in float vViewDepth;
layout(location = 7) in vec3 vWorldPosition;

layout(location = 0) out vec4 outColor;

// 与 ribbon 相同的 particle pass set 3 输入。
layout(set = 3, binding = 0) uniform sampler2D sceneDepth;
layout(set = 3, binding = 1) uniform sampler2DArrayShadow shadowMap;
layout(std430, set = 0, binding = 5) buffer ParticleDebug
{
    uint fragments;
    uint reserved;
} particleDebug;

void main()
{
    // 与 ribbon 相同的 sceneDepth/软粒子逻辑。
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
            (sceneViewDepth - vViewDepth) /
                max(frame.softParams.x, 0.001),
            0.0,
            1.0);
    }

    // 电弧横截面：中心亮核 + 两侧快速衰减，不做 FBM 侵蚀，保持干净的
    // 放电轮廓。vSoftness 仍控制边缘宽度。
    float edgeWidth = mix(0.03, 0.45, clamp(vSoftness, 0.0, 1.0));
    float crossSection = 1.0 - abs(vUV.y * 2.0 - 1.0);
    float edge = smoothstep(0.0, edgeWidth, crossSection);
    float core = pow(clamp(crossSection, 0.0, 1.0), 6.0);
    vec3 color = vColor.rgb * (0.55 + 0.85 * core);
    float alpha = vColor.a * edge;
    if (frame.flipbookParams.y > 0.5)
    {
        vec4 sprite = texture(particleTexture, vAtlasUV);
        if (frame.textureModeParams.x > 0.5)
        {
            color +=
                vColor.rgb * sprite.r * frame.textureModeParams.y;
        }
        else
        {
            color *= sprite.rgb;
        }
        alpha *= sprite.a;
    }
    outColor = vec4(color, alpha * softFade);

    if (frame.shadowParams.w > 0.5)
    {
        vec3 facingNormal =
            normalize(frame.cameraPosition.xyz - vWorldPosition);
        float shadow = SampleParticleCsmShadow(
            shadowMap,
            vWorldPosition,
            facingNormal);
        outColor.rgb *= mix(1.0, shadow, 0.5);
    }
    outColor *= frame.colorTint;
    if (outColor.a > 0.0)
        atomicAdd(particleDebug.fragments, 1u);
}

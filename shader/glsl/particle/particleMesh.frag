#version 450

#include "particleCommon.glsl"

layout(location = 0) in vec4 vColor;
layout(location = 2) in float vViewDepth;

layout(location = 0) out vec4 outColor;

// particle pass 只写 sceneColor；sceneDepth 是图输入，不能同时作为附件。
layout(set = 3, binding = 0) uniform sampler2D sceneDepth;
// 与 billboard/ribbon 共用 set 0 binding 2；v1 不采样，只用尺寸门控
// 让反射出的 stageFlags 保持 Vertex|Fragment，descriptor set 可复用。
layout(set = 0, binding = 2) uniform sampler2D particleTexture;

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

    // v1 不接材质/纹理：颜色完全来自粒子颜色曲线与网格顶点色。
    outColor = vColor;
    outColor.a *= softFade;
    outColor.rgb *= clamp(
        float(textureSize(particleTexture, 0).x),
        0.0,
        1.0);
    outColor *= frame.colorTint;
}

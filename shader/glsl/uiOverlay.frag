#version 450

layout(set = 0, binding = 0) uniform sampler2D uiTexture;
layout(set = 1, binding = 0) uniform sampler2D sceneDepth;
layout(push_constant) uniform DebugViewport { vec4 rect; } debugViewport;

layout(location = 0) in vec2 inTexCoord;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inDebugDepth;

layout(location = 0) out vec4 outColor;

void main()
{
    if (inDebugDepth >= 0.0)
    {
        // 场景成图为完整 backing extent，编辑器只缩放显示；按逻辑视口反算深度 UV。
        // 普通 UI 使用 -1 哨兵；缺失场景深度时隐藏 DepthTest 项，不降级为穿透绘制。
        if (debugViewport.rect.z <= 0.0) discard;
        vec2 uv = (gl_FragCoord.xy - debugViewport.rect.xy) / debugViewport.rect.zw;
        float depth = texelFetch(sceneDepth, ivec2(uv * vec2(textureSize(sceneDepth, 0))), 0).r;
        // device Z 在屏幕上仿射插值；不写深度，调试图形彼此不会成为遮挡物。
        if (inDebugDepth > depth) discard;
    }
    outColor = texture(uiTexture, inTexCoord) * inColor;
}

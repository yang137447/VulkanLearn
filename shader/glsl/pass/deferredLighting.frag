#version 450

#include "generate/M_deferredLightingParamter.glsl"
#include "../engine/gbufferCodec.glsl"
#include "../engine/deferredLighting.glsl"
#include "../engine/materialDebugView.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

// Set 3 是 RenderGraph 为“当前 pass 输入”预留的 descriptor set。
// 这里的 binding 顺序必须和 config/renderGraphConfig.json 中 deferredLighting.input 完全一致。
layout(set = 3, binding = 0) uniform sampler2D gbufferA;
layout(set = 3, binding = 1) uniform sampler2D gbufferB;
layout(set = 3, binding = 2) uniform sampler2D gbufferC;
layout(set = 3, binding = 3) uniform sampler2D gbufferD;
layout(set = 3, binding = 4) uniform sampler2D gbufferE;
layout(set = 3, binding = 5) uniform sampler2D gbufferVelocity;
layout(set = 3, binding = 6) uniform sampler2D gbufferF;
layout(set = 3, binding = 7) uniform sampler2D sceneColorBase;
layout(set = 3, binding = 8) uniform sampler2D sceneDepth;
layout(set = 3, binding = 9) uniform sampler2DArrayShadow shadowMap;

GBufferData SampleGBuffer(vec2 uv)
{
    GBufferData data;
    data.gbufferA = texture(gbufferA, uv);
    data.gbufferB = texture(gbufferB, uv);
    data.gbufferC = texture(gbufferC, uv);
    data.gbufferD = texture(gbufferD, uv);
    data.gbufferE = texture(gbufferE, uv);
    data.gbufferVelocity = texture(gbufferVelocity, uv);
    data.gbufferF = texture(gbufferF, uv);
    data.sceneColorBase = texture(sceneColorBase, uv);
    return data;
}

void main()
{
    GBufferData gbuffer = SampleGBuffer(inUV);
    MaterialSurface surface = DecodeGBufferSurface(gbuffer);

    float deviceDepth = texture(sceneDepth, inUV).r;
    surface.worldPosition = ReconstructWorldPositionFromSceneDepth(inUV, deviceDepth);

    DeferredLightingResult lighting = ShadeDeferredSurfaceDetailed(surface, shadowMap);
    vec4 finalColor = vec4(lighting.finalColor, surface.opacity);
    MaterialDebugLightingData debugLighting = CreateMaterialDebugLightingData(
        lighting.shadow,
        lighting.shadowCascadeIndex,
        lighting.directLighting,
        lighting.indirectDiffuse,
        lighting.indirectSpecular);
    outColor = ResolveMaterialDebugView(surface, debugLighting, finalColor);
}

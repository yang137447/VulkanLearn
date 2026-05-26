#version 450

#include "common/shadingModel.glsl"
#include "engine/materialSurface.glsl"
#include "engine/gbufferCodec.glsl"

//layout(binding = 4) uniform sampler2D albedoMap;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldPosition;
layout(location = 3) in vec3 fragWorldNormal;

layout(location = 0) out vec4 outGBufferA;
layout(location = 1) out vec4 outGBufferB;
layout(location = 2) out vec4 outGBufferC;
layout(location = 3) out vec4 outGBufferD;
layout(location = 4) out vec4 outGBufferE;
layout(location = 5) out vec4 outGBufferVelocity;
layout(location = 6) out vec4 outGBufferF;
layout(location = 7) out vec4 outSceneColorBase;

void main()
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.worldPosition = fragWorldPosition;
    surface.worldNormal = normalize(fragWorldNormal);
    surface.baseColor = fragColor;
    surface.opacity = 1.0;
    surface.shadingModel = SHADING_MODEL_UNLIT;

    GBufferData gbuffer = EncodeGBuffer(surface, CreateDefaultGBufferPixelData());
    outGBufferA = gbuffer.gbufferA;
    outGBufferB = gbuffer.gbufferB;
    outGBufferC = gbuffer.gbufferC;
    outGBufferD = gbuffer.gbufferD;
    outGBufferE = gbuffer.gbufferE;
    outGBufferVelocity = gbuffer.gbufferVelocity;
    outGBufferF = gbuffer.gbufferF;
    outSceneColorBase = gbuffer.sceneColorBase;
}

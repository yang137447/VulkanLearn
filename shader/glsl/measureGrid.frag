#version 450

#include "common/shadingModel.glsl"
#include "engine/materialSurface.glsl"
#include "engine/gbufferCodec.glsl"
#include "materialFunction/mf_measureGrid.glsl"

layout(location = 0) in vec3 v2fPosition;
layout(location = 1) in vec3 v2fNormal;
layout(location = 2) in vec3 v2fColor;
layout(location = 3) in vec2 v2fTexCoord;

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
    const vec4 baseColor = vec4(0.5, 0.5, 0.5, 1.0);
    const vec4 gridColor = vec4(0.0, 0.0, 0.0, 1.0);
    const vec4 subGridColor = vec4(0.2, 0.2, 0.2, 1.0);

    float subGridMask = CalculateMeasureGridMask(v2fPosition, v2fNormal, 0.2);
    vec4 albedo = mix(baseColor, subGridColor, subGridMask);

    float gridMask = CalculateMeasureGridMask(v2fPosition, v2fNormal, 1.0);
    albedo = mix(albedo, gridColor, gridMask);

    float roughness = mix(1.0, 0.5, max(gridMask, subGridMask));
    float metallic = 0.0;
    vec3 normal = normalize(v2fNormal);

    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.worldPosition = v2fPosition;
    surface.worldNormal = normal;
    surface.baseColor = albedo.rgb;
    surface.opacity = albedo.a;
    surface.roughness = roughness;
    surface.metallic = metallic;
    surface.ambientOcclusion = 1.0;
    surface.shadingModel = SHADING_MODEL_DEFAULT_LIT;

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

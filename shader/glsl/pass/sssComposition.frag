#version 450

#include "generate/M_sssCompositionParamter.glsl"
#include "../engine/gbufferCodec.glsl"
#include "../engine/subsurfaceProfileFilter.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform sampler2D diffuseLighting;
layout(set = 3, binding = 1) uniform sampler2D nonDiffuseLighting;
layout(set = 3, binding = 2) uniform sampler2D transmissionLighting;
layout(set = 3, binding = 3) uniform sampler2D sssSource;
layout(set = 3, binding = 4) uniform sampler2D sssPong;
layout(set = 3, binding = 5) uniform sampler2D gbufferB;
layout(set = 3, binding = 6) uniform sampler2D gbufferD;
layout(set = 3, binding = 7) uniform sampler2D sceneDepth;

void main()
{
    vec4 diffuse = texture(diffuseLighting, inUV);
    vec4 filteredDiffuse = texture(sssPong, inUV);
    vec3 sssSourceColor = texture(sssSource, inUV).rgb;
    vec3 nonDiffuse = texture(nonDiffuseLighting, inUV).rgb;
    vec3 transmission = texture(transmissionLighting, inUV).rgb;
    // GBuffer 的 shading model、profile ID 和 profile weight 是离散编码，
    // composition 也必须按 texel 读取，不能让线性 sampler 在物体边界插值出伪 ID。
    ivec2 gbufferBCoordinate = GetNearestSubsurfaceProfileTexel(
        inUV,
        textureSize(gbufferB, 0));
    ivec2 gbufferDCoordinate = GetNearestSubsurfaceProfileTexel(
        inUV,
        textureSize(gbufferD, 0));
    uint shadingModel = DecodeGBufferShadingModel(
        texelFetch(gbufferB, gbufferBCoordinate, 0).a);
    vec4 customData = texelFetch(
        gbufferD,
        gbufferDCoordinate,
        0);

    // 1..17 的 debug view 在 deferred lighting 已经求出颜色，这里只负责通路合成。
    if ((uboVP.debugViewMode >= 1 &&
         uboVP.debugViewMode <= 17) ||
        (uboVP.debugViewMode >= 21 &&
         uboVP.debugViewMode <= 41))
    {
        outColor = vec4(
            diffuse.rgb + nonDiffuse + transmission,
            diffuse.a);
        return;
    }
    // 18/19/20 专门观察 profile filter 的输出、像素半径和有效权重。
    if (uboVP.debugViewMode == 18)
    {
        outColor = vec4(filteredDiffuse.rgb, 1.0);
        return;
    }
    if (uboVP.debugViewMode == 19)
    {
        float pixelRadius = 0.0;
        if (shadingModel == SHADING_MODEL_SUBSURFACE_PROFILE)
        {
            pixelRadius = CalculateSubsurfaceProfilePixelRadius(
                inUV,
                sceneDepth,
                subsurfaceProfileTable,
                int(customData.x + 0.5),
                textureSize(diffuseLighting, 0).y);
        }
        else if (shadingModel == SHADING_MODEL_EYE)
        {
            uint packedEye = uint(customData.z + 0.5);
            if (IsEyePackedProfileVersionValid(packedEye))
            {
                pixelRadius = CalculateSubsurfaceProfilePixelRadius(
                    inUV,
                    sceneDepth,
                    subsurfaceProfileTable,
                    int((packedEye >> 4u) & 0x0fu),
                    textureSize(diffuseLighting, 0).y);
            }
        }
        outColor = vec4(
            vec3(pixelRadius / u_sssDiagnosticsParameters.x),
            1.0);
        return;
    }
    if (uboVP.debugViewMode == 20)
    {
        float validWeight = 0.0;
        if (shadingModel == SHADING_MODEL_SUBSURFACE_PROFILE ||
            shadingModel == SHADING_MODEL_EYE)
        {
            validWeight = filteredDiffuse.a;
        }
        outColor = vec4(vec3(validWeight), 1.0);
        return;
    }

    // profile 只混合 diffuse；specular、emissive 和 transmission 保持未过滤。
    vec3 resolvedDiffuse = diffuse.rgb;
    if (shadingModel == SHADING_MODEL_SUBSURFACE_PROFILE)
    {
        resolvedDiffuse = mix(
            diffuse.rgb,
            filteredDiffuse.rgb,
            customData.y);
    }
    else if (shadingModel == SHADING_MODEL_EYE)
    {
        // Eye 的 filter source 只包含 sclera tissue；虹膜/角膜保持原始 diffuse/specular。
        float scleraWeight = 1.0 - customData.w;
        vec3 filteredSclera = mix(
            sssSourceColor,
            filteredDiffuse.rgb,
            scleraWeight);
        resolvedDiffuse = diffuse.rgb - sssSourceColor + filteredSclera;
    }
    outColor = vec4(
        resolvedDiffuse + nonDiffuse + transmission,
        diffuse.a);
}

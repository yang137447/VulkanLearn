#ifndef VL_ENGINE_SUBSURFACE_PROFILE_FILTER_GLSL
#define VL_ENGINE_SUBSURFACE_PROFILE_FILTER_GLSL

#include "../common/commonUbo.glsl"
#include "gbufferCodec.glsl"
#include "deferredLighting.glsl"

const int SUBSURFACE_PROFILE_TAP_COUNT = 13;

ivec2 GetNearestSubsurfaceProfileTexel(
    in vec2 uv,
    in ivec2 textureExtent)
{
    // GBuffer 的 ID、法线和深度必须按离散 texel 读取，不能让线性 sampler
    // 把相邻物体的编码值混成一个新的 rejection 输入。
    return clamp(
        ivec2(uv * vec2(textureExtent)),
        ivec2(0),
        textureExtent - ivec2(1));
}

float CalculateSubsurfaceProfilePixelRadius(
    in vec2 uv,
    in sampler2D sceneDepthTexture,
    in sampler2D profileTableTexture,
    int profileId,
    int textureHeight)
{
    // profile metadata 保存世界半径；这里按当前 view depth 和 viewport 高度换算为像素半径。
    ivec2 depthExtent = textureSize(sceneDepthTexture, 0);
    float deviceDepth = texelFetch(
        sceneDepthTexture,
        GetNearestSubsurfaceProfileTexel(uv, depthExtent),
        0).r;
    vec3 worldPosition = ReconstructWorldPositionFromSceneDepth(
        uv,
        deviceDepth);
    float viewDepth = abs(
        (uboVP.view * vec4(worldPosition, 1.0)).z);
    float worldRadius = texelFetch(
        profileTableTexture,
        ivec2(0, profileId),
        0).w;
    return
        0.5 * float(textureHeight) *
        uboVP.projection[1][1] /
        viewDepth *
        worldRadius;
}

vec4 FilterSubsurfaceProfile(
    in vec2 uv,
    in vec2 filterDirection,
    in sampler2D sourceLighting,
    in sampler2D gbufferBTexture,
    in sampler2D gbufferDTexture,
    in sampler2D sceneDepthTexture,
    in sampler2D profileTableTexture,
    in vec4 filterParameters)
{
    ivec2 sourceExtent = textureSize(sourceLighting, 0);
    ivec2 gbufferBExtent = textureSize(gbufferBTexture, 0);
    ivec2 gbufferDExtent = textureSize(gbufferDTexture, 0);
    ivec2 depthExtent = textureSize(sceneDepthTexture, 0);
    ivec2 centerSourceCoordinate =
        GetNearestSubsurfaceProfileTexel(uv, sourceExtent);
    ivec2 centerGBufferBCoordinate =
        GetNearestSubsurfaceProfileTexel(uv, gbufferBExtent);
    ivec2 centerGBufferDCoordinate =
        GetNearestSubsurfaceProfileTexel(uv, gbufferDExtent);
    ivec2 centerDepthCoordinate =
        GetNearestSubsurfaceProfileTexel(uv, depthExtent);
    vec4 centerLighting = texelFetch(
        sourceLighting,
        centerSourceCoordinate,
        0);
    vec4 centerGBufferB = texelFetch(
        gbufferBTexture,
        centerGBufferBCoordinate,
        0);
    vec4 centerGBufferD = texelFetch(
        gbufferDTexture,
        centerGBufferDCoordinate,
        0);
    uint centerShadingModel = DecodeGBufferShadingModel(
        centerGBufferB.a);
    int centerProfileId = int(
        centerGBufferD.x + 0.5);
    // 只有 ID 5 进入邻域 filter；其它模型必须原样通过，避免跨模型污染。
    if (centerShadingModel != SHADING_MODEL_SUBSURFACE_PROFILE ||
        (uboVP.debugViewMode >= 1 && uboVP.debugViewMode <= 17))
    {
        return centerLighting;
    }

    float pixelRadius = CalculateSubsurfaceProfilePixelRadius(
        uv,
        sceneDepthTexture,
        profileTableTexture,
        centerProfileId,
        sourceExtent.y);
    vec3 centerNormal = DecodeGBufferDirection(centerGBufferB.rgb);
    float centerDepth = texelFetch(
        sceneDepthTexture,
        centerDepthCoordinate,
        0).r;
    vec3 weightedLighting = vec3(0.0);
    vec3 validWeight = vec3(0.0);

    for (int tapIndex = 0;
         tapIndex < SUBSURFACE_PROFILE_TAP_COUNT;
         ++tapIndex)
    {
        vec4 kernelTap = texelFetch(
            profileTableTexture,
            ivec2(tapIndex + 1, centerProfileId),
            0);
        vec2 sampleUv =
            uv +
            filterDirection * kernelTap.a * pixelRadius /
                vec2(sourceExtent);
        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 ||
            sampleUv.y < 0.0 || sampleUv.y > 1.0)
        {
            continue;
        }

        ivec2 sampleSourceCoordinate =
            GetNearestSubsurfaceProfileTexel(sampleUv, sourceExtent);
        ivec2 sampleGBufferBCoordinate =
            GetNearestSubsurfaceProfileTexel(sampleUv, gbufferBExtent);
        ivec2 sampleGBufferDCoordinate =
            GetNearestSubsurfaceProfileTexel(sampleUv, gbufferDExtent);
        ivec2 sampleDepthCoordinate =
            GetNearestSubsurfaceProfileTexel(sampleUv, depthExtent);
        vec4 sampleGBufferB = texelFetch(
            gbufferBTexture,
            sampleGBufferBCoordinate,
            0);
        uint sampleShadingModel = DecodeGBufferShadingModel(
            sampleGBufferB.a);
        int sampleProfileId = int(
            texelFetch(
                gbufferDTexture,
                sampleGBufferDCoordinate,
                0).x + 0.5);
        vec3 sampleNormal = DecodeGBufferDirection(
            sampleGBufferB.rgb);
        float sampleDepth = texelFetch(
            sceneDepthTexture,
            sampleDepthCoordinate,
            0).r;
        // depth、normal、shading model 和 profile ID 四重 rejection，防止屏幕空间跨表面扩散。
        bool sameSurface =
            sampleShadingModel == SHADING_MODEL_SUBSURFACE_PROFILE &&
            sampleProfileId == centerProfileId &&
            dot(centerNormal, sampleNormal) >= filterParameters.y &&
            abs(centerDepth - sampleDepth) <= filterParameters.x;
        if (!sameSurface)
        {
            continue;
        }

        weightedLighting +=
            texelFetch(
                sourceLighting,
                sampleSourceCoordinate,
                0).rgb * kernelTap.rgb;
        validWeight += kernelTap.rgb;
    }

    // rejection 后按 RGB 独立重归一化，避免屏幕边缘或轮廓处因丢 tap 而变暗。
    vec3 filteredLighting = weightedLighting / validWeight;
    float averageValidWeight =
        dot(validWeight, vec3(1.0 / 3.0));
    return vec4(filteredLighting, averageValidWeight);
}

#endif

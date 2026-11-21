#version 450

#include "../common/commonUbo.glsl"
#include "../materialFunction/terrainCompositedFeedback.glsl"
#include "generate/M_terrainFeedbackParamter.glsl"

layout(location = 0) in vec2 inPageUv;
layout(location = 1) in vec2 inMaterialUv;
layout(location = 0) out vec4 outFeedback;
layout(location = 1) out vec4 outLayerSlot0;
layout(location = 2) out vec4 outLayerSlot1;
layout(location = 3) out vec4 outLayerSlot2;
layout(location = 4) out vec4 outLayerSlot3;
layout(location = 5) out uvec4 outQualityWinner0;
layout(location = 6) out uvec4 outQualityWinner1;

layout(set = 3, binding = 0) uniform sampler2D pageTable;
layout(set = 3, binding = 1) uniform sampler2D layerCatalog;
layout(set = 3, binding = 2) uniform sampler2D layerIds;
layout(set = 3, binding = 3) uniform sampler2D layerWeights;
layout(set = 3, binding = 4) uniform sampler2D compositedSurfaceMap;

void main()
{
    const int pageTableSize = int(u_pageTableSize + 0.5);
    const float pageInset = 0.5 / (u_pageTableSize * u_pageResolution);
    const vec2 pageUvGlobal = clamp(
        inPageUv, vec2(pageInset), vec2(1.0 - pageInset));
    const vec2 pageUv = pageUvGlobal * float(pageTableSize);
    const ivec2 page = ivec2(floor(pageUv));
    const vec4 compositedSurface = texelFetch(compositedSurfaceMap, page, 0);
    const vec2 compositedResolution = max(compositedSurface.gb, vec2(128.0));
    const TerrainCompositedFootprint compositedFootprint =
        TerrainCompositedFeedbackFootprintFromGradients(
            dFdx(inMaterialUv), dFdy(inMaterialUv),
            compositedResolution, compositedSurface.a,
            vec2(u_feedbackScaleX, u_feedbackScaleY));
    const float compositedMip = compositedFootprint.targetMip;
    const int compositedSurfaceIndex = max(0, int(round(compositedSurface.r)) - 1);
    const float pageDerivative = max(fwidth(pageUv.x), fwidth(pageUv.y));
    const float pagePixels = 1.0 / max(pageDerivative, 1.0e-4);
    const float coverage = clamp(log2(pagePixels + 1.0) / 8.0, 0.0, 1.0);
    uint priority = uint(round(255.0 * coverage));
    priority = min(priority + uint(step(2.0, uboM.selectionData.x)), 255u);
    const vec4 coveragePage = texelFetch(pageTable, page, 0);
    const bool miss = coveragePage.a < 0.5;
    if (miss)
        priority = max(priority, 192u);
    const uint frameTag = uint(round(clamp(u_frameTag, 1.0, 255.0)));
    const uint packedX = uint(page.x) | (miss ? 32u : 0u) | 64u;
    outFeedback = vec4(
        float(packedX), float(page.y), float(priority), float(frameTag)) / 255.0;

    const bool compositedValid = compositedSurface.r >= 1.0;
    outQualityWinner0 = uvec4(floatBitsToUint(inMaterialUv),
        floatBitsToUint(compositedFootprint.majorUv));
    outQualityWinner1 = uvec4(floatBitsToUint(compositedMip), floatBitsToUint(0.0),
        (compositedValid ? uint(compositedSurfaceIndex) : 65535u) |
            (uint(compositedFootprint.tapCount) << 16),
        floatBitsToUint(gl_FragCoord.z) | 0x80000000u);
    outLayerSlot0 = compositedValid ? TerrainEncodeCompositedFeedbackAddress(
        inMaterialUv, compositedResolution, compositedSurfaceIndex,
        int(floor(compositedMip))) : vec4(0.0);
    outLayerSlot1 = compositedValid
        ? TerrainEncodeCompositedFeedbackMetadata(frameTag) : vec4(0.0);
    outLayerSlot2 = compositedValid ? TerrainEncodeCompositedFeedbackAddress(
        inMaterialUv, compositedResolution, compositedSurfaceIndex,
        min(int(floor(compositedMip)) + 1, int(compositedSurface.a))) : vec4(0.0);
    outLayerSlot3 = compositedValid
        ? TerrainEncodeCompositedFeedbackMetadata(frameTag) : vec4(0.0);
}

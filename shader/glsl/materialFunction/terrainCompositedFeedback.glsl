#ifndef VL_TERRAIN_COMPOSITED_FEEDBACK_GLSL
#define VL_TERRAIN_COMPOSITED_FEEDBACK_GLSL

#include "terrainCompositedFootprint.glsl"

// feedbackScale=主视图尺寸/反馈尺寸；导数必须在逐片元分支之前无条件求出。
// 使用未 clamp 的局部材质 UV，避免地形边缘或覆盖虚拟页接缝破坏 mip 选择。
TerrainCompositedFootprint TerrainCompositedFeedbackFootprintFromGradients(
    vec2 uvDx, vec2 uvDy, vec2 resolution, float maxMip, vec2 feedbackScale)
{
    return TerrainCompositedFootprintFromGradients(
        uvDx / max(feedbackScale.x, 1.0e-6),
        uvDy / max(feedbackScale.y, 1.0e-6), resolution, maxMip);
}

float TerrainCompositedFeedbackMip(
    vec2 materialUv, vec2 resolution, float maxMip, vec2 feedbackScale)
{
    const TerrainCompositedFootprint footprint = TerrainCompositedFeedbackFootprintFromGradients(
        dFdx(materialUv), dFdy(materialUv), resolution, maxMip, feedbackScale);
    return footprint.targetMip;
}

vec4 TerrainEncodeCompositedFeedbackAddress(
    vec2 materialUv, vec2 resolution, int surfaceIndex, int mip)
{
    const vec2 extent = max(floor(resolution / exp2(float(mip))), vec2(1.0));
    // 与合成 VT clamp 采样一致：UV=1 取最后一个 texel，不回绕到第一页。
    const vec2 texel = clamp(
        clamp(materialUv, vec2(0.0), vec2(1.0)) * extent,
        vec2(0.5), extent - 0.5);
    const uvec2 page = uvec2(floor(texel / 128.0));
    const uint packedMip = uint(mip) | 128u;
    return vec4(float(surfaceIndex), float(page.x), float(page.y), float(packedMip)) / 255.0;
}

vec4 TerrainEncodeCompositedFeedbackMetadata(uint frameTag)
{
    // 版本与保留零字节独立于地址；默认 RT clear(.2,.2,.2,1) 不能成为有效请求。
    return vec4(float(frameTag), 1.0, 0.0, 0.0) / 255.0;
}

#endif

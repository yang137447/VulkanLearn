#ifndef VL_MF_TERRAIN_COMPOSITED_VT_GLSL
#define VL_MF_TERRAIN_COMPOSITED_VT_GLSL

#include "terrainCompositedFootprint.glsl"

struct TerrainCompositedVTResult
{
    vec4 albedo;
    vec3 normalWorld;
    vec4 orm;
    vec2 actualMips;
    float targetMip;
    float valid;
};

// 局部 UV 导数决定合成输出 mip；feedback 传入反馈 RT/主视图的尺寸比。
float TerrainCompositedTargetMip(
    vec2 localUv, vec2 resolution, float maxMip, vec2 derivativeScale)
{
    TerrainCompositedFootprint footprint = TerrainCompositedFootprintFromGradients(
        dFdx(localUv) * derivativeScale.x, dFdy(localUv) * derivativeScale.y,
        resolution, maxMip);
    return footprint.targetMip;
}

ivec2 TerrainCompositedMipExtent(vec2 resolution, int mip)
{
    return max(ivec2(floor(resolution / exp2(float(mip)))), ivec2(1));
}

vec2 TerrainCompositedTexel(vec2 uv, ivec2 extent, float repeatMode)
{
    vec2 addressed = repeatMode > 0.5 ? fract(uv) : clamp(uv, vec2(0.0), vec2(1.0));
    return repeatMode > 0.5 ? addressed * vec2(extent) :
        clamp(addressed * vec2(extent), vec2(0.5), vec2(extent) - 0.5);
}

// 页表每 array layer 对应一个虚拟 mip；0 缺页，slot+1 为有效物理层。
// 每级重新按 UV 算页地址，避免奇数/非正方形尺寸的父页边缘寻址错误。
bool TerrainCompositedAddress(sampler2DArray pageTable, vec2 uv, vec2 resolution,
    int requestedMip, int maxMip, float repeatMode, out vec3 atlasUv, out int actualMip)
{
    for (int mip = requestedMip; mip <= maxMip; ++mip)
    {
        ivec2 extent = TerrainCompositedMipExtent(resolution, mip);
        vec2 texel = TerrainCompositedTexel(uv, extent, repeatMode);
        ivec2 page = ivec2(floor(texel / 128.0));
        uint entry = uint(texelFetch(pageTable, ivec3(page, mip), 0).r + 0.5);
        if (entry != 0u)
        {
            atlasUv = vec3((texel - vec2(page) * 128.0 + 4.0) / 136.0, float(entry - 1u));
            actualMip = mip;
            return true;
        }
    }
    atlasUv = vec3(0.0);
    actualMip = maxMip;
    return false;
}

bool TerrainCompositedAddressAnchored(
    sampler2DArray pageTable, vec2 centerUv, vec2 tapUv, vec2 resolution,
    int requestedMip, int maxMip, float repeatMode, out vec3 atlasUv, out int actualMip)
{
    for (int mip = requestedMip; mip <= maxMip; ++mip)
    {
        ivec2 extent = TerrainCompositedMipExtent(resolution, mip);
        vec2 centerTexel = TerrainCompositedTexel(centerUv, extent, repeatMode);
        ivec2 centerPage = ivec2(floor(centerTexel / 128.0));
        vec2 deltaUv = tapUv - centerUv;
        deltaUv -= round(deltaUv);
        vec2 tapTexel = repeatMode > 0.5
            ? centerTexel + deltaUv * vec2(extent)
            : TerrainCompositedTexel(tapUv, extent, repeatMode);
        vec2 local = tapTexel - vec2(centerPage) * 128.0;
        if (all(greaterThanEqual(local, vec2(-3.5))) &&
            all(lessThanEqual(local, vec2(131.5))))
        {
            uint entry = uint(texelFetch(pageTable, ivec3(centerPage, mip), 0).r + 0.5);
            if (entry != 0u)
            {
                atlasUv = vec3((local + 4.0) / 136.0, float(entry - 1u));
                actualMip = mip;
                return true;
            }
        }
        if (TerrainCompositedAddress(pageTable, tapUv, resolution,
            mip, mip, repeatMode, atlasUv, actualMip))
            return true;
    }
    atlasUv = vec3(0.0);
    actualMip = maxMip;
    return false;
}

vec3 TerrainCompositedDecodeNormal(vec4 encoded, float normalRG)
{
    vec3 normalLocal = encoded.xyz * 2.0 - 1.0;
    if (normalRG > 0.5)
    {
        vec2 normalXZ = encoded.xy * 2.0 - 1.0;
        normalLocal = vec3(normalXZ.x, sqrt(max(0.0, 1.0 - dot(normalXZ, normalXZ))), normalXZ.y);
    }
    float lengthSquared = dot(normalLocal, normalLocal);
    return lengthSquared > 1e-12 ? normalLocal * inversesqrt(lengthSquared) : vec3(0.0, 1.0, 0.0);
}

// localNormalToWorld 必须是 placement 的 inverse-transpose，包含非均匀缩放；
// 烘焙法线已经在地形局部 XYZ，不能再次套 mesh tangent basis。
TerrainCompositedVTResult MF_TerrainCompositedVTAtMip(
    sampler2DArray pageTable, sampler2DArray albedoAtlas,
    sampler2DArray normalAtlas, sampler2DArray ormAtlas,
    vec2 localUv, vec2 resolution, float maxMip, float repeatMode,
    float normalRG, mat3 localNormalToWorld, float targetMip, vec2 centerUv)
{
    TerrainCompositedVTResult result;
    result.targetMip = clamp(targetMip, 0.0, maxMip);
    int lowMip = int(floor(result.targetMip));
    int highMip = min(lowMip + 1, int(maxMip));
    vec3 lowAddress;
    vec3 highAddress;
    int lowActual;
    int highActual;
    bool lowValid = TerrainCompositedAddressAnchored(pageTable, centerUv, localUv, resolution,
        lowMip, int(maxMip), repeatMode, lowAddress, lowActual);
    bool highValid = TerrainCompositedAddressAnchored(pageTable, centerUv, localUv, resolution,
        highMip, int(maxMip), repeatMode, highAddress, highActual);
    result.valid = lowValid && highValid ? 1.0 : 0.0;
    result.actualMips = vec2(lowActual, highActual);
    result.albedo = vec4(0.0);
    result.orm = vec4(1.0, 1.0, 0.0, 1.0);
    result.normalWorld = normalize(localNormalToWorld * vec3(0.0, 1.0, 0.0));
    if (result.valid < 0.5)
        return result;
    // atlas 只有物理 mip0；两个虚拟 mip 独立找祖先后显式插值，禁止硬件生成 atlas mip。
    float blend = fract(result.targetMip);
    result.albedo = mix(textureLod(albedoAtlas, lowAddress, 0.0),
        textureLod(albedoAtlas, highAddress, 0.0), blend);
    result.orm = mix(textureLod(ormAtlas, lowAddress, 0.0),
        textureLod(ormAtlas, highAddress, 0.0), blend);
    vec3 lowNormal = TerrainCompositedDecodeNormal(textureLod(normalAtlas, lowAddress, 0.0), normalRG);
    vec3 highNormal = TerrainCompositedDecodeNormal(textureLod(normalAtlas, highAddress, 0.0), normalRG);
    vec3 blendedNormal = mix(lowNormal, highNormal, blend);
    if (dot(blendedNormal, blendedNormal) < 1e-12)
        blendedNormal = lowNormal;
    result.normalWorld = normalize(localNormalToWorld * normalize(blendedNormal));
    return result;
}

TerrainCompositedVTResult MF_TerrainCompositedVTAtMip(
    sampler2DArray pageTable, sampler2DArray albedoAtlas,
    sampler2DArray normalAtlas, sampler2DArray ormAtlas,
    vec2 localUv, vec2 resolution, float maxMip, float repeatMode,
    float normalRG, mat3 localNormalToWorld, float targetMip)
{
    return MF_TerrainCompositedVTAtMip(
        pageTable, albedoAtlas, normalAtlas, ormAtlas, localUv,
        resolution, maxMip, repeatMode, normalRG, localNormalToWorld, targetMip, localUv);
}

TerrainCompositedVTResult MF_TerrainCompositedVTAtFootprint(
    sampler2DArray pageTable, sampler2DArray albedoAtlas,
    sampler2DArray normalAtlas, sampler2DArray ormAtlas,
    vec2 localUv, vec2 resolution, float maxMip, float repeatMode,
    float normalRG, mat3 localNormalToWorld, TerrainCompositedFootprint footprint)
{
    footprint.tapCount = clamp(footprint.tapCount, 1, TerrainCompositedMaxTaps);
    TerrainCompositedVTResult result;
    result.albedo = vec4(0.0);
    result.orm = vec4(0.0);
    result.normalWorld = vec3(0.0);
    result.actualMips = vec2(0.0);
    result.targetMip = footprint.targetMip;
    result.valid = 1.0;
    for (int tap = 0; tap < footprint.tapCount; ++tap)
    {
        TerrainCompositedVTResult sampled = MF_TerrainCompositedVTAtMip(
            pageTable, albedoAtlas, normalAtlas, ormAtlas,
            TerrainCompositedTapUv(localUv, footprint, tap),
            resolution, maxMip, repeatMode, normalRG, localNormalToWorld,
            footprint.targetMip, localUv);
        result.albedo += sampled.albedo;
        result.orm += sampled.orm;
        result.normalWorld += sampled.normalWorld;
        result.actualMips = max(result.actualMips, sampled.actualMips);
        result.valid = min(result.valid, sampled.valid);
    }
    result.albedo /= float(footprint.tapCount);
    result.orm /= float(footprint.tapCount);
    result.normalWorld = dot(result.normalWorld, result.normalWorld) > 1.0e-12
        ? normalize(result.normalWorld) : normalize(localNormalToWorld * vec3(0.0, 1.0, 0.0));
    return result;
}

TerrainCompositedVTResult MF_TerrainCompositedVT(
    sampler2DArray pageTable, sampler2DArray albedoAtlas,
    sampler2DArray normalAtlas, sampler2DArray ormAtlas,
    vec2 localUv, vec2 resolution, float maxMip, float repeatMode,
    float normalRG, mat3 localNormalToWorld)
{
    TerrainCompositedFootprint footprint = TerrainCompositedFootprintFromGradients(
        dFdx(localUv), dFdy(localUv), resolution, maxMip);
    return MF_TerrainCompositedVTAtFootprint(pageTable, albedoAtlas, normalAtlas, ormAtlas,
        localUv, resolution, maxMip, repeatMode, normalRG, localNormalToWorld, footprint);
}
#endif

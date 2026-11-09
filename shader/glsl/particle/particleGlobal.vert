#version 450

#include "particleCommon.glsl"

struct GatheredBillboard
{
    vec4 centerSize;
    vec4 rightRot;
    vec4 upSoft;
    vec4 color;
    vec4 uvRect;
    vec4 params;
    vec4 frame;
    vec4 flags;
    vec4 lightParams;
    vec4 lightDirection;
    vec4 lightColor;
    vec4 lightParams2;
    vec4 flags2;
    vec4 flags3;
    vec4 uvTransform;
    vec4 corner3Kind;
    vec4 color2;
};

layout(std430, set = 0, binding = 1) readonly buffer GatheredBuffer
{
    GatheredBillboard records[];
} gatheredBuffer;

layout(std430, set = 0, binding = 2) readonly buffer SortedPairs
{
    vec4 pairs[];
} sortedPairs;

// 全局纹理槽位固定 8；未使用的槽绑定默认白图。
layout(set = 0, binding = 3) uniform sampler2D particleTextures[8];

// 不使用未启用的 sampled-image dynamic indexing 特性：纹理槽在实例间
// 可以不同，因此用固定索引分支选择，避免非 uniform 数组索引。
vec2 ParticleTextureSize(int index)
{
    if (index <= 0) return vec2(textureSize(particleTextures[0], 0));
    if (index == 1) return vec2(textureSize(particleTextures[1], 0));
    if (index == 2) return vec2(textureSize(particleTextures[2], 0));
    if (index == 3) return vec2(textureSize(particleTextures[3], 0));
    if (index == 4) return vec2(textureSize(particleTextures[4], 0));
    if (index == 5) return vec2(textureSize(particleTextures[5], 0));
    if (index == 6) return vec2(textureSize(particleTextures[6], 0));
    return vec2(textureSize(particleTextures[7], 0));
}

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vSoftness;
layout(location = 3) out float vSeed;
layout(location = 4) out float vTime;
layout(location = 5) out vec2 vAtlasUV;
layout(location = 6) out float vViewDepth;
layout(location = 7) out flat int vTextureIndex;
layout(location = 8) out vec2 vSpriteUV;
layout(location = 9) out vec2 vSoftParams;
layout(location = 10) out flat float vHasTexture;
layout(location = 11) out vec3 vWorldPosition;
layout(location = 12) out flat vec4 vLightParams;
layout(location = 13) out flat vec3 vLightDirection;
layout(location = 14) out flat vec3 vLightColor;
layout(location = 15) out flat vec3 vLightingNormal;
layout(location = 16) out flat vec4 vLightParams2;
layout(location = 17) out vec2 vAtlasUV2;
layout(location = 18) out float vAtlasBlend;
layout(location = 19) out float vSize;
layout(location = 20) out float vDensity;
layout(location = 21) out float vSoftTapRadius;
layout(location = 22) out float vEooPacked;
layout(location = 23) out float vEooEmissiveStrength;
layout(location = 24) out flat float vKind;
layout(location = 25) out flat float vSoftThickness;

void main()
{
    uint drawIndex = uint(gl_InstanceIndex);
    if (drawIndex >= uint(gatheredBuffer.records.length()))
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vColor = vec4(0.0);
        vUV = vec2(0.0);
        vSoftness = 0.0;
        vSeed = 0.0;
        vTime = 0.0;
        vAtlasUV = vec2(0.0);
        vViewDepth = 0.0;
        vTextureIndex = 0;
        vSpriteUV = vec2(0.0);
        vSoftParams = vec2(0.0);
        vHasTexture = 0.0;
        vWorldPosition = vec3(0.0);
        vLightParams = vec4(0.0);
        vLightDirection = vec3(0.0);
        vLightColor = vec3(0.0);
        vLightingNormal = vec3(0.0);
        vAtlasUV2 = vec2(0.0);
        vAtlasBlend = 0.0;
        vSize = 0.0;
        vDensity = 0.0;
        vSoftTapRadius = 0.0;
        vEooPacked = 0.0;
        vEooEmissiveStrength = 0.0;
        vKind = 0.0;
        vSoftThickness = 0.0;
        return;
    }
    uint recordIndex = uint(sortedPairs.pairs[drawIndex].y);
    if (recordIndex >= uint(gatheredBuffer.records.length()))
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vColor = vec4(0.0);
        vUV = vec2(0.0);
        vSoftness = 0.0;
        vSeed = 0.0;
        vTime = 0.0;
        vAtlasUV = vec2(0.0);
        vViewDepth = 0.0;
        vTextureIndex = 0;
        vSpriteUV = vec2(0.0);
        vSoftParams = vec2(0.0);
        vHasTexture = 0.0;
        vWorldPosition = vec3(0.0);
        vLightParams = vec4(0.0);
        vLightDirection = vec3(0.0);
        vLightColor = vec3(0.0);
        vLightingNormal = vec3(0.0);
        vAtlasUV2 = vec2(0.0);
        vAtlasBlend = 0.0;
        vSize = 0.0;
        vDensity = 0.0;
        vSoftTapRadius = 0.0;
        vEooPacked = 0.0;
        vEooEmissiveStrength = 0.0;
        vKind = 0.0;
        vSoftThickness = 0.0;
        return;
    }

    GatheredBillboard record = gatheredBuffer.records[recordIndex];
    vec2 corner = kParticleCorners[uint(gl_VertexIndex) % 6u];

    if (record.corner3Kind.w > 0.5)
    {
        float u = corner.x * 0.5 + 0.5;
        float v = corner.y * 0.5 + 0.5;
        vec3 bottom = mix(
            record.centerSize.xyz, record.rightRot.xyz, u);
        vec3 top = mix(
            record.upSoft.xyz, record.corner3Kind.xyz, u);
        vec3 worldPosition = mix(bottom, top, v);
        vec2 uv = vec2(
            mix(record.uvRect.x, record.uvRect.z, u),
            mix(record.uvRect.y, record.uvRect.w, v));

        gl_Position =
            frame.viewProjection * vec4(worldPosition, 1.0);
        vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;
        vColor = mix(record.color, record.color2, u);
        vUV = uv;
        vSpriteUV = uv;
        vec2 atlasSize = ParticleTextureSize(int(record.frame.w));
        vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));
        vAtlasUV = clamp(
            vec2(uv.x, 1.0 - uv.y),
            halfTexel,
            vec2(1.0) - halfTexel);
        vAtlasUV2 = vAtlasUV;
        vAtlasBlend = 0.0;
        vTextureIndex = int(record.frame.w);
        vSoftness = record.upSoft.w;
        vSeed = record.params.x;
        vTime = record.params.y;
        vSoftParams = record.flags.zw;
        vHasTexture = record.flags.y;
        vWorldPosition = worldPosition;
        vLightParams = record.lightParams;
        vLightParams2 = record.lightParams2;
        vSize = record.centerSize.w;
        vDensity = record.flags2.y;
        vSoftTapRadius = record.flags2.x;
        vEooPacked = record.flags3.x;
        vEooEmissiveStrength = record.flags3.y;
        vLightDirection = record.lightDirection.xyz;
        vLightColor = record.lightColor.rgb;
        vLightingNormal = normalize(
            frame.cameraPosition.xyz - worldPosition);
        vKind = 1.0;
        vSoftThickness = record.flags2.z;
        return;
    }

    float rotationCos = cos(record.rightRot.w);
    float rotationSin = sin(record.rightRot.w);
    vec2 rotatedCorner = vec2(
        corner.x * rotationCos - corner.y * rotationSin,
        corner.x * rotationSin + corner.y * rotationCos);
    vec2 pivotOffset = record.frame.yz;
    vec3 worldPosition = record.centerSize.xyz +
        (record.rightRot.xyz * (rotatedCorner.x + pivotOffset.x) +
         record.upSoft.xyz * (rotatedCorner.y + pivotOffset.y)) *
        record.centerSize.w;
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;

    vColor = record.color;
    vUV = corner * 0.5 + 0.5;
    vSoftness = record.upSoft.w;
    vSeed = record.params.x;
    vTime = record.params.y;

    // sprite UV：有逐帧 rect 时按记录写入；否则取整张纹理。
    vSpriteUV = vUV;
    if (record.flags.x > 0.5)
    {
        vSpriteUV = mix(
            record.uvRect.xy, record.uvRect.zw, vUV);
    }
    vSpriteUV = vSpriteUV * record.uvTransform.xy +
        record.uvTransform.zw;
    vec2 atlasCell = vec2(
        mod(record.frame.x, record.params.z),
        floor(record.frame.x / record.params.z));
    vAtlasUV = (atlasCell +
        vec2(vSpriteUV.x, 1.0 - vSpriteUV.y)) /
        vec2(record.params.z, record.params.w);
    vec2 atlasSize = ParticleTextureSize(int(record.frame.w));
    vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));
    vec2 cellMin =
        atlasCell / vec2(record.params.z, record.params.w);
    vec2 cellMax =
        (atlasCell + vec2(1.0)) /
        vec2(record.params.z, record.params.w);
    vAtlasUV = clamp(
        vAtlasUV, cellMin + halfTexel, cellMax - halfTexel);
    vec2 atlasCell2 = vec2(
        mod(record.flags2.z, record.params.z),
        floor(record.flags2.z / record.params.z));
    vec2 atlasUV2 = (atlasCell2 +
        vec2(vSpriteUV.x, 1.0 - vSpriteUV.y)) /
        vec2(record.params.z, record.params.w);
    vec2 cellMin2 =
        atlasCell2 / vec2(record.params.z, record.params.w);
    vec2 cellMax2 =
        (atlasCell2 + vec2(1.0)) /
        vec2(record.params.z, record.params.w);
    vAtlasUV2 = clamp(
        atlasUV2, cellMin2 + halfTexel, cellMax2 - halfTexel);
    vTextureIndex = int(record.frame.w);
    vSoftParams = record.flags.zw;
    vHasTexture = record.flags.y;
    vWorldPosition = worldPosition;
    vLightParams = record.lightParams;
    vLightParams2 = record.lightParams2;
    vAtlasBlend = record.flags2.w;
    vSize = record.centerSize.w;
    vDensity = record.flags2.y;
    vSoftTapRadius = record.flags2.x;
    vEooPacked = record.flags3.x;
    vEooEmissiveStrength = record.flags3.y;
    vLightDirection = record.lightDirection.xyz;
    vLightColor = record.lightColor.rgb;
    vec3 lightingNormal = cross(record.rightRot.xyz, record.upSoft.xyz);
    float lightingNormalLength = length(lightingNormal);
    vLightingNormal = lightingNormalLength > 1.0e-5
        ? lightingNormal / lightingNormalLength
        : normalize(frame.cameraPosition.xyz - worldPosition);
    vKind = 0.0;
    vSoftThickness = 0.0;
}

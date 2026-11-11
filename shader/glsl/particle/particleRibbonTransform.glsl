#ifndef PARTICLE_RIBBON_TRANSFORM_GLSL
#define PARTICLE_RIBBON_TRANSFORM_GLSL

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"
#include "particleSprite.glsl"
#include "particleRibbonGeometry.glsl"

// 可见 ribbon 与阴影 caster 共用这一份条带构建：历史点变换、面向相机的
// 切线/侧向量、尾端 alpha 与 atlas UV 只保留一份，避免两侧几何漂移。
struct ParticleRibbonVertex
{
    vec3 worldPosition;
    vec4 color;
    vec2 uv;
    vec2 atlasUV;
    float viewDepth;
    float seed;
    float time;
};

// caster 用 aliveIndices 解析存活粒子；可见 pass 仍可按 sortParams 走
// pair 表。两者粒子集合相同，只是顺序不同，深度写入不依赖顺序。
bool ResolveParticleRibbonParticle(
    uint drawIndex,
    bool useSortedPairs,
    out uint particleIndex,
    out Particle particle)
{
    particleIndex = 0;
    particle.positionAge = vec4(0.0);
    particle.velocityLife = vec4(0.0);
    for (int historyIndex = 0; historyIndex < 16; ++historyIndex)
    {
        particle.history[historyIndex] = vec4(0.0);
    }
    particle.source0 = vec4(0.0);
    particle.source1 = vec4(0.0);
    particle.source2 = vec4(0.0);

    uint count = uint(particleBuffer.particles.length());
    if (drawIndex >= count)
    {
        return false;
    }

    particleIndex = useSortedPairs
        ? uint(sortedPairs.pairs[drawIndex].y)
        : aliveIndices.indices[drawIndex];
    if (particleIndex >= count)
    {
        return false;
    }

    particle = particleBuffer.particles[particleIndex];
    return IsParticleAlive(particle);
}

bool BuildParticleRibbonVertex(
    uint drawIndex,
    uint vertexIndex,
    bool useSortedPairs,
    out ParticleRibbonVertex outVertex)
{
    uint segments = clamp(uint(frame.ribbonParams.x + 0.5), 1u, 64u);
    uint verticesPerParticle = segments * 6u;
    uint localVertex = vertexIndex % verticesPerParticle;

    uint particleIndex;
    Particle particle;
    if (!ResolveParticleRibbonParticle(
            drawIndex,
            useSortedPairs,
            particleIndex,
            particle))
    {
        return false;
    }

    uint segment = localVertex / 6u;
    vec2 corner = kParticleCorners[localVertex % 6u];
    float lifeT = clamp(
        particle.positionAge.w /
            max(particle.velocityLife.w, 1.0e-4),
        0.0,
        1.0);

    vec3 points[17];
    float cumulative[17];
    float totalLength = 0.0;
    if (!BuildParticleRibbonPath(
            particle, points, cumulative, totalLength))
    {
        return false;
    }

    float startT = float(segment) / float(segments);
    float endT = float(segment + 1u) / float(segments);
    float along = corner.x * 0.5 + 0.5;
    float t = mix(startT, endT, along);
    vec3 worldPosition;
    float width = 0.0;
    vec4 color;
    vec2 uv;
    BuildParticleRibbonCorner(
        points,
        cumulative,
        totalLength,
        t,
        corner.y,
        lifeT,
        worldPosition,
        width,
        color,
        uv);
    // ribbon 只采样整张纹理，不消费 flipbook cell；V 仍按图集约定翻转，
    // 并内缩半个 texel 避免线性过滤采到图集外的透明边界。
    vec2 atlasSize = vec2(textureSize(particleTexture, 0));
    vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));

    outVertex.worldPosition = worldPosition;
    outVertex.color = color;
    outVertex.uv = uv;
    outVertex.atlasUV = clamp(
        vec2(uv.x, 1.0 - uv.y),
        halfTexel,
        vec2(1.0) - halfTexel);
    outVertex.viewDepth =
        -(frame.view * vec4(worldPosition, 1.0)).z;
    outVertex.seed = float(particleIndex) * 0.61803398875;
    outVertex.time = frame.simulation.z;
    return true;
}

#endif

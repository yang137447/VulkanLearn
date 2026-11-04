#ifndef PARTICLE_RIBBON_TRANSFORM_GLSL
#define PARTICLE_RIBBON_TRANSFORM_GLSL

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"
#include "particleSprite.glsl"

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
    particle = Particle(
        vec4(0.0),
        vec4(0.0),
        vec4(0.0),
        vec4(0.0),
        vec4(0.0));

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
    uint segments = clamp(uint(frame.ribbonParams.x + 0.5), 1u, 3u);
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

    // current + 3 个历史点；localSpace 发射器的历史同样保存局部坐标，
    // 在这里统一变换到世界空间后再生成条带。
    vec3 points[4];
    points[0] = particle.positionAge.xyz;
    points[1] = particle.history0.xyz;
    points[2] = particle.history1.xyz;
    points[3] = particle.history2.xyz;
    if (frame.shape.z > 0.5)
    {
        for (int index = 0; index < 4; ++index)
        {
            points[index] =
                (frame.localToWorld * vec4(points[index], 1.0)).xyz;
        }
    }

    vec3 start = points[segment];
    vec3 end = points[segment + 1u];
    vec3 tangent = end - start;
    vec3 midpoint = (start + end) * 0.5;
    vec3 viewOffset = frame.cameraPosition.xyz - midpoint;
    float viewLength = length(viewOffset);
    vec3 viewDirection = viewLength > 1.0e-5
        ? viewOffset / viewLength
        : frame.cameraRight.xyz;

    // 条带面向相机：法向同时垂直于切线与视线。切线过短（首帧历史
    // 尚未拉开）时退化为 cameraRight，避免 normalize(0) 产生 NaN。
    vec3 side;
    float tangentLength = length(tangent);
    if (tangentLength > 1.0e-5)
    {
        vec3 crossValue = cross(tangent, viewDirection);
        float crossLength = length(crossValue);
        side = crossLength > 1.0e-5
            ? crossValue / crossLength
            : frame.cameraRight.xyz;
    }
    else
    {
        side = frame.cameraRight.xyz;
    }
    side *= frame.ribbonParams.z;

    float along = corner.x * 0.5 + 0.5;
    vec3 worldPosition = mix(start, end, along) + side * corner.y;

    // 颜色仍按粒子寿命求值；尾端从当前端(0)向最旧历史端(1)线性衰减。
    float tailT = (float(segment) + along) / float(segments);
    vec4 color = EvaluateColorCurve(lifeT);
    color.a *= frame.render.w;
    color.a *= mix(
        1.0,
        1.0 - tailT,
        clamp(frame.ribbonParams.y, 0.0, 1.0));

    vec2 uv = vec2(along, corner.y * 0.5 + 0.5);
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

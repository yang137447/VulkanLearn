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
    particle.positionAge = vec4(0.0);
    particle.velocityLife = vec4(0.0);
    for (int historyIndex = 0; historyIndex < 16; ++historyIndex)
    {
        particle.history[historyIndex] = vec4(0.0);
    }

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

// 固定 16 个历史样本的折线求值：distance 从当前位置(0)到最旧端。
// 输入已按历史顺序排列，cumulative[i] 是 points[0..i] 的累计弧长。
vec3 RibbonPointAt(
    vec3 points[17],
    float cumulative[17],
    float totalLength,
    float normalizedDistance)
{
    if (totalLength <= 1.0e-5)
    {
        return points[0];
    }
    float target = clamp(normalizedDistance, 0.0, 1.0) * totalLength;
    for (int index = 1; index < 17; ++index)
    {
        if (target <= cumulative[index])
        {
            float span = max(cumulative[index] - cumulative[index - 1],
                1.0e-6);
            float weight = clamp(
                (target - cumulative[index - 1]) / span,
                0.0,
                1.0);
            return mix(points[index - 1], points[index], weight);
        }
    }
    return points[16];
}

vec3 RibbonTangentAt(
    vec3 points[17],
    float cumulative[17],
    float totalLength,
    float normalizedDistance)
{
    const float epsilon = 0.01;
    vec3 before = RibbonPointAt(
        points,
        cumulative,
        totalLength,
        max(normalizedDistance - epsilon, 0.0));
    vec3 after = RibbonPointAt(
        points,
        cumulative,
        totalLength,
        min(normalizedDistance + epsilon, 1.0));
    vec3 tangent = after - before;
    float lengthSquared = dot(tangent, tangent);
    return lengthSquared > 1.0e-10
        ? tangent * inversesqrt(lengthSquared)
        : vec3(0.0);
}

bool BuildParticleRibbonVertex(
    uint drawIndex,
    uint vertexIndex,
    bool useSortedPairs,
    out ParticleRibbonVertex outVertex)
{
    uint segments = clamp(uint(frame.ribbonParams.x + 0.5), 1u, 32u);
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

    // current + 16 个历史点；localSpace 发射器的历史同样保存局部坐标，
    // 在这里统一变换到世界空间后再生成条带。
    vec3 points[17];
    points[0] = particle.positionAge.xyz;
    for (int historyIndex = 0; historyIndex < 16; ++historyIndex)
    {
        points[historyIndex + 1] = particle.history[historyIndex].xyz;
    }
    if (frame.shape.z > 0.5)
    {
        for (int index = 0; index < 17; ++index)
        {
            points[index] =
                (frame.localToWorld * vec4(points[index], 1.0)).xyz;
        }
    }

    float cumulative[17];
    cumulative[0] = 0.0;
    for (int index = 1; index < 17; ++index)
    {
        cumulative[index] = cumulative[index - 1] +
            length(points[index] - points[index - 1]);
    }
    float totalLength = cumulative[16];
    if (totalLength <= 1.0e-4)
    {
        return false;
    }

    // 按弧长重采样：输出段在 0..1 上等分，历史帧间距不均匀时也不会
    // 让条带在低帧率处出现折线/宽度跳变。
    float startT = float(segment) / float(segments);
    float endT = float(segment + 1u) / float(segments);
    float along = corner.x * 0.5 + 0.5;
    float t = mix(startT, endT, along);
    vec3 pathPosition = RibbonPointAt(
        points,
        cumulative,
        totalLength,
        t);
    vec3 tangent = RibbonTangentAt(
        points,
        cumulative,
        totalLength,
        t);
    vec3 viewOffset = frame.cameraPosition.xyz - pathPosition;
    float viewLength = length(viewOffset);
    vec3 viewDirection = viewLength > 1.0e-5
        ? viewOffset / viewLength
        : frame.cameraRight.xyz;

    // 条带面向相机：法向同时垂直于切线与视线。切线过短（首帧历史
    // 尚未拉开）时退化为 cameraRight，避免 normalize(0) 产生 NaN。
    vec3 side = cross(tangent, viewDirection);
    float sideLength = length(side);
    side = sideLength > 1.0e-5
        ? side / sideLength
        : frame.cameraRight.xyz;
    float width = frame.ribbonParams.z * EvaluateRibbonWidth(t);
    vec3 worldPosition = pathPosition + side * (corner.y * width);

    // 颜色仍按粒子寿命求值；尾端从当前端(0)向最旧历史端(1)线性衰减。
    vec4 color = EvaluateColorCurve(lifeT);
    color.a *= frame.render.w;
    color.a *= mix(
        1.0,
        1.0 - t,
        clamp(frame.ribbonParams.y, 0.0, 1.0));

    float tiledX = t * max(frame.ribbonUVParams.x, 1.0e-4);
    float spriteX = frame.ribbonUVParams.x > 1.0
        ? fract(tiledX)
        : clamp(tiledX, 0.0, 1.0);
    vec2 uv = vec2(spriteX, corner.y * 0.5 + 0.5);
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

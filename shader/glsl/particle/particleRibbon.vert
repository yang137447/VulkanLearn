#version 450

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleSprite.glsl"

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vSoftness;
layout(location = 3) out float vSeed;
layout(location = 4) out float vTime;
layout(location = 5) out vec2 vAtlasUV;
layout(location = 6) out float vViewDepth;

void CullVertex()
{
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
    vColor = vec4(0.0);
    vUV = vec2(0.0);
    vSoftness = 0.0;
    vSeed = 0.0;
    vTime = 0.0;
    vAtlasUV = vec2(0.0);
    vViewDepth = 0.0;
}

void main()
{
    uint count = uint(particleBuffer.particles.length());
    uint segments = clamp(uint(frame.ribbonParams.x + 0.5), 1u, 3u);
    uint verticesPerParticle = segments * 6u;
    uint drawIndex = uint(gl_VertexIndex) / verticesPerParticle;
    uint localVertex = uint(gl_VertexIndex) % verticesPerParticle;
    if (drawIndex >= count)
    {
        CullVertex();
        return;
    }

    // 与 billboard 相同的排序映射：drawIndex 是排序后的槽位，
    // pair.y 才是原始粒子池索引；seed 继续使用原始索引。
    uint particleIndex = drawIndex;
    if (frame.sortParams.x > 0.5)
    {
        particleIndex = uint(sortedPairs.pairs[drawIndex].y);
    }
    if (particleIndex >= count)
    {
        CullVertex();
        return;
    }

    Particle particle = particleBuffer.particles[particleIndex];
    if (!IsParticleAlive(particle))
    {
        CullVertex();
        return;
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
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;

    // 颜色仍按粒子寿命求值；尾端从当前端(0)向最旧历史端(1)线性衰减。
    float tailT = (float(segment) + along) / float(segments);
    vColor = EvaluateColorCurve(lifeT);
    vColor.a *= frame.render.w;
    vColor.a *= mix(
        1.0,
        1.0 - tailT,
        clamp(frame.ribbonParams.y, 0.0, 1.0));

    vSeed = float(particleIndex) * 0.61803398875;
    vTime = frame.simulation.z;
    vUV = vec2(along, corner.y * 0.5 + 0.5);
    // ribbon 只采样整张纹理，不消费 flipbook cell；V 仍按图集约定翻转，
    // 并内缩半个 texel 避免线性过滤采到图集外的透明边界。这里在 vertex
    // 阶段实际使用 sampler，是为了让 descriptor set layout 的 binding 2
    // stageFlags 与 billboard 完全一致，复用同一批 descriptor set。
    vec2 atlasSize = vec2(textureSize(particleTexture, 0));
    vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));
    vAtlasUV = clamp(
        vec2(vUV.x, 1.0 - vUV.y),
        halfTexel,
        vec2(1.0) - halfTexel);
    vSoftness = frame.render.z;
}

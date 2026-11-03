#version 450

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"
#include "particleSprite.glsl"

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vSoftness;
layout(location = 3) out float vSeed;
layout(location = 4) out float vTime;
layout(location = 5) out vec2 vAtlasUV;
layout(location = 6) out float vViewDepth;
layout(location = 7) out vec3 vWorldPosition;

const float kBeamPi = 3.14159265359;

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
    vWorldPosition = vec3(0.0);
}

// 稳定的整数相位哈希；端点抖动按弧编号与时间量化，避免每帧白噪声闪烁。
float BeamHash(vec3 value)
{
    vec3 p = fract(value * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 BeamJitter(uint arcIndex, float salt, float time, float amount)
{
    if (amount <= 0.0)
    {
        return vec3(0.0);
    }
    float arc = float(arcIndex);
    vec3 seed = vec3(arc * 1.731 + salt, time * 0.37, arc * 0.619);
    vec3 random = vec3(
        BeamHash(seed),
        BeamHash(seed + vec3(17.13, 4.71, 9.37)),
        BeamHash(seed + vec3(41.29, 27.11, 63.53)));
    return (random * 2.0 - 1.0) * amount;
}

// 三层正弦叠加：频率倍数与相位逐层错开，形成不规则的锯齿电弧。
float BeamNoiseWave(
    float t,
    uint arcIndex,
    float frequency,
    float speed,
    float time,
    float channel)
{
    float phase = float(arcIndex) * 1.7 + channel * 2.3;
    return sin(t * frequency * kBeamPi * 2.0 +
            time * speed * 1.7 + phase) *
            0.6 +
        sin(t * frequency * kBeamPi * 4.0 +
            time * speed * 2.9 + phase * 1.9) *
            0.3 +
        sin(t * frequency * kBeamPi * 8.0 +
            time * speed * 4.1 + phase * 3.1) *
            0.1;
}

vec3 BeamPoint(
    vec3 start,
    vec3 end,
    vec3 sideA,
    vec3 sideB,
    uint arcIndex,
    float t,
    float time)
{
    // 每条弧使用略不同的频率，避免多条同频正弦形成规则编织网格。
    float arcFrequency = frame.beamNoise.y *
        (0.85 + 0.30 * BeamHash(
            vec3(float(arcIndex), 1.0, 2.0)));
    float envelope = sin(kBeamPi * t);
    float noiseA = BeamNoiseWave(
        t,
        arcIndex,
        arcFrequency,
        frame.beamNoise.z,
        time,
        0.0);
    float noiseB = BeamNoiseWave(
        t,
        arcIndex,
        arcFrequency,
        frame.beamNoise.z,
        time,
        1.0);
    return mix(start, end, t) +
        (sideA * noiseA + sideB * noiseB) *
            (frame.beamNoise.x * envelope);
}

// 边界处的连续侧向量：相邻 segment 共享同一个 p(t) 与 side(t)，
// 因而四边形在接缝处共享边而不是各自旋转产生亮斑/缺口。
vec3 BeamSide(
    vec3 start,
    vec3 end,
    vec3 sideA,
    vec3 sideB,
    uint arcIndex,
    float t,
    float time,
    float delta)
{
    float previousT = max(t - delta, 0.0);
    float nextT = min(t + delta, 1.0);
    vec3 previous = BeamPoint(
        start, end, sideA, sideB, arcIndex, previousT, time);
    vec3 next = BeamPoint(
        start, end, sideA, sideB, arcIndex, nextT, time);
    vec3 tangent = next - previous;
    vec3 midpoint = BeamPoint(
        start, end, sideA, sideB, arcIndex, t, time);
    vec3 viewOffset = frame.cameraPosition.xyz - midpoint;
    float viewLength = length(viewOffset);
    vec3 viewDirection = viewLength > 1.0e-5
        ? viewOffset / viewLength
        : frame.cameraRight.xyz;
    vec3 side = cross(tangent, viewDirection);
    float sideLength = length(side);
    return sideLength > 1.0e-5
        ? side / sideLength
        : frame.cameraRight.xyz;
}

void main()
{
    uint segments =
        uint(clamp(int(frame.beamStart.w + 0.5), 2, 64));
    // 这里不 clamp count：作者非法值应被资产校验拒绝，而不是在 GPU 上
    // 静默改变实例数。
    uint count = uint(max(int(frame.beamParams.x + 0.5), 0));
    uint verticesPerArc = segments * 6u;
    uint arcIndex = uint(gl_InstanceIndex);
    uint localVertex = uint(gl_VertexIndex) % verticesPerArc;
    uint segment = localVertex / 6u;
    vec2 corner = kParticleCorners[localVertex % 6u];

    // beam 不读取粒子池；显式读取长度与纹理尺寸只为保持与 billboard/ribbon
    // 完全一致的 set 0 descriptor layout（binding 1/2 必须被 SPIR-V 反射）。
    uint particleCapacity = uint(particleBuffer.particles.length());
    vec2 atlasSize = vec2(textureSize(particleTexture, 0));
    if (arcIndex >= count || particleCapacity == 0u)
    {
        CullVertex();
        return;
    }

    float time = frame.simulation.z;
    float jitter = frame.beamNoise.w;
    vec3 start = frame.beamStart.xyz +
        BeamJitter(arcIndex, 1.0, time * frame.beamNoise.z, jitter);
    vec3 end = frame.beamEnd.xyz +
        BeamJitter(arcIndex, 7.0, time * frame.beamNoise.z, jitter);

    vec3 axis = end - start;
    float axisLength = length(axis);
    axis = axisLength > 1.0e-5
        ? axis / axisLength
        : frame.cameraRight.xyz;
    vec3 reference = abs(axis.y) < 0.95
        ? vec3(0.0, 1.0, 0.0)
        : vec3(1.0, 0.0, 0.0);
    vec3 sideA = cross(axis, reference);
    sideA = length(sideA) > 1.0e-5
        ? normalize(sideA)
        : frame.cameraRight.xyz;
    vec3 sideB = normalize(cross(axis, sideA));

    float t0 = float(segment) / float(segments);
    float t1 = float(segment + 1u) / float(segments);
    vec3 point0 = BeamPoint(
        start, end, sideA, sideB, arcIndex, t0, time);
    vec3 point1 = BeamPoint(
        start, end, sideA, sideB, arcIndex, t1, time);
    float delta = 1.0 / float(segments);
    vec3 side0 = BeamSide(
        start, end, sideA, sideB, arcIndex, t0, time, delta);
    vec3 side1 = BeamSide(
        start, end, sideA, sideB, arcIndex, t1, time, delta);

    float along = corner.x * 0.5 + 0.5;
    float t = mix(t0, t1, along);
    // 端点处收窄，避免多条弧在固定端点叠成亮斑。
    float taper0 = smoothstep(0.0, 0.06, t0) *
        smoothstep(0.0, 0.06, 1.0 - t0);
    float taper1 = smoothstep(0.0, 0.06, t1) *
        smoothstep(0.0, 0.06, 1.0 - t1);
    float halfWidth0 = frame.beamEnd.w * mix(0.25, 1.0, taper0);
    float halfWidth1 = frame.beamEnd.w * mix(0.25, 1.0, taper1);
    float halfWidth = mix(halfWidth0, halfWidth1, along);
    vec3 worldPosition =
        mix(point0, point1, along) +
        mix(side0 * halfWidth0, side1 * halfWidth1, along) *
            corner.y;
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;
    vWorldPosition = worldPosition;

    vec4 color = mix(frame.colorStart, frame.colorEnd, t);
    // 每条弧以不同频率闪烁，模拟 Tesla 放电的断续感。
    float flicker = 0.75 + 0.25 *
        (0.5 + 0.5 * sin(
            time * (3.0 + float(arcIndex) * 0.37) +
            float(arcIndex) * 2.1));
    vColor = vec4(color.rgb * flicker, frame.render.w);
    vSeed = float(arcIndex) * 0.61803398875;
    vTime = time;
    vUV = vec2(t, corner.y * 0.5 + 0.5);
    vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));
    vAtlasUV = clamp(
        vec2(vUV.x, 1.0 - vUV.y),
        halfTexel,
        vec2(1.0) - halfTexel);
    vSoftness = frame.render.z;
}

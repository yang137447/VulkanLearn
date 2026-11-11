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

vec3 BeamJitter(
    uint arcIndex,
    float salt,
    float time,
    float amount,
    float noisePhase)
{
    if (amount <= 0.0)
    {
        return vec3(0.0);
    }
    float arc = float(arcIndex);
    vec3 seed = vec3(
        arc * 1.731 + salt + noisePhase * 0.137,
        time * 0.37,
        arc * 0.619);
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
    float channel,
    float noisePhase)
{
    float phase =
        float(arcIndex) * 1.7 + channel * 2.3 + noisePhase;
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
    float time,
    float noiseScale,
    float noisePhase)
{
    // 每条弧使用略不同的频率，避免多条同频正弦形成规则编织网格。
    float arcFrequency = frame.beamNoise.y * noiseScale *
        (0.85 + 0.30 * BeamHash(
            vec3(float(arcIndex), 1.0, 2.0)));
    float envelope = sin(kBeamPi * t);
    float noiseA = BeamNoiseWave(
        t,
        arcIndex,
        arcFrequency,
        frame.beamNoise.z,
        time,
        0.0,
        noisePhase);
    float noiseB = BeamNoiseWave(
        t,
        arcIndex,
        arcFrequency,
        frame.beamNoise.z,
        time,
        1.0,
        noisePhase);
    return mix(start, end, t) +
        (sideA * noiseA + sideB * noiseB) *
            (frame.beamNoise.x * noiseScale * envelope);
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
    float delta,
    float noiseScale,
    float noisePhase)
{
    float previousT = max(t - delta, 0.0);
    float nextT = min(t + delta, 1.0);
    vec3 previous = BeamPoint(
        start, end, sideA, sideB, arcIndex, previousT, time, noiseScale,
        noisePhase);
    vec3 next = BeamPoint(
        start, end, sideA, sideB, arcIndex, nextT, time, noiseScale,
        noisePhase);
    vec3 tangent = next - previous;
    vec3 midpoint = BeamPoint(
        start, end, sideA, sideB, arcIndex, t, time, noiseScale,
        noisePhase);
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

// 主弧两端都收窄；分支只在末端收窄，根部保持全宽以贴住主弧。
float BeamTaper(float t, bool branchRoot)
{
    float endTaper = smoothstep(0.0, 0.06, 1.0 - t);
    return branchRoot
        ? endTaper
        : smoothstep(0.0, 0.06, t) * endTaper;
}

float BeamColorKeyTime(int index)
{
    return index < 4
        ? frame.beamColorKeyTimes[0][index]
        : frame.beamColorKeyTimes[1][index - 4];
}

// 颜色曲线按弧归一化 t 采样，起点/终点外 clamp 到首尾 key。
vec4 EvaluateBeamColorCurve(float t)
{
    int count = int(frame.beamCurveParams.x);
    if (count <= 1)
    {
        return frame.beamColorKeys[0];
    }
    if (t <= BeamColorKeyTime(0))
    {
        return frame.beamColorKeys[0];
    }
    if (t >= BeamColorKeyTime(count - 1))
    {
        return frame.beamColorKeys[count - 1];
    }
    for (int index = 0; index < count - 1; ++index)
    {
        float t0 = BeamColorKeyTime(index);
        float t1 = BeamColorKeyTime(index + 1);
        if (t >= t0 && t <= t1)
        {
            float span = max(t1 - t0, 1.0e-5);
            float weight = (t - t0) / span;
            if (ParticleCurveMode() != 0)
            {
                return ParticleHermiteVector4(
                    frame.beamColorKeys[index],
                    frame.beamColorKeys[index + 1],
                    frame.beamColorTangentOut[index],
                    frame.beamColorTangentIn[index + 1],
                    weight,
                    span);
            }
            return mix(
                frame.beamColorKeys[index],
                frame.beamColorKeys[index + 1],
                weight);
        }
    }
    return frame.beamColorKeys[count - 1];
}

// 宽度 key 是 [0,1] 乘数；无表时恒为 1，保持旧资产画面。
float BeamWidthScaleAt(float t)
{
    int count = int(frame.beamCurveParams.y);
    if (count <= 0)
    {
        return 1.0;
    }
    if (count == 1 || t <= frame.beamWidthKeys[0].x)
    {
        return frame.beamWidthKeys[0].y;
    }
    if (t >= frame.beamWidthKeys[count - 1].x)
    {
        return frame.beamWidthKeys[count - 1].y;
    }
    for (int index = 0; index < count - 1; ++index)
    {
        float t0 = frame.beamWidthKeys[index].x;
        float t1 = frame.beamWidthKeys[index + 1].x;
        if (t >= t0 && t <= t1)
        {
            float span = max(t1 - t0, 1.0e-5);
            float weight = (t - t0) / span;
            if (ParticleCurveMode() != 0)
            {
                return ParticleHermiteScalar(
                    frame.beamWidthKeys[index].y,
                    frame.beamWidthKeys[index + 1].y,
                    frame.beamWidthKeys[index].w,
                    frame.beamWidthKeys[index + 1].z,
                    weight,
                    span);
            }
            return mix(
                frame.beamWidthKeys[index].y,
                frame.beamWidthKeys[index + 1].y,
                weight);
        }
    }
    return frame.beamWidthKeys[count - 1].y;
}

// 逐弧颜色是基础色曲线的乘性 tint；省略 arcs 时两边都是 1。
vec4 BeamArcColor(float t, uint arcIndex)
{
    vec4 base = int(frame.beamCurveParams.x) > 0
        ? EvaluateBeamColorCurve(t)
        : mix(frame.colorStart, frame.colorEnd, t);
    vec3 tint = mix(
        frame.beamArcColorStart[arcIndex].rgb,
        frame.beamArcColorEnd[arcIndex].rgb,
        t);
    // beam 的 alpha 继续由 opacity/branch fade 控制；颜色 key 的 alpha
    // 保留给未来扩展，避免旧资产的 colorStart.w 突然改变亮度。
    return vec4(base.rgb * tint, 1.0);
}

void main()
{
    uint segments =
        uint(clamp(int(frame.beamStart.w + 0.5), 2, 64));
    // 这里不 clamp count：作者非法值应被资产校验拒绝，而不是在 GPU 上
    // 静默改变实例数。
    uint count = uint(max(int(frame.beamParams.x + 0.5), 0));
    uint branchCount =
        uint(clamp(int(frame.beamParams.y + 0.5), 0, 4));
    // 实例布局：[arc0 main, arc0 branch1..N, arc1 main, ...]。
    // 分支复用所属弧的端点/相位，但使用独立的噪声种子。
    uint arcsPerInstance = 1u + branchCount;
    uint arcIndex = uint(gl_InstanceIndex) / arcsPerInstance;
    uint branchIndex = uint(gl_InstanceIndex) % arcsPerInstance;
    bool isBranch = branchIndex > 0u;
    uint verticesPerArc = segments * 6u;
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

    vec4 arcParams = frame.beamArcParams[arcIndex];
    float arcNoisePhase = arcParams.z;
    float time = frame.simulation.z;
    float jitter = frame.beamNoise.w;
    vec3 start = frame.beamStart.xyz +
        BeamJitter(
            arcIndex,
            1.0,
            time * frame.beamNoise.z,
            jitter,
            arcNoisePhase);
    vec3 end = frame.beamEnd.xyz +
        BeamJitter(
            arcIndex,
            7.0,
            time * frame.beamNoise.z,
            jitter,
            arcNoisePhase);

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

    // 分支：从主弧内部取稳定 root t，沿主切线朝侧向偏转，
    // 末端再叠加 branchSpread。strip 的 t=0 与主弧 root 完全重合。
    vec3 stripStart = start;
    vec3 stripEnd = end;
    uint stripSeed = arcIndex;
    float branchWeight = 1.0;
    float stripNoiseScale = 1.0;
    if (isBranch)
    {
        float rootHash = BeamHash(vec3(
            float(arcIndex) * 7.13 + float(branchIndex) * 3.71,
            2.0,
            9.0));
        float rootT = clamp(
            frame.beamParams.z + 0.2 * rootHash, 0.0, 1.0);
        vec3 root = BeamPoint(
            start,
            end,
            sideA,
            sideB,
            arcIndex,
            rootT,
            time,
            1.0,
            arcNoisePhase);
        float delta = 1.0 / float(segments);
        vec3 previous = BeamPoint(
            start,
            end,
            sideA,
            sideB,
            arcIndex,
            max(rootT - delta, 0.0),
            time,
            1.0,
            arcNoisePhase);
        vec3 next = BeamPoint(
            start,
            end,
            sideA,
            sideB,
            arcIndex,
            min(rootT + delta, 1.0),
            time,
            1.0,
            arcNoisePhase);
        vec3 tangent = next - previous;
        float tangentLength = length(tangent);
        tangent = tangentLength > 1.0e-5
            ? tangent / tangentLength
            : axis;
        float lateralHash = BeamHash(vec3(
            float(arcIndex) * 5.31 + float(branchIndex) * 13.7,
            4.0,
            2.0));
        float lateralAngle = lateralHash * kBeamPi * 2.0;
        vec3 lateral = normalize(
            sideA * cos(lateralAngle) +
            sideB * sin(lateralAngle));
        float bendHash = BeamHash(vec3(
            float(arcIndex) * 9.17 + float(branchIndex) * 7.3,
            6.0,
            8.0));
        float bendAngle = mix(0.55, 1.15, bendHash);
        vec3 branchDirection = normalize(
            tangent * cos(bendAngle) +
            lateral * sin(bendAngle));
        float branchLength = clamp(frame.beamParams.w, 0.0001, 1.0);
        stripStart = root;
        stripEnd = root +
            branchDirection * (branchLength * axisLength) +
            lateral * max(frame.beamBranch.x, 0.0);
        stripSeed = 4000u + arcIndex * 8u + branchIndex;
        stripNoiseScale = clamp(branchLength, 0.1, 1.0);
        branchWeight = 0.55;
    }

    float t0 = float(segment) / float(segments);
    float t1 = float(segment + 1u) / float(segments);
    vec3 point0 = BeamPoint(
        stripStart, stripEnd, sideA, sideB, stripSeed, t0, time,
        stripNoiseScale, arcNoisePhase);
    vec3 point1 = BeamPoint(
        stripStart, stripEnd, sideA, sideB, stripSeed, t1, time,
        stripNoiseScale, arcNoisePhase);
    float delta = 1.0 / float(segments);
    vec3 side0 = BeamSide(
        stripStart, stripEnd, sideA, sideB, stripSeed, t0, time, delta,
        stripNoiseScale, arcNoisePhase);
    vec3 side1 = BeamSide(
        stripStart, stripEnd, sideA, sideB, stripSeed, t1, time, delta,
        stripNoiseScale, arcNoisePhase);

    float along = corner.x * 0.5 + 0.5;
    float t = mix(t0, t1, along);
    // 端点处收窄，避免多条弧在固定端点叠成亮斑。
    float taper0 = BeamTaper(t0, isBranch);
    float taper1 = BeamTaper(t1, isBranch);
    float arcWidthScale = clamp(arcParams.x, 0.0, 1.0);
    float halfWidth0 = frame.beamEnd.w * frame.lodParams.x * branchWeight *
        arcWidthScale * BeamWidthScaleAt(t0) * mix(0.25, 1.0, taper0);
    float halfWidth1 = frame.beamEnd.w * frame.lodParams.x * branchWeight *
        arcWidthScale * BeamWidthScaleAt(t1) * mix(0.25, 1.0, taper1);
    float halfWidth = mix(halfWidth0, halfWidth1, along);
    vec3 worldPosition =
        mix(point0, point1, along) +
        mix(side0 * halfWidth0, side1 * halfWidth1, along) *
            corner.y;
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;
    vWorldPosition = worldPosition;

    vec4 color = BeamArcColor(t, arcIndex);
    // 每条弧以不同频率闪烁，模拟 Tesla 放电的断续感。
    float flicker = 0.75 + 0.25 *
        (0.5 + 0.5 * sin(
            time * (3.0 + float(arcIndex) * 0.37) +
            float(arcIndex) * 2.1 +
            float(branchIndex) * 0.83));
    // 分支比主弧暗、细，并沿长度淡出，形成 UE 电弧的短分叉。
    float branchColor = isBranch ? 0.55 : 1.0;
    float branchFade = isBranch ? mix(1.0, 0.35, t) : 1.0;
    float arcIntensity = max(arcParams.y, 0.0);
    vColor = vec4(
        color.rgb * flicker * branchColor * arcIntensity,
        frame.render.w * branchFade);
    vSeed = float(arcIndex) * 0.61803398875 +
        float(branchIndex) * 0.137;
    vTime = time;
    vUV = vec2(t, corner.y * 0.5 + 0.5);
    vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));
    vAtlasUV = clamp(
        vec2(vUV.x, 1.0 - vUV.y),
        halfTexel,
        vec2(1.0) - halfTexel);
    vSoftness = frame.render.z;
}

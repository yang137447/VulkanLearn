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

void main()
{
    // 顶点选择逻辑与 particle.vert 保持一致：排序开启时实例索引是排序槽，
    // sortedPairs.y 才是原始粒子池索引；否则使用 finalize 写出的存活索引。
    uint count = uint(particleBuffer.particles.length());
    uint drawIndex = uint(gl_InstanceIndex);
    if (drawIndex >= count)
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vColor = vec4(0.0);
        vUV = vec2(0.0);
        vSoftness = 0.0;
        vSeed = 0.0;
        vTime = 0.0;
        vAtlasUV = vec2(0.0);
        vViewDepth = 0.0;
        return;
    }

    uint particleIndex = drawIndex;
    if (frame.sortParams.x > 0.5)
    {
        particleIndex = uint(sortedPairs.pairs[drawIndex].y);
    }
    else
    {
        particleIndex = aliveIndices.indices[drawIndex];
    }
    if (particleIndex >= count)
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vColor = vec4(0.0);
        vUV = vec2(0.0);
        vSoftness = 0.0;
        vSeed = 0.0;
        vTime = 0.0;
        vAtlasUV = vec2(0.0);
        vViewDepth = 0.0;
        return;
    }

    Particle particle = particleBuffer.particles[particleIndex];
    if (!IsParticleAlive(particle))
    {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vColor = vec4(0.0);
        vUV = vec2(0.0);
        vSoftness = 0.0;
        vSeed = 0.0;
        vTime = 0.0;
        vAtlasUV = vec2(0.0);
        vViewDepth = 0.0;
        return;
    }

    uint cornerIndex = uint(gl_VertexIndex) % 6u;
    vec2 corner = kParticleCorners[cornerIndex];
    float lifeT = clamp(
        particle.positionAge.w /
            max(particle.velocityLife.w, 1.0e-4),
        0.0,
        1.0);
    float size = EvaluateSizeCurve(lifeT) * frame.lodParams.x;

    // localSpace 发射器只在渲染边界变换到世界空间；distortion 与 billboard
    // 共用同一套中心、尺寸与视深度语义。
    vec3 particleCenter = particle.positionAge.xyz;
    if (frame.shape.z > 0.5)
    {
        particleCenter =
            (frame.localToWorld * vec4(particleCenter, 1.0)).xyz;
    }

    float centerDepth =
        -(frame.view * vec4(particleCenter, 1.0)).z;
    float projScaleY =
        1.0 / max(abs(frame.invProjection[1][1]), 1.0e-6);
    float pixelHalfSize;
    if (abs(frame.invProjection[3][3]) < 1.0e-6)
    {
        pixelHalfSize = size * projScaleY *
            frame.screenParams.x * 0.5 /
            max(centerDepth, 1.0e-3);
    }
    else
    {
        pixelHalfSize = size * projScaleY *
            frame.screenParams.x * 0.5;
    }
    if (frame.screenParams.y > 0.0 &&
        pixelHalfSize > frame.screenParams.y)
    {
        size *= frame.screenParams.y / pixelHalfSize;
    }

    vSeed = float(particleIndex) * 0.61803398875;
    vTime = frame.simulation.z;

    float columns = max(frame.curveParams.z, 1.0);
    float rows = max(frame.curveParams.w, 1.0);
    float startFrame = frame.flipbookParams.z;
    float endFrame = frame.flipbookParams.w;
    float frameCount = endFrame - startFrame + 1.0;

    float localFrameIndex = 0.0;
    float timelineT = 0.0;
    if (frame.flipbookParams3.x > 0.5)
    {
        int durationCount = clamp(
            int(frame.flipbookParams3.z + 0.5), 1, 256);
        float totalWeight = 0.0;
        for (int index = 0; index < durationCount; ++index)
        {
            totalWeight += FlipbookDuration(index);
        }
        float phaseWeight = 0.0;
        if (frame.flipbookParams.x > 0.0)
        {
            float totalSeconds =
                frameCount / frame.flipbookParams.x;
            float cycle = vTime / totalSeconds;
            if (frame.flipbookParams2.y > 0.5)
            {
                cycle += fract(vSeed);
            }
            cycle = frame.flipbookParams2.x > 0.5
                ? mod(cycle, 1.0)
                : min(cycle, 1.0);
            phaseWeight = cycle * totalWeight;
            timelineT = cycle;
        }
        else
        {
            phaseWeight = lifeT * totalWeight;
            timelineT = clamp(lifeT, 0.0, 1.0);
        }
        int selected = durationCount - 1;
        float accumulated = 0.0;
        for (int index = 0; index < durationCount; ++index)
        {
            float duration = FlipbookDuration(index);
            if (phaseWeight < accumulated + duration)
            {
                selected = index;
                break;
            }
            accumulated += duration;
        }
        localFrameIndex = float(selected);
    }
    else if (frame.flipbookParams.x > 0.0)
    {
        float phase = vTime * frame.flipbookParams.x;
        if (frame.flipbookParams2.y > 0.5)
        {
            phase += fract(vSeed);
        }
        float stepped = floor(phase);
        timelineT = clamp(phase / frameCount, 0.0, 1.0);
        localFrameIndex = frame.flipbookParams2.x > 0.5
            ? mod(stepped, frameCount)
            : min(stepped, frameCount - 1.0);
    }
    else
    {
        localFrameIndex = min(floor(lifeT * frameCount), frameCount - 1.0);
        timelineT = clamp(lifeT, 0.0, 1.0);
    }
    float frameIndex = clamp(
        localFrameIndex + startFrame,
        startFrame,
        endFrame);

    float frameRotation = 0.0;
    if (frame.flipbookParams4.x > 0.5)
    {
        int rotationIndex = clamp(
            int(floor(localFrameIndex)), 0, int(frameCount) - 1);
        frameRotation = FlipbookRotation(rotationIndex);
    }
    if (frame.flipbookTimelineParams.y > 0.5)
    {
        frameRotation += EvaluateFlipbookRotationTimeline(timelineT);
    }

    float rotationDegrees =
        frame.rotationParams.x +
        frame.rotationParams.y * particle.positionAge.w +
        frameRotation;
    if (frame.rotationParams.z > 0.5)
    {
        rotationDegrees += fract(vSeed) * 360.0;
    }
    float rotationRadians = radians(rotationDegrees);
    float rotationCos = cos(rotationRadians);
    float rotationSin = sin(rotationRadians);
    vec2 rotatedCorner = vec2(
        corner.x * rotationCos - corner.y * rotationSin,
        corner.x * rotationSin + corner.y * rotationCos);

    vec2 pivot = vec2(0.5);
    if (frame.flipbookParams3.y > 0.5)
    {
        int pivotIndex = clamp(
            int(floor(localFrameIndex)), 0, int(frameCount) - 1);
        pivot = FlipbookPivot(pivotIndex);
    }
    if (frame.flipbookTimelineParams.x > 0.5)
    {
        pivot += EvaluateFlipbookPivotTimeline(timelineT) - vec2(0.5);
    }
    vec2 pivotOffset = (vec2(0.5) - pivot) * 2.0;

    vec3 billboardRight = frame.cameraRight.xyz;
    vec3 billboardUp = frame.cameraUp.xyz;
    int billboardAlignment = int(frame.billboardParams.x + 0.5);
    if (billboardAlignment == 1)
    {
        vec3 velocity = particle.velocityLife.xyz;
        if (frame.shape.z > 0.5)
        {
            velocity = mat3(frame.localToWorld) * velocity;
        }
        float velocityLengthSquared = dot(velocity, velocity);
        if (velocityLengthSquared > 1.0e-6)
        {
            vec3 forward = normalize(velocity);
            vec3 candidateRight =
                cross(vec3(0.0, 1.0, 0.0), forward);
            float rightLengthSquared =
                dot(candidateRight, candidateRight);
            if (rightLengthSquared > 1.0e-6)
            {
                candidateRight = normalize(candidateRight);
                vec3 candidateUp = cross(forward, candidateRight);
                float upLengthSquared = dot(candidateUp, candidateUp);
                if (upLengthSquared > 1.0e-6)
                {
                    billboardRight = candidateRight;
                    billboardUp = normalize(candidateUp);
                }
            }
        }
    }
    else if (billboardAlignment == 2)
    {
        vec3 normal = frame.billboardFixedNormal.xyz;
        vec3 upHint = frame.billboardFixedUp.xyz;
        float normalLengthSquared = dot(normal, normal);
        float upLengthSquared = dot(upHint, upHint);
        if (normalLengthSquared > 1.0e-6 &&
            upLengthSquared > 1.0e-6)
        {
            vec3 normalDir = normal * inversesqrt(normalLengthSquared);
            vec3 upDir = upHint * inversesqrt(upLengthSquared);
            vec3 candidateRight = cross(upDir, normalDir);
            float rightLengthSquared =
                dot(candidateRight, candidateRight);
            if (rightLengthSquared > 1.0e-6)
            {
                billboardRight =
                    candidateRight * inversesqrt(rightLengthSquared);
                billboardUp = normalize(cross(normalDir, billboardRight));
            }
        }
    }

    vec3 worldPosition = particleCenter +
        (billboardRight * (rotatedCorner.x + pivotOffset.x) +
         billboardUp * (rotatedCorner.y + pivotOffset.y)) * size;
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;

    vColor = EvaluateColorCurve(lifeT);
    vColor.a *= frame.render.w;
    vec2 localUV = corner * 0.5 + 0.5;
    vUV = localUV;
    vec2 spriteUV = localUV;
    if (frame.flipbookParams3.w > 0.5)
    {
        int uvIndex = clamp(
            int(floor(localFrameIndex)), 0, int(frameCount) - 1);
        vec4 rect = FlipbookUvRect(uvIndex);
        spriteUV = mix(rect.xy, rect.zw, localUV);
    }
    vSoftness = frame.render.z;
    vec2 atlasCell = vec2(
        mod(frameIndex, columns),
        floor(frameIndex / columns));
    vAtlasUV = (atlasCell +
        vec2(spriteUV.x, 1.0 - spriteUV.y)) / vec2(columns, rows);

    vec2 atlasSize = vec2(textureSize(particleTexture, 0));
    vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));
    vec2 cellMin = atlasCell / vec2(columns, rows);
    vec2 cellMax = (atlasCell + vec2(1.0)) / vec2(columns, rows);
    vAtlasUV = clamp(
        vAtlasUV,
        cellMin + halfTexel,
        cellMax - halfTexel);
}

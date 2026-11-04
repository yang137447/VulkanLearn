#ifndef PARTICLE_BILLBOARD_TRANSFORM_GLSL
#define PARTICLE_BILLBOARD_TRANSFORM_GLSL

#include "particleCommon.glsl"
#include "particleStorageRO.glsl"
#include "particleSorted.glsl"
#include "particleAlive.glsl"
#include "particleSprite.glsl"

// 可见 billboard 与阴影 caster 共用这一份几何构建：实例解析、屏幕尺寸
// 封顶、flipbook 帧/轴心/旋转、billboard 朝向基与 atlas UV 都只保留一份。
// 两套 pass 从同一函数取世界位置，避免阴影与可见卡片随作者参数或相机变化
// 而错位。
struct ParticleBillboardVertex
{
    vec3 worldPosition;
    vec4 color;
    vec2 localUV;
    vec2 atlasUV;
    float viewDepth;
    float seed;
    float time;
    vec3 lightingNormal;
};

// useSortedPairs 只在可见 alpha 绘制时为 true。阴影 caster 不关心绘制
// 顺序，固定走 aliveIndices；这样全局排序池发射器（跳过自身 bitonic 排序、
// 自己的 pair 表未初始化）也能安全生成同一批几何。
bool ResolveParticleBillboardParticle(
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

// 返回 false 表示该实例/顶点不该写深度；调用方负责把 gl_Position 推到
// 必然裁剪的位置。vertexIndex 对应 gl_VertexIndex，一个粒子固定 6 个
// 顶点（两个三角形）。
bool BuildParticleBillboardVertex(
    uint drawIndex,
    uint vertexIndex,
    bool useSortedPairs,
    out ParticleBillboardVertex outVertex)
{
    uint particleIndex;
    Particle particle;
    if (!ResolveParticleBillboardParticle(
            drawIndex,
            useSortedPairs,
            particleIndex,
            particle))
    {
        return false;
    }

    uint cornerIndex = vertexIndex % 6u;
    vec2 corner = kParticleCorners[cornerIndex];
    float lifeT = clamp(
        particle.positionAge.w /
            max(particle.velocityLife.w, 1.0e-4),
        0.0,
        1.0);
    float size = EvaluateSizeCurve(lifeT);

    // localSpace 发射器的模拟坐标是局部的，只在这里变换到世界；
    // 屏幕尺寸、深度与 billboard 都以变换后的世界中心为准。
    vec3 particleCenter = particle.positionAge.xyz;
    if (frame.shape.z > 0.5)
    {
        particleCenter =
            (frame.localToWorld * vec4(particleCenter, 1.0)).xyz;
    }

    // 逐 billboard 屏幕尺寸封顶只缩小 quad 的世界半边长，粒子中心、
    // 寿命与曲线数据保持不变。projectionScaleY 从 inverse projection 取回，
    // 对透视与正交投影都成立；正交分支没有深度除法。
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

    // 每个粒子固定相位，避免整池噪声同步闪烁。
    float seed = float(particleIndex) * 0.61803398875;
    float time = frame.simulation.z;

    float columns = max(frame.curveParams.z, 1.0);
    float rows = max(frame.curveParams.w, 1.0);
    float startFrame = frame.flipbookParams.z;
    float endFrame = frame.flipbookParams.w;
    float frameCount = endFrame - startFrame + 1.0;

    // 局部帧号（0..frameCount-1）。有逐帧时长表时按累计时长选帧，
    // 否则沿用 fps / 寿命驱动的等长帧选择。
    float localFrameIndex = 0.0;
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
            // fps>0：一轮固定 frameCount/fps 秒；duration 只是相对权重。
            float totalSeconds =
                frameCount / frame.flipbookParams.x;
            float cycle = time / totalSeconds;
            if (frame.flipbookParams2.y > 0.5)
            {
                cycle += fract(seed);
            }
            cycle = frame.flipbookParams2.x > 0.5
                ? mod(cycle, 1.0)
                : min(cycle, 1.0);
            phaseWeight = cycle * totalWeight;
        }
        else
        {
            // fps=0：整轮按寿命播放，权重只决定各帧占比。
            phaseWeight = lifeT * totalWeight;
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
        // randomStartFrame 只改变时间相位；loop 关闭时推进到最后一帧后停住。
        // 帧号必须离散：连续 phase 直接取 mod 会让 atlas cell 变成小数，
        // 在相邻图集单元之间跨帧采样，并与 pivot 的整数帧不一致。
        float phase = time * frame.flipbookParams.x;
        if (frame.flipbookParams2.y > 0.5)
        {
            phase += fract(seed);
        }
        float stepped = floor(phase);
        localFrameIndex = frame.flipbookParams2.x > 0.5
            ? mod(stepped, frameCount)
            : min(stepped, frameCount - 1.0);
    }
    else
    {
        // fps=0 时用寿命归一化时间走完闭区间，天然只播放一次。
        localFrameIndex = min(floor(lifeT * frameCount), frameCount - 1.0);
    }
    float frameIndex = clamp(
        localFrameIndex + startFrame,
        startFrame,
        endFrame);

    // 逐帧旋转叠加到逐粒子旋转上；角度按当前整数局部帧号取表值。
    // 表与 UV rect 一样只覆盖 0..frameCount-1，越界由 clamp 兜底。
    float frameRotation = 0.0;
    if (frame.flipbookParams4.x > 0.5)
    {
        int rotationIndex = clamp(
            int(floor(localFrameIndex)), 0, int(frameCount) - 1);
        frameRotation = FlipbookRotation(rotationIndex);
    }

    // Billboard 在相机平面内绕自身中心旋转；UV 仍由未旋转的 corner 生成。
    float rotationDegrees =
        frame.rotationParams.x +
        frame.rotationParams.y * particle.positionAge.w +
        frameRotation;
    if (frame.rotationParams.z > 0.5)
    {
        rotationDegrees += fract(seed) * 360.0;
    }
    float rotationRadians = radians(rotationDegrees);
    float rotationCos = cos(rotationRadians);
    float rotationSin = sin(rotationRadians);
    vec2 rotatedCorner = vec2(
        corner.x * rotationCos - corner.y * rotationSin,
        corner.x * rotationSin + corner.y * rotationCos);

    // 轴心：精灵图上的锚点落在粒子中心，等价于把 quad 中心反向平移。
    // 索引使用区间内的局部帧号，而不是 atlas 绝对帧号。
    vec2 pivotOffset = vec2(0.0);
    if (frame.flipbookParams3.y > 0.5)
    {
        int pivotIndex = clamp(
            int(floor(localFrameIndex)), 0, int(frameCount) - 1);
        pivotOffset = (vec2(0.5) - FlipbookPivot(pivotIndex)) * 2.0;
    }

    // Billboard 朝向基：默认跟随相机平面；velocity 模式让 quad 的法线
    // 对齐粒子速度，适合拖尾/火花；fixed 模式使用作者化的世界空间
    // 法线与上方向，适合枪口闪光等不随相机转动的卡片。任何模式退化时
    // 都回退到相机基，避免零面积 quad 与 NaN。
    vec3 billboardRight = frame.cameraRight.xyz;
    vec3 billboardUp = frame.cameraUp.xyz;
    int billboardAlignment = int(frame.billboardParams.x + 0.5);
    if (billboardAlignment == 1)
    {
        // localSpace 粒子池保存局部速度；构造世界空间 billboard 基前
        // 必须乘发射器线性部分，否则旋转后的发射器会让朝向偏转。
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
        // fixed 基：normal 是 quad 面法线，up 只作为上方向提示；
        // 先构造 right 再正交化 up，使 cross(right, up) 精确等于 normal。
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

    // size 是世界空间半边长（cm）。
    vec3 worldPosition = particleCenter +
        (billboardRight * (rotatedCorner.x + pivotOffset.x) +
         billboardUp * (rotatedCorner.y + pivotOffset.y)) * size;

    vec3 lightingNormal = cross(billboardRight, billboardUp);
    float lightingNormalLength = length(lightingNormal);
    outVertex.worldPosition = worldPosition;
    outVertex.color = EvaluateColorCurve(lifeT);
    outVertex.color.a *= frame.render.w;
    outVertex.localUV = corner * 0.5 + 0.5;
    // quad 局部 UV 继续供程序化圆盘使用；图集采样走逐帧 UV rect。
    vec2 spriteUV = outVertex.localUV;
    if (frame.flipbookParams3.w > 0.5)
    {
        // rect 是 sprite 空间 [0,1] 的子矩形，索引同样是区间内局部帧号。
        int uvIndex = clamp(
            int(floor(localFrameIndex)), 0, int(frameCount) - 1);
        vec4 rect = FlipbookUvRect(uvIndex);
        spriteUV = mix(rect.xy, rect.zw, outVertex.localUV);
    }
    vec2 atlasCell = vec2(
        mod(frameIndex, columns),
        floor(frameIndex / columns));
    // 图集按 top-down 行存储，而 billboard 的 vUV 以左下为 0；
    // 在 cell 内翻转 V，避免火焰/烟雾纹理上下颠倒。
    vec2 atlasUV = (atlasCell +
        vec2(spriteUV.x, 1.0 - spriteUV.y)) / vec2(columns, rows);

    // 图集禁止 mipmap 后仍会被相邻 cell 的线性过滤采到。把 UV 内缩半个
    // texel 到当前 cell 边界内，保留上面的 V 翻转与 cell 布局。
    vec2 atlasSize = vec2(textureSize(particleTexture, 0));
    vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));
    vec2 cellMin = atlasCell / vec2(columns, rows);
    vec2 cellMax = (atlasCell + vec2(1.0)) / vec2(columns, rows);
    outVertex.atlasUV = clamp(
        atlasUV,
        cellMin + halfTexel,
        cellMax - halfTexel);
    outVertex.viewDepth =
        -(frame.view * vec4(worldPosition, 1.0)).z;
    outVertex.seed = seed;
    outVertex.time = time;
    outVertex.lightingNormal = lightingNormalLength > 1.0e-5
        ? lightingNormal / lightingNormalLength
        : normalize(frame.cameraPosition.xyz - worldPosition);
    return true;
}

#endif

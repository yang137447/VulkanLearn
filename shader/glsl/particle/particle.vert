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

void main()
{
    uint count = uint(particleBuffer.particles.length());
    // 间接绘制把实例数写到 alive；一个实例固定六个顶点，因此实例索引
    // 才是紧凑存活槽位，顶点索引只决定 quad 角点。
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
        vWorldPosition = vec3(0.0);
        return;
    }

    // 排序开启时 drawIndex 是排序后的槽位，pair.y 才是原始粒子池索引；
    // seed 必须继续使用原始索引，保证噪声相位不随深度排序跳变。
    uint particleIndex = drawIndex;
    if (frame.sortParams.x > 0.5)
    {
        particleIndex = uint(sortedPairs.pairs[drawIndex].y);
    }
    else
    {
        // 未排序路径的 draw 数量来自 GPU 存活计数。
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
        vWorldPosition = vec3(0.0);
        return;
    }

    Particle particle =
        particleBuffer.particles[particleIndex];
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
        vWorldPosition = vec3(0.0);
        return;
    }

    uint cornerIndex = uint(gl_VertexIndex) % 6u;
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
    // UBO 提供的是 inverse projection；取其倒数才与 C++ 的
    // abs(camera.projection(1,1)) 等价，否则屏幕尺寸会被错误放大。
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
    vSeed = float(particleIndex) * 0.61803398875;
    vTime = frame.simulation.z;

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
            float cycle = vTime / totalSeconds;
            if (frame.flipbookParams2.y > 0.5)
            {
                cycle += fract(vSeed);
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
        float phase = vTime * frame.flipbookParams.x;
        if (frame.flipbookParams2.y > 0.5)
        {
            // vSeed 是 index * 0.618…，不是 [0,1) 随机数；必须取小数部分，
            // 否则 loop=false 时绝大多数粒子会被 clamp 到末帧。
            phase += fract(vSeed);
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
        rotationDegrees += fract(vSeed) * 360.0;
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
    // 对齐粒子速度，适合拖尾/火花。速度过短、与世界上方平行或
    // normalize 退化时回退到相机基，避免零面积 quad 与 NaN。
    vec3 billboardRight = frame.cameraRight.xyz;
    vec3 billboardUp = frame.cameraUp.xyz;
    if (frame.billboardParams.x > 0.5)
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

    // size 是世界空间半边长（cm）。
    vec3 worldPosition = particleCenter +
        (billboardRight * (rotatedCorner.x + pivotOffset.x) +
         billboardUp * (rotatedCorner.y + pivotOffset.y)) * size;
    gl_Position =
        frame.viewProjection * vec4(worldPosition, 1.0);
    // 视空间深度取正：相机前方为正，供片元与 sceneDepth 比较。
    vViewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;
    vWorldPosition = worldPosition;

    vColor = EvaluateColorCurve(lifeT);
    vColor.a *= frame.render.w;
    // quad 局部 UV 继续供程序化圆盘使用；图集采样走逐帧 UV rect。
    vec2 localUV = corner * 0.5 + 0.5;
    vUV = localUV;
    vec2 spriteUV = localUV;
    if (frame.flipbookParams3.w > 0.5)
    {
        // rect 是 sprite 空间 [0,1] 的子矩形，索引同样是区间内局部帧号。
        int uvIndex = clamp(
            int(floor(localFrameIndex)), 0, int(frameCount) - 1);
        vec4 rect = FlipbookUvRect(uvIndex);
        spriteUV = mix(rect.xy, rect.zw, localUV);
    }
    vSoftness = frame.render.z;
    vec2 atlasCell = vec2(
        mod(frameIndex, columns),
        floor(frameIndex / columns));
    // 图集按 top-down 行存储，而 billboard 的 vUV 以左下为 0；
    // 在 cell 内翻转 V，避免火焰/烟雾纹理上下颠倒。
    vAtlasUV = (atlasCell +
        vec2(spriteUV.x, 1.0 - spriteUV.y)) / vec2(columns, rows);

    // 图集禁止 mipmap 后仍会被相邻 cell 的线性过滤采到。把 UV 内缩半个
    // texel 到当前 cell 边界内，保留上面的 V 翻转与 cell 布局。
    vec2 atlasSize = vec2(textureSize(particleTexture, 0));
    vec2 halfTexel = vec2(0.5) / max(atlasSize, vec2(1.0));
    vec2 cellMin = atlasCell / vec2(columns, rows);
    vec2 cellMax = (atlasCell + vec2(1.0)) / vec2(columns, rows);
    vAtlasUV = clamp(
        vAtlasUV,
        cellMin + halfTexel,
        cellMax - halfTexel);
}

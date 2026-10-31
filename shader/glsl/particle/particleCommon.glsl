#ifndef PARTICLE_COMMON_GLSL
#define PARTICLE_COMMON_GLSL

// 一个粒子只保存模拟状态；颜色/尺寸是寿命曲线，渲染时由 vertex shader
// 从 authoring 参数求值，避免每帧为颜色写回整段 buffer。
struct Particle
{
    // localSpace 发射器保存局部坐标；其余发射器保存世界坐标。
    vec4 positionAge;   // xyz = simulation-space position, w = age seconds
    vec4 velocityLife;  // xyz = simulation-space velocity, w = lifetime seconds
    // 最近 3 帧的位置快照，供 ribbon 条带使用；xyz 是记录时的模拟空间
    // 位置，w 是记录时的 age。history0 是上一帧位置，history2 最旧。
    vec4 history0;
    vec4 history1;
    vec4 history2;
};

// std140 布局与 particleRenderer.h::FrameUniforms 一一对应。
layout(std140, set = 0, binding = 0) uniform ParticleFrame
{
    mat4 viewProjection;
    mat4 view;
    mat4 invProjection;
    vec4 cameraPosition;
    vec4 cameraRight;
    vec4 cameraUp;
    mat4 localToWorld;
    vec4 gravityDrag;
    vec4 velocityMinLifetimeMin;
    vec4 velocityMaxLifetimeMax;
    vec4 simulation; // spawnRate, dt, time, prewarm
    vec4 render;     // sizeStart, sizeEnd, softness, opacity
    vec4 shape;      // sphere radius, burst, localSpace enabled, reserved
    vec4 softParams; // softParticleDistance, softParticles enabled, reserved
    vec4 colorStart;
    vec4 colorEnd;
    vec4 colorKeys[8];
    vec4 colorKeyTimes[2];
    vec4 sizeKeys[8]; // x = time, y = half-size
    vec4 curveParams; // colorCount, sizeCount, flipbookColumns, flipbookRows
    vec4 flipbookParams; // fps, hasTexture, startFrame, endFrame
    vec4 flipbookParams2; // loop, randomStartFrame, reserved
    vec4 sortParams; // sort enabled, particle pool count, sorted capacity, reserved
    vec4 sortPass;   // bitonic k, bitonic j, sorted capacity, reserved
    // viewport height, max billboard half-size px, prewarm spawnRate, reserved
    vec4 screenParams;
    vec4 flipbookFrameDurations[64]; // 每 vec4 打包 4 帧的秒数
    vec4 flipbookFramePivots[128];   // 每 vec4 打包 2 帧的 (u0,v0,u1,v1)，支持 256 帧
    vec4 flipbookParams3;            // hasDurations, hasPivots, frameCount, hasUVs
    vec4 flipbookFrameUVs[64];       // 每帧一个 (u0,v0,u1,v1) sprite UV rect
    vec4 collisionParams;            // type(0 none,1 plane,2 box,3 capsule), bounce, friction, radius
    vec4 collisionPlane;             // normal.xyz, offset
    vec4 collisionBoxMin;            // min.xyz, killOnCollision
    vec4 collisionBoxMax;            // max.xyz, reserved
    vec4 burstParams;                // x = burstInterval, y/z/w reserved
    // x = base rotation degrees, y = speed degrees/sec, z = random start, w reserved
    vec4 rotationParams;
    vec4 flipbookFrameRotations[16]; // 每 vec4 打包 4 帧的角度（度）
    vec4 flipbookParams4;            // hasRotations, y/z/w reserved
    vec4 collisionParams2;           // ccd enabled, capsule radius, y/z/w reserved
    vec4 collisionCapsuleA;          // 线段起点 A.xyz, 0
    vec4 collisionCapsuleB;          // 线段终点 B.xyz, 0
    // x = ribbon segments, y = tail fade, z = half width (cm), w reserved
    vec4 ribbonParams;
    // x = mesh alignment (0 world, 1 camera, 2 velocity), y/z/w reserved
    vec4 meshParams;
    // mesh local scale; v1 ignores the size curve
    vec4 meshScale;
} frame;

const vec2 kParticleCorners[6] = vec2[6](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0));

bool IsParticleAlive(Particle particle)
{
    return particle.velocityLife.w > 0.0 &&
        particle.positionAge.w < particle.velocityLife.w;
}

float ColorKeyTime(int index)
{
    return index < 4
        ? frame.colorKeyTimes[0][index]
        : frame.colorKeyTimes[1][index - 4];
}

// 帧时长/轴心按需从打包数组里取；GLSL 450 允许 UBO 数组动态索引。
float FlipbookDuration(int index)
{
    return frame.flipbookFrameDurations[index / 4][index % 4];
}

vec2 FlipbookPivot(int index)
{
    vec4 packed = frame.flipbookFramePivots[index / 2];
    return (index & 1) == 0 ? packed.xy : packed.zw;
}

// index 是区间内的局部帧号（0..frameCount-1）；有 UV rect 时每帧一个 vec4。
vec4 FlipbookUvRect(int index)
{
    return frame.flipbookFrameUVs[index];
}

// 逐帧旋转按区间内局部帧号取整；无表时调用方跳过读取。
float FlipbookRotation(int index)
{
    return frame.flipbookFrameRotations[index / 4][index % 4];
}

vec4 EvaluateColorCurve(float t)
{
    int count = int(frame.curveParams.x);
    if (count <= 1)
    {
        return frame.colorKeys[0];
    }
    if (t <= ColorKeyTime(0))
    {
        return frame.colorKeys[0];
    }
    if (t >= ColorKeyTime(count - 1))
    {
        return frame.colorKeys[count - 1];
    }
    for (int index = 0; index < count - 1; ++index)
    {
        float t0 = ColorKeyTime(index);
        float t1 = ColorKeyTime(index + 1);
        if (t >= t0 && t <= t1)
        {
            float alpha = (t - t0) / max(t1 - t0, 1.0e-5);
            return mix(
                frame.colorKeys[index],
                frame.colorKeys[index + 1],
                alpha);
        }
    }
    return frame.colorKeys[count - 1];
}

float EvaluateSizeCurve(float t)
{
    int count = int(frame.curveParams.y);
    if (count <= 1)
    {
        return frame.sizeKeys[0].y;
    }
    if (t <= frame.sizeKeys[0].x)
    {
        return frame.sizeKeys[0].y;
    }
    if (t >= frame.sizeKeys[count - 1].x)
    {
        return frame.sizeKeys[count - 1].y;
    }
    for (int index = 0; index < count - 1; ++index)
    {
        float t0 = frame.sizeKeys[index].x;
        float t1 = frame.sizeKeys[index + 1].x;
        if (t >= t0 && t <= t1)
        {
            float alpha = (t - t0) / max(t1 - t0, 1.0e-5);
            return mix(
                frame.sizeKeys[index].y,
                frame.sizeKeys[index + 1].y,
                alpha);
        }
    }
    return frame.sizeKeys[count - 1].y;
}

#endif

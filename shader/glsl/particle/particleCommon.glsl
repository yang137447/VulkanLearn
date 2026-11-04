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
    vec4 simulation; // effective spawnRate (LOD * instance scale), dt, time, prewarm
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
    // viewport height, max billboard half-size px, prewarm spawnRate with instance scale, reserved
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
    // 场景实例级颜色 tint；rgb/alpha 都乘到最终片元输出
    vec4 colorTint;
    // x = billboard alignment (0 camera, 1 velocity, 2 fixed), y/z/w reserved
    vec4 billboardParams;
    // x = shape code (0 sphere, 1 box, 2 cone, 3 cylinder, 4 disc, 5 ring),
    // y = height (cm), z = innerRadius (cm), w reserved
    vec4 shapeParams;
    // xyz = box half extents (cm), w reserved
    vec4 shapeExtent;
    // x = curl noise strength, y = frequency, z = time speed, w reserved
    vec4 curlParams;
    // x = vortex strength, y = radius, z/w reserved
    vec4 vortexParams;
    // xyz = vortex axis, w = 0
    vec4 vortexAxis;
    // x = attractor strength, y = radius, z/w reserved
    vec4 attractorParams;
    // xyz = attractor position in simulation space, w = 0
    vec4 attractorPosition;
    // xyz = constant wind acceleration in simulation space, w = 0
    vec4 wind;
    // x = lifeT, y = time scale, z/w reserved；count=0 表示恒为 1
    vec4 lifetimeScaleKeys[8];
    // x = key count, y/z/w reserved
    vec4 lifetimeScaleParams;
    // x = mesh 照明开关, y = ambient, z/w reserved
    vec4 meshLightParams;
    // xyz = 表面指向光源的方向
    vec4 meshLightDirection;
    // xyz = 方向光线性 RGB 颜色（不含 lux 强度）
    vec4 meshLightColor;
    // mesh 材质 v1 子集：baseColor / emissive(强度在 w) / PBR 标量
    vec4 materialBaseColor;
    vec4 materialEmissiveStrength;
    // x = roughness, y = metallic, z = hasMaterial, w reserved
    vec4 materialPbr;
    // CSM：4 级 lightViewProj、逐级 bias、split 距离
    mat4 shadowLightViewProj[4];
    vec4 shadowCascadeBias[4];
    vec4 shadowCascadeSplits;
    // x = active cascade count, y = shadow distance, z = enabled, w reserved
    vec4 shadowParams;
    // x = random seed, y = fixed step seconds, z = fixed step index,
    // w = deterministic random enabled
    vec4 determinismParams;
    // x = spawn events, y = death events, z = collision events, w reserved
    vec4 eventParams;
    // SDF 碰撞体：bounds + 分辨率（w 分别是 x/y/z 轴分辨率）
    vec4 collisionSdfBoundsMin;
    vec4 collisionSdfBoundsMax;
    vec4 collisionSdfResolution;
    // x = billboard lighting enabled, y = ambient, z/w reserved.
    // 与 mesh 共用 lightDirection/lightColor，但 billboard 使用面向相机的法线。
    vec4 billboardLightParams;
    // beam 渲染：xyz 世界端点，w 分别为段数与半宽。
    vec4 beamStart;
    vec4 beamEnd;
    // x = 噪声幅度, y = 频率, z = 速度, w = 端点抖动（cm）。
    vec4 beamNoise;
    // x = 并行弧数量, y = branchCount, z = branchStart,
    // w = branchLength。
    vec4 beamParams;
    // fixed billboard 的世界空间法线与上方向（CPU 已归一化）。
    vec4 billboardFixedNormal;
    vec4 billboardFixedUp;
    // x = 分支末端横向偏移（cm，已乘模型最大列缩放），y/z/w 保留。
    vec4 beamBranch;
    // x = distortionStrength（屏幕高度归一化偏移），y = 纹理可用，z/w 保留。
    vec4 distortionParams;
    // beam 逐段控制：颜色 key 覆盖基础色，width key 乘基础半宽。
    vec4 beamColorKeys[8];
    vec4 beamColorKeyTimes[2];
    vec4 beamWidthKeys[8];
    // x = color key count, y = width key count, z/w 保留。
    vec4 beamCurveParams;
    // 逐主弧覆盖：颜色 tint 是基础色曲线的乘数，分支沿用所属主弧。
    vec4 beamArcColorStart[16];
    vec4 beamArcColorEnd[16];
    // x = widthScale, y = intensity, z = noisePhase, w 保留。
    vec4 beamArcParams[16];
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

// 按 lifeT = age / lifetime 取时间倍率：空表恒为 1，首尾外 clamp，
// 段内线性插值，输出限制在 [0,4]。重复 time 取最后一个 <= lifeT 的
// key，和 CPU 的 EvaluateParticleLifetimeScale 保持一致。
float EvaluateLifetimeScale(float lifeT)
{
    int count = int(frame.lifetimeScaleParams.x);
    if (count <= 0)
    {
        return 1.0;
    }
    float t = clamp(lifeT, 0.0, 1.0);
    int index = 0;
    for (int current = 0; current < count; ++current)
    {
        if (frame.lifetimeScaleKeys[current].x <= t)
        {
            index = current;
        }
        else
        {
            break;
        }
    }
    if (index + 1 >= count)
    {
        return clamp(frame.lifetimeScaleKeys[index].y, 0.0, 4.0);
    }
    float t0 = frame.lifetimeScaleKeys[index].x;
    float scale0 = frame.lifetimeScaleKeys[index].y;
    if (t <= t0)
    {
        return clamp(scale0, 0.0, 4.0);
    }
    float span =
        frame.lifetimeScaleKeys[index + 1].x - t0;
    float scale1 = frame.lifetimeScaleKeys[index + 1].y;
    if (span <= 0.0)
    {
        return clamp(scale1, 0.0, 4.0);
    }
    float weight = clamp((t - t0) / span, 0.0, 1.0);
    return clamp(mix(scale0, scale1, weight), 0.0, 4.0);
}

// 粒子 pass 自己声明 shadowMap（set 3 binding 1），因此这里不能复用
// common/lighting.glsl 的 UBOGlobal 版本，只保留同样的 CSM 语义：
// 选择级联 -> light clip 空间转为 [0,1] UV -> 比较采样 -> 3x3 PCF。
int SelectParticleShadowCascade(vec3 worldPos)
{
    int activeCascadeCount = int(frame.shadowParams.x + 0.5);
    if (activeCascadeCount <= 0)
    {
        return 0;
    }
    float viewDepth = -(frame.view * vec4(worldPos, 1.0)).z;
    for (int cascadeIndex = 0; cascadeIndex < 4; ++cascadeIndex)
    {
        if (cascadeIndex >= activeCascadeCount - 1 ||
            viewDepth <= frame.shadowCascadeSplits[cascadeIndex])
        {
            return cascadeIndex;
        }
    }
    return max(activeCascadeCount - 1, 0);
}

float SampleParticleShadowCascade(
    sampler2DArrayShadow shadowMap,
    vec3 worldPos,
    float viewDepth,
    int cascadeIndex)
{
    vec4 clip =
        frame.shadowLightViewProj[cascadeIndex] *
        vec4(worldPos, 1.0);
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0)
    {
        return 1.0;
    }
    vec4 bias = frame.shadowCascadeBias[cascadeIndex];
    float compareDepth = ndc.z - bias.x;
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float shadow = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            shadow += texture(
                shadowMap,
                vec4(
                    uv + vec2(x, y) * texel,
                    float(cascadeIndex),
                    compareDepth));
        }
    }
    return shadow / 9.0;
}

float SampleParticleCsmShadow(
    sampler2DArrayShadow shadowMap,
    vec3 worldPos,
    vec3 worldNormal)
{
    if (frame.shadowParams.z < 0.5 || frame.shadowParams.x < 0.5)
    {
        return 1.0;
    }
    float viewDepth = -(frame.view * vec4(worldPos, 1.0)).z;
    if (viewDepth > frame.shadowParams.y)
    {
        return 1.0;
    }
    int cascadeIndex =
        SelectParticleShadowCascade(worldPos);
    return SampleParticleShadowCascade(
        shadowMap,
        worldPos,
        viewDepth,
        cascadeIndex);
}

// billboard 光照的运行时近似：sprite 没有几何法线，因此使用面向相机的
// 法线。direct 只取场景第一个方向光的 Lambert 项；ambient 是作者常量。
// 该函数同时服务逐发射器 UBO 与全局 gather 记录，参数由调用方解包。
vec3 EvaluateParticleBillboardLighting(
    vec3 baseColor,
    vec3 worldPosition,
    vec3 lightingNormal,
    vec4 lightParams,
    vec3 lightDirection,
    vec3 lightColor,
    float shadowEnabled,
    sampler2DArrayShadow shadowMap)
{
    if (lightParams.x < 0.5)
    {
        return baseColor;
    }
    vec3 normal = normalize(lightingNormal);
    vec3 direction = normalize(lightDirection);
    float ndl = max(dot(normal, direction), 0.0);
    float shadow = 1.0;
    if (shadowEnabled > 0.5)
    {
        shadow = SampleParticleCsmShadow(
            shadowMap,
            worldPosition,
            normal);
    }
    float ambient = clamp(lightParams.y, 0.0, 1.0);
    return baseColor *
        (ambient + lightColor * ndl * shadow);
}

#endif

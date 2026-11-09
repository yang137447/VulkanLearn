#ifndef PARTICLE_RIBBON_GEOMETRY_GLSL
#define PARTICLE_RIBBON_GEOMETRY_GLSL

#include "particleCommon.glsl"

// 只依赖 FrameUniforms 与 Particle 的 ribbon 几何；不声明 sampler/SSBO，
// 因此可见/阴影顶点着色器与全局 gather compute 可以共用同一份数学。
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

vec3 RotateRibbonSide(vec3 side, vec3 axis, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return side * c + cross(axis, side) * s +
        axis * dot(axis, side) * (1.0 - c);
}

bool BuildParticleRibbonPath(
    Particle particle,
    out vec3 points[17],
    out float cumulative[17],
    out float totalLength)
{
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
    cumulative[0] = 0.0;
    for (int index = 1; index < 17; ++index)
    {
        cumulative[index] = cumulative[index - 1] +
            length(points[index] - points[index - 1]);
    }
    totalLength = cumulative[16];
    return totalLength > 1.0e-4;
}

// 单个 ribbon 顶点：t 是沿条带的归一化位置，lateral 是 -1..1。
void BuildParticleRibbonCorner(
    vec3 points[17],
    float cumulative[17],
    float totalLength,
    float t,
    float lateral,
    float lifeT,
    out vec3 worldPosition,
    out float width,
    out vec4 color,
    out vec2 uv)
{
    vec3 pathPosition = RibbonPointAt(
        points, cumulative, totalLength, t);
    vec3 tangent = RibbonTangentAt(
        points, cumulative, totalLength, t);
    vec3 viewOffset = frame.cameraPosition.xyz - pathPosition;
    float viewLength = length(viewOffset);
    vec3 viewDirection = viewLength > 1.0e-5
        ? viewOffset / viewLength
        : frame.cameraRight.xyz;

    vec3 side = cross(tangent, viewDirection);
    float sideLength = length(side);
    side = sideLength > 1.0e-5
        ? side / sideLength
        : frame.cameraRight.xyz;
    float twist = radians(frame.ribbonShapeParams.y);
    if (abs(twist) > 1.0e-5 && dot(tangent, tangent) > 1.0e-8)
    {
        side = RotateRibbonSide(side, tangent, twist);
    }
    width = frame.ribbonParams.z * EvaluateRibbonWidth(t) *
        frame.lodParams.x;
    float lateralOffset =
        lateral + frame.ribbonShapeParams.x;
    worldPosition = pathPosition + side * (lateralOffset * width);

    color = EvaluateRibbonColor(t, EvaluateColorCurve(lifeT));
    color.a *= frame.render.w;
    color.a *= mix(
        1.0,
        1.0 - t,
        clamp(frame.ribbonParams.y, 0.0, 1.0));

    float tiledX = t * max(frame.ribbonUVParams.x, 1.0e-4);
    float spriteX = frame.ribbonUVParams.x > 1.0
        ? fract(tiledX)
        : clamp(tiledX, 0.0, 1.0);
    uv = vec2(spriteX, lateral * 0.5 + 0.5);
}

// 一个 segment 的四个角（-1,-1 / -1,+1 / +1,-1 / +1,+1）与首尾颜色。
bool BuildParticleRibbonQuad(
    Particle particle,
    uint particleIndex,
    uint segment,
    uint segments,
    out vec3 corner0,
    out vec3 corner1,
    out vec3 corner2,
    out vec3 corner3,
    out vec4 color0,
    out vec4 color1,
    out vec2 uv0,
    out vec2 uv1,
    out float widthSum)
{
    vec3 points[17];
    float cumulative[17];
    float totalLength = 0.0;
    if (!BuildParticleRibbonPath(
            particle, points, cumulative, totalLength))
    {
        return false;
    }
    float lifeT = clamp(
        particle.positionAge.w /
            max(particle.velocityLife.w, 1.0e-4),
        0.0,
        1.0);
    float startT = float(segment) / float(segments);
    float endT = float(segment + 1u) / float(segments);
    float widthStart = 0.0;
    float widthEnd = 0.0;
    vec2 startUV;
    vec2 endUV;
    BuildParticleRibbonCorner(
        points, cumulative, totalLength, startT, -1.0, lifeT,
        corner0, widthStart, color0, startUV);
    BuildParticleRibbonCorner(
        points, cumulative, totalLength, startT, 1.0, lifeT,
        corner1, widthStart, color0, startUV);
    BuildParticleRibbonCorner(
        points, cumulative, totalLength, endT, -1.0, lifeT,
        corner2, widthEnd, color1, endUV);
    BuildParticleRibbonCorner(
        points, cumulative, totalLength, endT, 1.0, lifeT,
        corner3, widthEnd, color1, endUV);
    // 四角 UV 的 v 必须覆盖 0..1；上面两次相同 lateral 的调用会覆盖
    // startUV/endUV 的 v 分量，因此这里显式重建。
    uv0 = vec2(startUV.x, 0.0);
    uv1 = vec2(endUV.x, 1.0);
    widthSum = widthStart + widthEnd;
    return true;
}

#endif

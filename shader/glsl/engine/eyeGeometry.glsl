#ifndef VL_ENGINE_EYE_GEOMETRY_GLSL
#define VL_ENGINE_EYE_GEOMETRY_GLSL

// Eye 的几何快照由 Base Pass 生成一次，Forward/Deferred 只消费冻结字段。
// 这样 Deferred 不需要重新访问 MI 纹理，也不会让不同路径各自猜测 iris frame。
struct EyeGeometrySnapshot
{
    vec3 irisPlanePoint;
    vec3 refractedViewDirection;
    vec2 irisUv;
    float irisHitDistance;
    float validIrisHit;
    float irisRadial;
    float pupilMask;
    float limbusMask;
};

EyeGeometrySnapshot CreateDefaultEyeGeometrySnapshot()
{
    EyeGeometrySnapshot snapshot;
    snapshot.irisPlanePoint = vec3(0.0);
    snapshot.refractedViewDirection = vec3(0.0, 0.0, -1.0);
    snapshot.irisUv = vec2(0.0);
    snapshot.irisHitDistance = 0.0;
    snapshot.validIrisHit = 0.0;
    snapshot.irisRadial = 1.0;
    snapshot.pupilMask = 0.0;
    snapshot.limbusMask = 0.0;
    return snapshot;
}

EyeGeometrySnapshot EvaluateEyeGeometry(
    vec3 worldPosition,
    vec3 corneaNormal,
    vec3 irisPlaneNormal,
    vec3 tangent,
    float tangentHandedness,
    vec3 viewDirectionToCamera,
    float corneaIor,
    float irisDistance,
    float irisRadius,
    float pupilRadius,
    float limbusWidth,
    float uvHandedness)
{
    EyeGeometrySnapshot snapshot = CreateDefaultEyeGeometrySnapshot();
    snapshot.irisPlanePoint = worldPosition - irisPlaneNormal * irisDistance;
    snapshot.refractedViewDirection = refract(
        -viewDirectionToCamera,
        corneaNormal,
        1.0 / corneaIor);

    float denominator = dot(
        irisPlaneNormal,
        snapshot.refractedViewDirection);
    if (abs(denominator) <= 1.0e-4)
    {
        return snapshot;
    }

    float hitDistance = dot(
        irisPlaneNormal,
        snapshot.irisPlanePoint - worldPosition) /
        denominator;
    if (hitDistance <= 0.0)
    {
        return snapshot;
    }

    vec3 hitPoint = worldPosition +
        snapshot.refractedViewDirection * hitDistance;
    vec3 offset = hitPoint - snapshot.irisPlanePoint;
    float radial = length(offset) / irisRadius;
    if (radial > 1.0)
    {
        return snapshot;
    }

    vec3 bitangent = normalize(
        cross(irisPlaneNormal, tangent) * tangentHandedness);
    snapshot.irisUv = vec2(
        dot(offset, tangent) / (2.0 * irisRadius) + 0.5,
        dot(offset, bitangent) / (2.0 * irisRadius) + 0.5);
    if (uvHandedness < 0.0)
    {
        snapshot.irisUv.x = 1.0 - snapshot.irisUv.x;
    }
    snapshot.irisHitDistance = hitDistance;
    snapshot.validIrisHit = 1.0;
    snapshot.irisRadial = radial;
    float pupilRatio = pupilRadius / irisRadius;
    float limbusStart = 1.0 - limbusWidth / irisRadius;
    snapshot.pupilMask = 1.0 - smoothstep(
        pupilRatio,
        pupilRatio + 0.04,
        radial);
    snapshot.limbusMask = smoothstep(
        limbusStart,
        1.0,
        radial);
    return snapshot;
}

#endif

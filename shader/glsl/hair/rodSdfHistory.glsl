#ifndef VL_ROD_SDF_HISTORY
#define VL_ROD_SDF_HISTORY

layout(set = 0, binding = 26, std430) buffer RodSdfHistory
{
    vec4 originCell;
    uvec4 dims;
    vec4 range;
    mat4 worldToHair;
    vec4 timing;
    uint cells[];
} sdfHistory;

float RodPreviousSdf(vec3 worldPosition)
{
    vec3 local = (sdfHistory.worldToHair * vec4(worldPosition * 100.0, 1)).xyz;
    vec3 coordinate = (local - sdfHistory.originCell.xyz) / sdfHistory.originCell.w;
    if (any(lessThan(coordinate, vec3(0))) ||
        any(greaterThan(coordinate, vec3(sdfHistory.dims.xyz))))
        return SdfOutOfRangeDistance;
    ivec3 base;
    vec3 fraction;
    fraction.x = SdfCoordinate(coordinate.x - 0.5, float(sdfHistory.dims.x - 1u), base.x);
    fraction.y = SdfCoordinate(coordinate.y - 0.5, float(sdfHistory.dims.y - 1u), base.y);
    fraction.z = SdfCoordinate(coordinate.z - 0.5, float(sdfHistory.dims.z - 1u), base.z);
    ivec3 high = min(base + ivec3(1), ivec3(sdfHistory.dims.xyz) - ivec3(1));
    float interpolated = 0.0;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        bvec3 upper = bvec3((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u);
        ivec3 coordinateIndex = mix(base, high, upper);
        uint index = uint(coordinateIndex.x) + sdfHistory.dims.x *
            (uint(coordinateIndex.y) + sdfHistory.dims.y * uint(coordinateIndex.z));
        uint bits = sdfHistory.cells[index];
        if (bits == SdfInitialBits)
            return SdfOutOfRangeDistance;
        vec3 weights = mix(vec3(1) - fraction, fraction, upper);
        interpolated += SdfIFloatFlip(bits) * weights.x * weights.y * weights.z;
    }
    return interpolated;
}

vec3 RodSdfSurfaceVelocity(vec3 position, vec3 normal, float currentDistance)
{
    if (rod.flags.x != 0u)
        return vec3(0);
    float elapsed = rod.timing.x - sdfHistory.timing.x;
    if (!(elapsed > 1.0e-7) || sdfHistory.timing.w == 0.0)
        return vec3(0);
    float previousDistance = RodPreviousSdf(position);
    if (previousDistance >= SdfOutOfRangeDistance)
        return vec3(0);
    return normal * ((previousDistance - currentDistance) * 0.01 / elapsed);
}

#endif

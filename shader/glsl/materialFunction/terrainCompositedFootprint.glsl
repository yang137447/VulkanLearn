#ifndef VL_TERRAIN_COMPOSITED_FOOTPRINT_GLSL
#define VL_TERRAIN_COMPOSITED_FOOTPRINT_GLSL

const int TerrainCompositedMaxTaps = 8;

struct TerrainCompositedFootprint
{
    vec2 majorUv;
    float targetMip;
    int tapCount;
};

TerrainCompositedFootprint TerrainCompositedFootprintFromGradients(
    vec2 uvDx, vec2 uvDy, vec2 resolution, float maxMip)
{
    vec2 deltaX = uvDx * resolution;
    vec2 deltaY = uvDy * resolution;
    float axisX = deltaX.x * deltaX.x + deltaY.x * deltaY.x;
    float axisY = deltaX.y * deltaX.y + deltaY.y * deltaY.y;
    float crossAxis = deltaX.x * deltaX.y + deltaY.x * deltaY.y;
    float discriminant = length(vec2(axisX - axisY, 2.0 * crossAxis));
    float majorSquared = max(0.5 * (axisX + axisY + discriminant), 1.0e-12);
    float determinant = deltaX.x * deltaY.y - deltaX.y * deltaY.x;
    float major = sqrt(majorSquared);
    float minor = abs(determinant) / major;
    vec2 firstAxis = vec2(crossAxis, majorSquared - axisX);
    vec2 secondAxis = vec2(majorSquared - axisY, crossAxis);
    vec2 direction = dot(firstAxis, firstAxis) > dot(secondAxis, secondAxis)
        ? firstAxis : secondAxis;
    direction = dot(direction, direction) > 1.0e-12
        ? normalize(direction) : vec2(1.0, 0.0);
    float filteredMinor = max(minor, major / float(TerrainCompositedMaxTaps));
    TerrainCompositedFootprint result;
    result.majorUv = direction * major / max(resolution, vec2(1.0));
    result.targetMip = clamp(log2(max(filteredMinor, 1.0e-6)), 0.0, maxMip);
    result.tapCount = clamp(int(ceil(major / max(filteredMinor, 1.0))),
        1, TerrainCompositedMaxTaps);
    return result;
}

vec2 TerrainCompositedTapUv(
    vec2 localUv, TerrainCompositedFootprint footprint, int tap)
{
    return localUv + footprint.majorUv *
        ((float(tap) + 0.5) / float(footprint.tapCount) - 0.5);
}

#endif

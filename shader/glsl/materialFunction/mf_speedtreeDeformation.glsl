#ifndef VL_MATERIAL_FUNCTION_SPEEDTREE_DEFORMATION_GLSL
#define VL_MATERIAL_FUNCTION_SPEEDTREE_DEFORMATION_GLSL

const float SPEEDTREE_GOLDEN_ANGLE = 2.39996323;

float SpeedTreeNoiseHash(float value)
{
    return fract(sin(value) * 10000.0);
}

float SpeedTreeNoiseHash(vec2 value)
{
    return fract(
        10000.0 *
        sin(17.0 * value.x + value.y * 0.1) *
        (0.1 + abs(sin(value.y * 13.0 + value.x))));
}

float SpeedTreeQNoise(vec2 value)
{
    vec2 cell = floor(value);
    vec2 fraction = fract(value);
    float a = SpeedTreeNoiseHash(cell);
    float b = SpeedTreeNoiseHash(cell + vec2(1.0, 0.0));
    float c = SpeedTreeNoiseHash(cell + vec2(0.0, 1.0));
    float d = SpeedTreeNoiseHash(cell + vec2(1.0, 1.0));
    vec2 smoothFraction = fraction * fraction * (vec2(3.0) - vec2(2.0) * fraction);
    return mix(a, b, smoothFraction.x) +
        (c - a) * smoothFraction.y * (1.0 - smoothFraction.x) +
        (d - b) * smoothFraction.x * smoothFraction.y;
}

vec3 SpeedTreeRuntimeNoise2DFlat(vec3 noisePosition)
{
    // Runtime SDK samples source-space XY. VulkanLearn stores the same vector
    // in engine Y-up space using (x, z, -y), so source Y is engine -Z.
    vec2 flatPosition = vec2(noisePosition.x, -noisePosition.z);
    float noiseX = SpeedTreeQNoise(flatPosition * 20.0) - 0.5;
    float noiseY = SpeedTreeQNoise(flatPosition.yx * 10.0);
    return vec3(noiseX, noiseY, 0.0);
}

vec3 DecodeSpeedTreeV10Direction(float normalizedPackedDirection)
{
    float tableIndex = floor(normalizedPackedDirection * 255.0 + 0.5);
    float sourceZ = 0.99609375 - 0.0078125 * tableIndex;
    float radial = sqrt(max(1.0 - sourceZ * sourceZ, 0.0));
    float angle = tableIndex * SPEEDTREE_GOLDEN_ANGLE;
    vec3 sourceDirection = vec3(
        cos(angle) * radial,
        sin(angle) * radial,
        sourceZ);
    return vec3(sourceDirection.x, sourceDirection.z, -sourceDirection.y);
}

vec3 DecodeSpeedTreeV10NoiseOffset(float normalizedPackedOffset)
{
    float packedOffset = floor(normalizedPackedOffset * 255.0 + 0.5);
    float sourceZ = floor(packedOffset / 81.0);
    packedOffset -= sourceZ * 81.0;
    float sourceY = floor(packedOffset / 9.0);
    float sourceX = packedOffset - sourceY * 9.0;
    vec3 sourceOffset = vec3(sourceX / 8.0, sourceY / 8.0, sourceZ / 2.0);
    return vec3(sourceOffset.x, sourceOffset.z, -sourceOffset.y);
}

vec3 SpeedTreeLocalWindDirection()
{
    return uboM.speedTreeWindVector.xyz;
}

vec3 SpeedTreeRippleMotion(
    vec3 localPosition,
    vec3 windDirection,
    vec3 globalNoisePosition,
    float rippleWeight)
{
    vec3 noisePosition =
        globalNoisePosition +
        uboM.speedTreeRippleNoisePosTurbulenceIndependence.xyz +
        localPosition * uboM.speedTreeRippleNoisePosTurbulenceIndependence.w +
        windDirection *
            uboM.speedTreeRipplePlanarDirectionalFlexibilityShimmer.z *
            rippleWeight;
    vec3 noise = SpeedTreeRuntimeNoise2DFlat(noisePosition);
    noise.x += 0.25;

    vec3 motion =
        windDirection * noise.x *
            uboM.speedTreeRipplePlanarDirectionalFlexibilityShimmer.y +
        vec3(0.0, 1.0, 0.0) * noise.y *
            uboM.speedTreeRipplePlanarDirectionalFlexibilityShimmer.x;
    return motion * rippleWeight;
}

vec3 SpeedTreeBranchPosition(
    vec3 localPosition,
    vec3 windDirection,
    vec3 globalNoisePosition,
    vec4 packedBranch,
    float stretchLimit,
    vec4 noisePositionTurbulenceIndependence,
    vec4 bendOscillationTurbulenceFlexibility)
{
    float branchWeight = packedBranch.x;
    float branchLength = branchWeight * stretchLimit;
    if (branchLength <= 0.0)
    {
        return localPosition;
    }

    vec3 branchDirection = DecodeSpeedTreeV10Direction(packedBranch.y);
    vec3 branchNoiseOffset =
        DecodeSpeedTreeV10NoiseOffset(packedBranch.z) *
        (uboM.speedTreeTreeBoundsMax.xyz - uboM.speedTreeTreeBoundsMin.xyz);
    vec3 branchAnchor = localPosition - branchDirection * branchLength;
    vec3 positionFromAnchor = localPosition - branchAnchor;

    float branchDotWind = dot(branchDirection, windDirection);
    vec3 effectiveWind = normalize(
        windDirection + vec3(0.0, 1.0, 0.0) * branchDotWind * branchDotWind);

    // v10's Standard packer already expands the integer offset into tree
    // units before RuntimeSdk consumes it. The branch independence scalar is
    // therefore applied once; multiplying it by tree height again is the
    // legacy v9 normalized-offset path and causes violent phase excursions.
    float branchIndependence = noisePositionTurbulenceIndependence.w;
    vec3 noisePosition =
        globalNoisePosition +
        noisePositionTurbulenceIndependence.xyz +
        branchNoiseOffset * branchIndependence +
        effectiveWind * bendOscillationTurbulenceFlexibility.w * branchWeight;
    vec3 noise = SpeedTreeRuntimeNoise2DFlat(noisePosition);

    vec3 turbulentOscillation =
        vec3(0.0, 1.0, 0.0) * bendOscillationTurbulenceFlexibility.z;
    vec3 motion =
        (effectiveWind * noise.x + turbulentOscillation * noise.y) *
        bendOscillationTurbulenceFlexibility.y;
    motion +=
        effectiveWind * bendOscillationTurbulenceFlexibility.x * (1.0 - noise.z);
    motion *= branchWeight;

    return normalize(positionFromAnchor + motion) * branchLength + branchAnchor;
}

vec3 SpeedTreeSharedPosition(
    vec3 localPosition,
    vec3 windDirection,
    vec3 globalNoisePosition)
{
    float positionLength = length(localPosition);
    if (positionLength <= 0.0)
    {
        return localPosition;
    }

    float treeMaxHeight = uboM.speedTreeTreeBoundsMax.y;
    float sharedHeightStart = uboM.speedTreeTreeExtentsSharedHeightStart.w;
    float heightWeight =
        max(localPosition.y - treeMaxHeight * sharedHeightStart, 0.0) /
        treeMaxHeight;
    heightWeight *= heightWeight;

    vec4 sharedState =
        uboM.speedTreeSharedBendOscillationTurbulenceFlexibility;
    vec3 noisePosition =
        globalNoisePosition +
        uboM.speedTreeSharedNoisePosTurbulenceIndependence.xyz +
        windDirection * sharedState.w * heightWeight;
    vec3 noise = SpeedTreeRuntimeNoise2DFlat(noisePosition);

    vec3 turbulentOscillation =
        cross(windDirection, vec3(0.0, 1.0, 0.0)) * sharedState.z;
    vec3 motion =
        (windDirection * noise.x + turbulentOscillation * noise.y) * sharedState.y +
        windDirection * sharedState.x * (1.0 - noise.z);
    motion *= heightWeight;
    return normalize(localPosition + motion) * positionLength;
}

// Runtime SDK 9/10 evaluates leaf ripple first, then the finer branch level,
// the coarser branch level, and finally the shared whole-tree bend.
vec3 EvaluateSpeedTreeDeformedPosition(
    vec3 localPosition,
    vec3 localNormal,
    vec4 vertexColor,
    vec2 texCoord,
    vec4 localTangent,
    vec4 speedTreeWindBranch1,
    vec4 speedTreeWindBranch2,
    out vec3 deformedNormal)
{
    deformedNormal = localNormal;
    vec3 windDirection = SpeedTreeLocalWindDirection();
    if (dot(windDirection, windDirection) == 0.0)
    {
        return localPosition;
    }

    vec3 treeWorldPosition = (uboM.model * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    // Runtime SDK wind randomness is an instance option. The tree world
    // position is used as a stable identifier to decorrelate nearby instances.
    vec3 globalNoisePosition =
        treeWorldPosition * uboM.speedTreeBranchStretchLimits.z;
    vec3 windyPosition = localPosition;

    vec3 rippleMotion = SpeedTreeRippleMotion(
        windyPosition,
        windDirection,
        globalNoisePosition,
        speedTreeWindBranch1.w);
    windyPosition += rippleMotion;
    deformedNormal = normalize(
        deformedNormal -
        rippleMotion * uboM.speedTreeRipplePlanarDirectionalFlexibilityShimmer.w);

    // Runtime SDK 10 只在 Ripple Shimmer 阶段近似调整法线；Branch1、
    // Branch2 与 Shared 仅形变位置，且不重建切线。这里保持官方风动合同，
    // 不额外重建 TBN，避免引入非 SDK 行为和额外顶点开销。

    windyPosition = SpeedTreeBranchPosition(
        windyPosition,
        windDirection,
        globalNoisePosition,
        speedTreeWindBranch2,
        uboM.speedTreeBranchStretchLimits.y,
        uboM.speedTreeBranch2NoisePosTurbulenceIndependence,
        uboM.speedTreeBranch2BendOscillationTurbulenceFlexibility);
    windyPosition = SpeedTreeBranchPosition(
        windyPosition,
        windDirection,
        globalNoisePosition,
        speedTreeWindBranch1,
        uboM.speedTreeBranchStretchLimits.x,
        uboM.speedTreeBranch1NoisePosTurbulenceIndependence,
        uboM.speedTreeBranch1BendOscillationTurbulenceFlexibility);
    return SpeedTreeSharedPosition(
        windyPosition,
        windDirection,
        globalNoisePosition);
}

#endif

#include "rodCommon.glsl"
#ifdef VL_ROD_V3_FOLLOW
layout(set = 0, binding = 27, std140) uniform RodV3FollowControls
{
    vec4 originTolerance;
    vec4 differences;
} implicitControls;
layout(set = 0, binding = 2, std430) buffer RodFollowPrevious
{
    vec4 positions[];
} followPrevious;
layout(set = 0, binding = 3, std430) buffer RodFollowPreviousPrevious
{
    vec4 positions[];
} followPreviousPrevious;
#endif
#ifdef VL_ROD_V3_DISPLACEMENT_FOLLOW
#include "rodV3Reference.glsl"
#endif
#ifdef VL_ROD_V3_COMPENSATED_FOLLOW
layout(set = 0, binding = 30, std430) readonly buffer RodFollowLowParts
{
    vec4 values[];
} followLowParts;
#endif

layout(local_size_x = 32) in;
shared uint sourceSlot;
layout(set = 0, binding = 20, std430) readonly buffer FollowGuideIndices
{
    uint indices[];
} followGuides;
layout(set = 0, binding = 21, std430) readonly buffer FollowOffsets
{
    vec4 offsets[];
} followOffsets;
layout(set = 0, binding = 22, std430) readonly buffer FollowReferences
{
    vec4 references[];
} followReferences;

vec4 RodPointFrame(vec4 first, vec4 last)
{
    if (dot(first, last) < 0.0)
        last = -last;
    return normalize(first + last);
}

void main()
{
    uint guide = gl_WorkGroupID.x;
    uint point = guide * 32u + gl_LocalInvocationID.x;
    if (point >= params.counts.z)
        return;
    uint source = followGuides.indices[guide];
    if (source == guide)
        return;
    if (gl_LocalInvocationID.x == 0u)
    {
        uint lower = 0u;
        uint upper = rod.counts.x;
        while (lower < upper)
        {
            uint middle = (lower + upper) / 2u;
            if (simulationGuides.indices[middle] < source)
                lower = middle + 1u;
            else
                upper = middle;
        }
        sourceSlot = lower < rod.counts.x && simulationGuides.indices[lower] == source ?
            lower : rod.counts.x;
    }
    barrier();
    if (sourceSlot >= rod.counts.x)
        return;
    uint slot = sourceSlot;
    if (rodState.guides[slot].diagnostic.x != 0.0)
        return;
    uint local = point % 32u;
    uint right = min(local, 30u);
    uint left = local == 0u ? 0u : local - 1u;
    vec4 currentFrame = RodPointFrame(
        rodState.guides[slot].segments[left].orientation,
        rodState.guides[slot].segments[right].orientation);
    vec4 restFrame = RodPointFrame(
        rodState.guides[slot].segments[left].restOrientation,
        rodState.guides[slot].segments[right].restOrientation);
    uint sourcePoint = source * 32u + local;
    vec3 tangent = local < 31u ?
        restPositions.positions[sourcePoint + 1u].xyz - restPositions.positions[sourcePoint].xyz :
        restPositions.positions[sourcePoint].xyz - restPositions.positions[sourcePoint - 1u].xyz;
    vec3 first = normalize(tangent);
    vec3 reference = followReferences.references[guide].xyz;
    vec3 second = reference - first * dot(first, reference);
    if (dot(second, second) <= 1.0e-10)
    {
        vec3 absolute = abs(first);
        vec3 fallback = absolute.x <= absolute.y && absolute.x <= absolute.z ?
            vec3(1, 0, 0) : (absolute.y <= absolute.z ? vec3(0, 1, 0) : vec3(0, 0, 1));
        second = fallback - first * dot(fallback, first);
    }
    second = normalize(second);
    vec3 third = normalize(cross(first, second));
    vec3 authoredOffset = mat3(first, second, third) * followOffsets.offsets[point].xyz * 0.01;
    vec3 materialOffset = transpose(RodRotation(restFrame)) * authoredOffset;
    vec3 worldPosition = rodState.guides[slot].points[local].position.xyz +
        RodRotation(currentFrame) * materialOffset;
#ifdef VL_ROD_V3_COMPENSATED_FOLLOW
    worldPosition += followLowParts.values[slot * 63u + local].xyz;
#endif
#ifdef VL_ROD_V3_FOLLOW
    worldPosition += implicitControls.originTolerance.xyz;
#endif
#ifdef VL_ROD_V3_DISPLACEMENT_FOLLOW
    worldPosition += displacementReferences.guides[slot].positions[local].xyz;
#endif
    vec4 published = vec4(
        (rod.worldToHair * vec4(worldPosition * 100.0, 1)).xyz, 1);
#ifdef VL_ROD_V3_FOLLOW
    followPreviousPrevious.positions[point] = rod.flags.x != 0u ?
        published : followPrevious.positions[point];
    followPrevious.positions[point] = rod.flags.x != 0u ?
        published : currentPositions.positions[point];
#endif
    currentPositions.positions[point] = published;
}

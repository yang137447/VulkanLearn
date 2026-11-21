#ifndef VL_ROD_V3_REFERENCE
#define VL_ROD_V3_REFERENCE

struct RodV3Reference
{
    vec4 positions[32];
    vec4 edges[31];
    vec4 frames[31];
    vec4 rootAnchor;
    vec4 rootRotation;
};

layout(set = 0, binding = 31, std430) buffer RodV3References
{
    RodV3Reference guides[];
} displacementReferences;
layout(set = 0, binding = 32, std430) buffer RodV3FrameDeltas
{
    vec4 values[];
} frameDeltas;

#endif

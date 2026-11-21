#ifndef VL_ROD_COMMON
#define VL_ROD_COMMON

#include "tressfxSimCommon.glsl"
#include "rodMath.glsl"

struct RodPoint
{
    vec4 position;
    vec4 velocity;
    vec4 rootLocal;
    vec4 massSupport;
    vec4 target;
};

struct RodSegment
{
    vec4 orientation;
    vec4 angularVelocity;
    vec4 restOrientation;
    vec4 restStrainLength;
    vec4 restJointLength;
    vec4 inertia;
    vec4 shearCompliance;
    vec4 bendCompliance;
};

struct RodGuide
{
    RodPoint points[32];
    RodSegment segments[31];
    vec4 diagnostic;
    vec4 energies;
    vec4 residuals;
    vec4 timing;
};

layout(set = 0, binding = 23, std430) buffer RodStates
{
    RodGuide guides[];
} rodState;

layout(set = 0, binding = 24, std140) uniform RodPhysicalParams
{
    mat4 hairToWorld;
    mat4 worldToHair;
    mat4 roots[9];
    vec4 gravityStep;
    vec4 material;
    vec4 support;
    vec4 timing;
    uvec4 counts;
    uvec4 flags;
    GuideCollider capsules[9][8];
    vec4 windDrag;
    vec4 probeAcceleration;
} rod;

layout(set = 0, binding = 19, std430) readonly buffer SimulationGuideIndices
{
    uint indices[];
} simulationGuides;

layout(set = 0, binding = 4, std430) readonly buffer RestPositions
{
    vec4 positions[];
} restPositions;

layout(set = 0, binding = 1, std430) buffer CurrentPositions
{
    vec4 positions[];
} currentPositions;

#endif

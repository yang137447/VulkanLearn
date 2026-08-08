#ifndef VL_UBO_SPEEDTREE_WIND_FIELDS_GLSL
#define VL_UBO_SPEEDTREE_WIND_FIELDS_GLSL

// SpeedTree Runtime SDK 10 / Games 9 wind state for one mesh profile.
// xyz: normalized object-local wind direction, w: combined wind strength
vec4 speedTreeWindVector;
vec4 speedTreeTreeExtentsSharedHeightStart;
vec4 speedTreeTreeBoundsMin;
vec4 speedTreeTreeBoundsMax;
// x/y: branch stretch limits, z: instance wind independence, w: import scale
vec4 speedTreeBranchStretchLimits;
vec4 speedTreeSharedNoisePosTurbulenceIndependence;
vec4 speedTreeSharedBendOscillationTurbulenceFlexibility;
vec4 speedTreeBranch1NoisePosTurbulenceIndependence;
vec4 speedTreeBranch1BendOscillationTurbulenceFlexibility;
vec4 speedTreeBranch2NoisePosTurbulenceIndependence;
vec4 speedTreeBranch2BendOscillationTurbulenceFlexibility;
vec4 speedTreeRippleNoisePosTurbulenceIndependence;
vec4 speedTreeRipplePlanarDirectionalFlexibilityShimmer;

#endif

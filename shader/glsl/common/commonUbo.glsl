#ifndef VL_COMMON_UBO_GLSL
#define VL_COMMON_UBO_GLSL

layout(std140, set = 0, binding = 0) uniform UBOGlobal{
    #include "uboGlobalFields.glsl"
} uboVP;

layout(std140, set = 2, binding = 0) uniform UBOModel{
    mat4 model;
    mat4 previousModel;
    #include "uboSpeedTreeWindFields.glsl"
    vec4 selectionData;
} uboM;

#endif

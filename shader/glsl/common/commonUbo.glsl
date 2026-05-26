#ifndef VL_COMMON_UBO_GLSL
#define VL_COMMON_UBO_GLSL

layout(std140, set = 0, binding = 0) uniform UBOGlobal{
    mat4 view;
    mat4 projection;
    mat4 invView;
    mat4 invProjection;
    mat4 viewProjection;
    mat4 invViewProjection;
    mat4 lightViewProj;
    vec3 cameraPosition;
    int debugViewMode;
    float environmentIntensity;
    vec4 environmentSH[9];
    mat4 previousViewProjection;
} uboVP;

layout(std140, set = 2, binding = 0) uniform UBOModel{
    mat4 model;
    mat4 previousModel;
} uboM;

#endif

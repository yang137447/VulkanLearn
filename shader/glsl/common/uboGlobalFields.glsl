#ifndef VL_UBO_GLOBAL_FIELDS_GLSL
#define VL_UBO_GLOBAL_FIELDS_GLSL

mat4 view;
mat4 projection;
mat4 invView;
mat4 invProjection;
mat4 viewProjection;
mat4 invViewProjection;
mat4 lightViewProj[4];
vec4 cascadeSplits;
vec4 shadowBias[4];
vec3 cameraPosition;
int debugViewMode;
mat4 previousViewProjection;

// 环境相关参数
float environmentIntensity;
vec4 environmentSH[9];
vec4 sunDirectionIntensity;
vec4 sunColorAngularRadius;
vec4 zenithColor;
vec4 horizonColor;
vec4 groundColor;
vec4 scatteringControls;
vec4 cloudControls;

#endif
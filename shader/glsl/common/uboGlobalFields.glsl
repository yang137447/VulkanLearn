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
// 0 = HDRI，1 = 程序化天空；字段放在 intensity 后以保持后续 environmentSH 对齐。
int environmentType;
vec4 environmentSH[9];
#include "skyParametersFields.glsl"

#endif

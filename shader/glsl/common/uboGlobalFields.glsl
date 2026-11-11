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
// x: 级联过渡比例，y: 最远距离淡出比例，z: 活动级联数，w: 阴影距离。
vec4 csmParameters;
vec3 cameraPosition;
int debugViewMode;
mat4 previousViewProjection;

// 环境相关参数
float environmentIntensity;
// 0 = HDRI，1 = 程序化天空；字段放在 intensity 后以保持后续 environmentSH 对齐。
int environmentType;
vec4 environmentSH[9];
#include "skyParametersFields.glsl"

// 局部光阴影：xyz = 灯位置 cm，w = far cm。
vec4 localShadowLightPos;
// x = mode（0 = 透视/hardware depth，1 = 八面体），y = 八面体象限 0..7，
// z = 接收面 bias cm，w 保留。
vec4 localShadowParams;

// 体积雾（volumetricFog compute + fogComposite）。
// density / heightFalloff 以“每米”书写，shader 内统一除以 100 转成每 cm，
// 这样资产参数可读且不依赖世界单位。
// x = density(1/m)，y = heightFalloff(1/m)，z = baseHeight(cm)，w = anisotropy(g)
vec4 fogParams0;
// rgb = 散射反照率（线性），w = 散射强度缩放。
vec4 fogParams1;
// x = near(cm)，y = far(cm)，z = 深度层数，w = 1 表示本帧启用。
vec4 fogParams2;
// 保留：后续再加时间/风扰动时使用。
vec4 fogParams3;

#endif

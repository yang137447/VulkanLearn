#version 450

#include "common/commonUbo.glsl"

layout(binding = 1) uniform UBOMIParamters{
    vec4 tintColor;
} uboMIP;


layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 v2fColor;
layout(location = 1) out vec2 v2fTexCoord;
layout(location = 2) out vec3 v2fLightColor;

#include "common/lighting.glsl"

void main()
{
    gl_Position = uboVP.projection * uboVP.view * uboM.model * vec4(inPosition, 1.0);
    v2fColor = uboVP.ambient * uboMIP.tintColor.rgb;
    v2fTexCoord = inTexCoord; // 使用传入的纹理坐标
    v2fLightColor = PointLight(uboM.model, uboVP.pointLightPosition, uboVP.pointLightColor, inNormal, inPosition).rgb;
}
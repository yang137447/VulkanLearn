#version 450

#include "common/commonUbo.glsl"
#include "common/lighting.glsl"
#include "common/function.glsl"

layout(set = 1, binding = 0) uniform UBOMIParamters{
    vec4 u_tintColor;
};


layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 v2fPosition;
layout(location = 1) out vec3 v2fNormal;
layout(location = 2) out vec3 v2fColor;
layout(location = 3) out vec2 v2fTexCoord;

void main()
{
    //初始化变量
    mat4 modelMatrix = uboM.model;
    mat4 viewMatrix = uboVP.view;
    mat4 projectionMatrix = uboVP.projection;
        //MIP
    vec4 tintColor = u_tintColor;

    gl_Position = projectionMatrix * viewMatrix * modelMatrix * vec4(inPosition, 1.0);
    v2fColor = tintColor.rgb;
    v2fTexCoord = inTexCoord;
    v2fNormal = GetNormal_WS(modelMatrix, inNormal);
    v2fPosition = (modelMatrix * vec4(inPosition, 1.0)).xyz;
}

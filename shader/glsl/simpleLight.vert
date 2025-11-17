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
    //初始化变量
    mat4 modelMatrix = uboM.model;
    mat4 viewMatrix = uboVP.view;
    mat4 projectionMatrix = uboVP.projection;
    vec3 ambient = uboVP.ambient;
    vec3 cameraPosition = uboVP.cameraPosition;
    vec3 pointLightPosition = uboVP.pointLightPosition;
    vec4 pointLightColor = uboVP.pointLightColor;
    vec4 pointLightSpecular = uboVP.pointLightSpecular;
        //MIP
    vec4 tintColor = uboMIP.tintColor;


    gl_Position = projectionMatrix * viewMatrix * modelMatrix * vec4(inPosition, 1.0);
    v2fColor = tintColor.rgb;
    v2fTexCoord = inTexCoord; // 使用传入的纹理坐标
    v2fLightColor = PointLight(
        modelMatrix, 
        inNormal, 
        inPosition, 
        cameraPosition, 
        vec4(ambient, 1.0),
        pointLightPosition, 
        pointLightColor,
        pointLightSpecular).rgb;
}
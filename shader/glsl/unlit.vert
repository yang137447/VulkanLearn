#version 450

#include "common/commonUbo.glsl"

layout(set = 1, binding = 0) uniform UBOMIParamters{
    vec4 u_tintColor;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main()
{
    gl_Position = uboVP.projection * uboVP.view * uboM.model * vec4(inPosition, 1.0);
    fragColor = mix(inColor, u_tintColor.rgb, u_tintColor.a); // 使用传入的颜色
    fragTexCoord = inTexCoord; // 使用传入的纹理坐标
}

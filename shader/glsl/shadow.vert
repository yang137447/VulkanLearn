#version 450

#include "common/commonUbo.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 8) in vec4 inInstanceModel0;
layout(location = 9) in vec4 inInstanceModel1;
layout(location = 10) in vec4 inInstanceModel2;
layout(location = 11) in vec4 inInstanceModel3;

void main()
{
    mat4 instanceModel = mat4(
        inInstanceModel0, inInstanceModel1, inInstanceModel2, inInstanceModel3);
    gl_Position = uboVP.projection * uboVP.view * uboM.model *
        instanceModel * vec4(inPosition, 1.0);
}

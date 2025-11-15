layout(binding = 0) uniform UBOGlobal{
    mat4 view;
    mat4 projection;
    vec3 ambient;
    vec3 pointLightPosition;
    vec4 pointLightColor;
} uboVP;

layout(binding = 2) uniform UBOModel{
    mat4 model;
} uboM;
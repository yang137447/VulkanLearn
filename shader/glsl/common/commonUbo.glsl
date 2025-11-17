layout(binding = 0) uniform UBOGlobal{
    mat4 view;
    mat4 projection;
    vec3 ambient;
    vec3 cameraPosition;
    vec3 pointLightPosition;
    vec4 pointLightColor;
    vec4 pointLightSpecular;
} uboVP;

layout(binding = 2) uniform UBOModel{
    mat4 model;
} uboM;
layout(std140, binding = 0) uniform UBOGlobal{
    mat4 view;
    mat4 projection;
    vec3 ambient;
    vec3 cameraPosition;
} uboVP;

layout(std140, binding = 3) uniform UBOModel{
    mat4 model;
} uboM;
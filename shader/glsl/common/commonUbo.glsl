layout(std140, set = 0, binding = 0) uniform UBOGlobal{
    mat4 view;
    mat4 projection;
    mat4 lightViewProj;
    vec3 cameraPosition;
} uboVP;

layout(std140, set = 2, binding = 0) uniform UBOModel{
    mat4 model;
} uboM;

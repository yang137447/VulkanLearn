layout(std140, set = 0, binding = 0) uniform UBOGlobal{
    mat4 view;
    mat4 projection;
    mat4 invView;
    mat4 invProjection;
    mat4 viewProjection;
    mat4 invViewProjection;
    mat4 lightViewProj;
    vec3 cameraPosition;
    float pad0;
    vec4 environmentSH[9];
} uboVP;

layout(std140, set = 2, binding = 0) uniform UBOModel{
    mat4 model;
} uboM;

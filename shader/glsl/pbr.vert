#version 450

#include "common/commonUbo.glsl"
#include "common/function.glsl"

layout(set = 1, binding = 0) uniform UBOMIParamters{
    vec4 tintColor;
    vec4 pbrFactors;
} uboMIP;

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
    // 初始化当前物体的模型矩阵。PBR 顶点阶段只负责把几何数据整理到世界空间，
    // 真正的光照分类（Direct / Indirect）留到片元阶段处理。
    mat4 modelMatrix = uboM.model;

    // 标准 MVP 变换，输出裁剪空间位置。
    gl_Position = uboVP.projection * uboVP.view * modelMatrix * vec4(inPosition, 1.0);

    // tintColor 当前先作为 baseColor 的额外乘子使用，方便不改贴图时快速调材质外观。
    v2fColor = uboMIP.tintColor.rgb;
    v2fTexCoord = inTexCoord;

    // 法线与位置都转换到世界空间，后续直接光和 IBL 都统一在世界空间中计算。
    v2fNormal = GetNormal_WS(modelMatrix, inNormal, inPosition);
    v2fPosition = (modelMatrix * vec4(inPosition, 1.0)).xyz;
}

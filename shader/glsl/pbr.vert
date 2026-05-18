#version 450

#include "common/commonUbo.glsl"
#include "common/function.glsl"

layout(set = 1, binding = 0) uniform UBOMIParamters{
    vec4 u_tintColor;
    vec4 u_pbrFactors;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 v2fPosition;
layout(location = 1) out vec3 v2fNormal;
layout(location = 2) out vec3 v2fColor;
layout(location = 3) out vec2 v2fTexCoord;
layout(location = 4) out vec4 v2fTangent;

void main()
{
    // 初始化当前物体的模型矩阵。PBR 顶点阶段只负责把几何数据整理到世界空间，
    // 真正的光照分类（Direct / Indirect）留到片元阶段处理。
    mat4 modelMatrix = uboM.model;

    // 标准 MVP 变换，输出裁剪空间位置。
    gl_Position = uboVP.projection * uboVP.view * modelMatrix * vec4(inPosition, 1.0);

    // tintColor 当前先作为 baseColor 的额外乘子使用，方便不改贴图时快速调材质外观。
    v2fColor = u_tintColor.rgb;
    v2fTexCoord = inTexCoord;

    // MikkTSpace 要求运行时解码尽量匹配烘焙端使用的切线空间基底。
    // 如果这里先把 T/B/N 单独 normalize，或提前做重新正交化，
    // 就会改变后续片元插值得到的基底，导致法线贴图解码结果与烘焙端不完全一致，
    // 在高光方向、UV 接缝和镜像区域更容易出现偏差。
    // 因此这里保留未归一化的世界空间 T/B/N，让片元阶段直接使用插值后的结果解码。
    v2fNormal = GetNormal_WS_Unnormalized(modelMatrix, inNormal);
    v2fTangent = vec4(GetDirection_WS_Unnormalized(modelMatrix, inTangent.xyz), inTangent.w);
    v2fPosition = (modelMatrix * vec4(inPosition, 1.0)).xyz;
}

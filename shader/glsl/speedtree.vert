#version 450

#include "common/commonUbo.glsl"
#include "common/function.glsl"
#include "engine/materialContext.glsl"
#include "generate/M_speedtreeParamter.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out MaterialVaryings v2f;

// MaterialVertex 是顶点阶段的公开材质入口：一般材质作者改这里。
// main 负责把引擎输入/输出接到 context 上，后续模板化时也应继续保持为底层封装。
void MaterialVertex(inout MaterialVertexContext vertex)
{
    // SpeedTree 第一版先复制 PBR 顶点路径，后续在这里逐步接入风和多 pivot 数据。
    mat4 modelMatrix = vertex.modelMatrix;
    vec3 localPosition = vertex.vertexInput.localPosition;

    // 标准 MVP 变换，输出裁剪空间位置。
    vertex.vertexOutput.clipPosition = uboVP.projection * uboVP.view * modelMatrix * vec4(localPosition, 1.0);
    // velocity 需要同一顶点的上一帧裁剪空间位置：上一帧 VP 来自全局 UBO，上一帧 model 来自对象 UBO。
    vertex.vertexOutput.previousClipPosition = uboVP.previousViewProjection * uboM.previousModel * vec4(localPosition, 1.0);

    // SpeedTree 顶点色 w 由导入器映射为 AO，片元阶段会按材质语义解释。
    vertex.vertexOutput.vertexColor = vertex.vertexInput.vertexColor;
    vertex.vertexOutput.texCoord = vertex.vertexInput.texCoord;

    // MikkTSpace 要求运行时解码尽量匹配烘焙端使用的切线空间基底。
    // 如果这里先把 T/B/N 单独 normalize，或提前做重新正交化，
    // 就会改变后续片元插值得到的基底，导致法线贴图解码结果与烘焙端不完全一致。
    // 因此这里保留未归一化的世界空间 T/B/N，让片元阶段直接使用插值后的结果解码。
    vertex.vertexOutput.worldNormal = GetNormal_WS_Unnormalized(modelMatrix, vertex.vertexInput.localNormal);
    vertex.vertexOutput.worldTangent = vec4(
        GetDirection_WS_Unnormalized(modelMatrix, vertex.vertexInput.localTangent.xyz),
        vertex.vertexInput.localTangent.w);
    vertex.vertexOutput.worldPosition = (modelMatrix * vec4(localPosition, 1.0)).xyz;
}

void main()
{
    MaterialVertexContext vertex;
    vertex.modelMatrix = uboM.model;
    vertex.vertexInput.localPosition = inPosition;
    vertex.vertexInput.localNormal = inNormal;
    vertex.vertexInput.vertexColor = inColor;
    vertex.vertexInput.texCoord = inTexCoord;
    vertex.vertexInput.localTangent = inTangent;

    MaterialVertex(vertex);

    gl_Position = vertex.vertexOutput.clipPosition;
    v2f = CreateMaterialVaryings(vertex.vertexOutput);
}

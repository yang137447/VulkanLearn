#ifndef VL_PASS_TEMPLATE_BASE_VERT_GLSL
#define VL_PASS_TEMPLATE_BASE_VERT_GLSL

#include "../../common/commonUbo.glsl"
#include "../../common/function.glsl"
#include "../materialContext.glsl"
#include "../skinnedVertexFactory.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;
// location 7 保持可选 UV1 的稳定顶点 ABI；无 UV1 的网格由导入端提供零值。
layout(location = 7) in vec2 inTexCoord1;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in vec4 inSpeedTreeWindBranch1;
layout(location = 6) in vec4 inSpeedTreeWindBranch2;

layout(location = 0) out MaterialVaryings v2f;

void main()
{
    MaterialVertexInput vertexInput;
    vertexInput.localPosition = inPosition;
    vertexInput.localNormal = inNormal;
    vertexInput.vertexColor = inColor;
    vertexInput.texCoord = inTexCoord;
    vertexInput.texCoord1 = inTexCoord1;
    vertexInput.localTangent = inTangent;
    vertexInput.speedTreeWindBranch1 = inSpeedTreeWindBranch1;
    vertexInput.speedTreeWindBranch2 = inSpeedTreeWindBranch2;

    vec3 previousLocalPosition = vertexInput.localPosition;
#if defined(VL_SKINNED_MESH) && VL_SKINNED_MESH
    previousLocalPosition = ApplySkinnedVertexFactory(vertexInput);
#endif
    MaterialVertex vertex = EvaluateMaterialVertex(vertexInput);
    mat4 modelMatrix = uboM.model;

    MaterialVertexOutput outputVertex;
    outputVertex.clipPosition =
        uboVP.projection * uboVP.view * modelMatrix * vec4(vertex.localPosition, 1.0);
    // TODO: 保存并重放上一帧 SpeedTree 风动状态后，再用上一帧形变位置计算运动矢量。
    // 当前 previousModel 只能保证相机/物体变换的历史，无法表达纯风动造成的叶片位移。
    outputVertex.previousClipPosition =
        uboVP.previousViewProjection * uboM.previousModel * vec4(vertex.localPosition, 1.0);
#if defined(VL_SKINNED_MESH) && VL_SKINNED_MESH
    // previous palette 来自上一成功提交；seek/换片段时由原生边界重置历史。
    outputVertex.previousClipPosition =
        uboVP.previousViewProjection * uboM.previousModel * vec4(previousLocalPosition, 1.0);
#endif
    outputVertex.worldPosition =
        (modelMatrix * vec4(vertex.localPosition, 1.0)).xyz;
    // 在插值前去除模型缩放的长度因子；否则 scale=100 时 N 长度变为 0.01，
    // T 长度变为 100，MikkTSpace 片元解码会被错误的相对长度扭曲。
    outputVertex.worldNormal =
        GetNormal_WS(modelMatrix, vertex.localNormal);
    outputVertex.vertexColor = vertex.vertexColor;
    outputVertex.texCoord = vertex.texCoord;
    outputVertex.texCoord1 = vertex.texCoord1;
    outputVertex.worldTangent = vec4(
        GetDirection_WS(modelMatrix, vertex.localTangent.xyz),
        vertex.localTangent.w);

    gl_Position = outputVertex.clipPosition;
    v2f = CreateMaterialVaryings(outputVertex);
}

#endif

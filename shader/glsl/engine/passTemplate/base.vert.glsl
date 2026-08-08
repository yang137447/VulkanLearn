#ifndef VL_PASS_TEMPLATE_BASE_VERT_GLSL
#define VL_PASS_TEMPLATE_BASE_VERT_GLSL

#include "../../common/commonUbo.glsl"
#include "../../common/function.glsl"
#include "../materialContext.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inColor;
layout(location = 3) in vec2 inTexCoord;
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
    vertexInput.localTangent = inTangent;
    vertexInput.speedTreeWindBranch1 = inSpeedTreeWindBranch1;
    vertexInput.speedTreeWindBranch2 = inSpeedTreeWindBranch2;

    MaterialVertex vertex = EvaluateMaterialVertex(vertexInput);
    mat4 modelMatrix = uboM.model;

    MaterialVertexOutput outputVertex;
    outputVertex.clipPosition =
        uboVP.projection * uboVP.view * modelMatrix * vec4(vertex.localPosition, 1.0);
    // TODO: 保存并重放上一帧 SpeedTree 风动状态后，再用上一帧形变位置计算运动矢量。
    // 当前 previousModel 只能保证相机/物体变换的历史，无法表达纯风动造成的叶片位移。
    outputVertex.previousClipPosition =
        uboVP.previousViewProjection * uboM.previousModel * vec4(vertex.localPosition, 1.0);
    outputVertex.worldPosition =
        (modelMatrix * vec4(vertex.localPosition, 1.0)).xyz;
    outputVertex.worldNormal =
        GetNormal_WS_Unnormalized(modelMatrix, vertex.localNormal);
    outputVertex.vertexColor = vertex.vertexColor;
    outputVertex.texCoord = vertex.texCoord;
    outputVertex.worldTangent = vec4(
        GetDirection_WS_Unnormalized(modelMatrix, vertex.localTangent.xyz),
        vertex.localTangent.w);

    gl_Position = outputVertex.clipPosition;
    v2f = CreateMaterialVaryings(outputVertex);
}

#endif

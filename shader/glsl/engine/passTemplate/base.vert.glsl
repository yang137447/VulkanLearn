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

layout(location = 0) out MaterialVaryings v2f;

void main()
{
    MaterialVertexInput vertexInput;
    vertexInput.localPosition = inPosition;
    vertexInput.localNormal = inNormal;
    vertexInput.vertexColor = inColor;
    vertexInput.texCoord = inTexCoord;
    vertexInput.localTangent = inTangent;

    MaterialVertex vertex = EvaluateMaterialVertex(vertexInput);
    mat4 modelMatrix = uboM.model;

    MaterialVertexOutput outputVertex;
    outputVertex.clipPosition =
        uboVP.projection * uboVP.view * modelMatrix * vec4(vertex.localPosition, 1.0);
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

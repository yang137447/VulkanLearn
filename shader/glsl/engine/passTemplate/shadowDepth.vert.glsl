#ifndef VL_PASS_TEMPLATE_SHADOW_DEPTH_VERT_GLSL
#define VL_PASS_TEMPLATE_SHADOW_DEPTH_VERT_GLSL

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

#if MATERIAL_USES_OPACITY_MASK
layout(location = 0) out MaterialVaryings v2f;
#endif

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
    vec4 worldPosition = modelMatrix * vec4(vertex.localPosition, 1.0);
    gl_Position = uboVP.projection * uboVP.view * worldPosition;

#if MATERIAL_USES_OPACITY_MASK
    MaterialVertexOutput outputVertex;
    outputVertex.clipPosition = gl_Position;
    outputVertex.previousClipPosition = gl_Position;
    outputVertex.worldPosition = worldPosition.xyz;
    outputVertex.worldNormal =
        GetNormal_WS_Unnormalized(modelMatrix, vertex.localNormal);
    outputVertex.vertexColor = vertex.vertexColor;
    outputVertex.texCoord = vertex.texCoord;
    outputVertex.worldTangent = vec4(
        GetDirection_WS_Unnormalized(modelMatrix, vertex.localTangent.xyz),
        vertex.localTangent.w);
    v2f = CreateMaterialVaryings(outputVertex);
#endif
}

#endif

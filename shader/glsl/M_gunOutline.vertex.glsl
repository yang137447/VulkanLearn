#ifndef VL_M_GUNOUTLINE_VERTEX_GLSL
#define VL_M_GUNOUTLINE_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"
#include "common/commonUbo.glsl"

// 屏幕空间等宽描边（倒置壳）+ 自适应深度偏置。
// 宽度：抓帧的 mix(4.0, 0.2, saturate((1200-d)/1200)) 展开后是 0.2 + 0.00317*d，
//       本质是"常数屏幕宽度"，这里直接写成精确式：
//       width = fraction * 2 * tanHalfFovY * d
//       fraction = 半宽 / 视口高度（与分辨率无关），tanHalfFovY 由投影矩阵给出
//       （perspective 的 P[1][1] = 1/tanHalfFovY），故任意 FOV 都成立。
// 偏置：抓帧用固定 3cm，但壳宽在 12m 处会到 4cm，超过偏置后壳会盖住本体；
//       而且本资产是"板状"几何，背面投影落在剪影内，cull Front 反而完全看不到轮廓。
//       因此沿用抓帧的 cull None + gl_FrontFacing 方案，把偏置改成随宽度自适应：
//       bias = 2 * width + 0.5cm，保证任何距离下壳都退到本体之后。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex = CreateDefaultMaterialVertex(vertexInput);
    vec3 worldPosition = (uboM.model * vec4(vertexInput.localPosition, 1.0)).xyz;
    vec3 viewVector = worldPosition - uboVP.cameraPosition;
    float distanceToCamera = length(viewVector);
    float tanHalfFovY = 1.0 / max(abs(uboVP.projection[1][1]), 1.0e-6);
    float width = u_outlineScreenFraction * 2.0 * tanHalfFovY * distanceToCamera * u_widthScale;
    vec3 localNormal = normalize(vertexInput.localNormal);
    vec3 localViewDirection = normalize(transpose(mat3(uboM.model)) * normalize(viewVector));
    vertex.localPosition += localNormal * width;
    vertex.localPosition += localViewDirection * (2.0 * width + 0.5);
    return vertex;
}

#endif

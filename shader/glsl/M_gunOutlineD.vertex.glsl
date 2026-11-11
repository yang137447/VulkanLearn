#ifndef VL_M_GUNOUTLINE_D_VERTEX_GLSL
#define VL_M_GUNOUTLINE_D_VERTEX_GLSL

#include "materialFunction/mf_defaultVertex.glsl"
#include "common/commonUbo.glsl"

// D: 投影后常量像素膨胀。先把法线压到视平面，再把该方向投到 NDC，
// 按屏幕像素宽度偏移 clip.xy，最后把该偏移反算回 localPosition。
// 这样不依赖物体空间宽度曲线，也不受模型缩放影响；C 的骨料/法线仍完全相同。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex = CreateDefaultMaterialVertex(vertexInput);
    mat4 model = uboM.model;
    mat3 modelInverse = inverse(mat3(model));
    vec3 localNormal = normalize(vertexInput.localNormal);
    vec4 worldPosition4 = model * vec4(vertexInput.localPosition, 1.0);
    vec3 worldPosition = worldPosition4.xyz;
    vec3 viewVector = worldPosition - uboVP.cameraPosition;
    float distanceToCamera = length(viewVector);
    float tanHalfFovY = 1.0 / max(abs(uboVP.projection[1][1]), 1.0e-6);
    float widthWorld =
        u_outlineScreenFraction * 2.0 * tanHalfFovY * distanceToCamera * u_widthScale;
    vec3 localViewDirection =
        normalize(transpose(mat3(model)) * normalize(viewVector));

    vec4 viewPosition = uboVP.view * worldPosition4;
    vec4 clipPosition = uboVP.projection * viewPosition;
    vec3 worldNormal = normalize(transpose(modelInverse) * localNormal);
    vec3 viewNormal = normalize(mat3(uboVP.view) * worldNormal);
    vec3 viewDirection = normalize(viewPosition.xyz);
    // 去掉朝向相机的分量，得到真正落在视平面上的膨胀方向。
    vec3 planeNormal = viewNormal - viewDirection * dot(viewNormal, viewDirection);
    float planeLength = length(planeNormal);

    if (clipPosition.w > 1.0e-4 && planeLength > 1.0e-4)
    {
        planeNormal /= planeLength;
        vec4 normalClip = uboVP.projection * vec4(planeNormal, 0.0);
        float aspect = abs(uboVP.projection[1][1] /
            max(abs(uboVP.projection[0][0]), 1.0e-6));
        // u_outlineScreenFraction 是半宽 / 视口高度；像素方向按宽高比归一化。
        vec2 pixelDirection = normalize(vec2(normalClip.x * aspect, normalClip.y));
        vec2 ndcOffset = pixelDirection *
            vec2(2.0 * u_outlineScreenFraction / aspect,
                 2.0 * u_outlineScreenFraction) * u_widthScale;
        vec4 clipOffset = vec4(ndcOffset * clipPosition.w, 0.0, 0.0);
        vec3 viewOffset = (uboVP.invProjection * clipOffset).xyz;
        vec3 worldOffset = mat3(uboVP.invView) * viewOffset;
        vertex.localPosition += modelInverse * worldOffset;
    }
    else
    {
        // 正对相机的退化方向由本体 mask 覆盖；给一个稳定的物体空间回退，
        // 避免投影法线长度接近 0 时壳直接塌回原位。
        vertex.localPosition += localNormal * widthWorld;
    }
    vertex.localPosition += localViewDirection * u_viewBias;
    return vertex;
}

#endif

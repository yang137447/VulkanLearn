#include "materialFunction/mf_defaultVertex.glsl"

MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex = EvaluateMFDefaultVertex(vertexInput);
    // 源 MF_ObjectAlignedTexture 在顶点阶段传递 PreSkinnedNormal。
    // 本材质不使用作者顶点色，借其已有插值槽保留 UE 局部法线，
    // 避免从非均匀缩放后的世界法线逆推，也不扩张所有材质的 varying ABI。
    vertex.vertexColor = vec4(vertexInput.localNormal.xzy, 1.0);
    return vertex;
}

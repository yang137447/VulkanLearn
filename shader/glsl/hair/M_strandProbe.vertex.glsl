#ifndef VL_STRAND_PROBE_VERTEX_GLSL
#define VL_STRAND_PROBE_VERTEX_GLSL
// 发丝展开由引擎 VertexFactory 完成，材质入口只传递结果。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput inputVertex)
{
    return CreateDefaultMaterialVertex(inputVertex);
}
#endif

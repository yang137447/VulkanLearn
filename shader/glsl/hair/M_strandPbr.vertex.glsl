#ifndef VL_STRAND_PBR_VERTEX_GLSL
#define VL_STRAND_PBR_VERTEX_GLSL
// 中心线变形与 ribbon 展开属于引擎工厂；材质不重复移动发根。
MaterialVertex EvaluateMaterialVertex(in MaterialVertexInput inputVertex)
{
    return CreateDefaultMaterialVertex(inputVertex);
}
#endif

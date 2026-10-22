#ifndef VL_TRESSFX_VERTEX_FACTORY_GLSL
#define VL_TRESSFX_VERTEX_FACTORY_GLSL
// Copyright (c) 2016-2025 Advanced Micro Devices, Inc.
// 派生算法的 MIT 许可见 source/hair/THIRD_PARTY_NOTICES.md。

#if defined(VL_TRESSFX_STRANDS) && VL_TRESSFX_STRANDS
// 参考 TressFX5-5.4.4 TressFXVertexFactory.ush 的 ComputeViewAlignedWorldPosition。
// 保留统一 ViewForward 展开语义，不用每点的透视射线；VL 从局部中心线变换到世界，
// 不照搬 UE 已预变换的 PositionBuffer，也不交换“法线=发丝切线”的 UE 材质约定。
void ApplyTressfxVertexFactory(inout MaterialVertexInput inputVertex)
{
    mat3 localToWorld = mat3(uboM.model);
    mat3 worldToLocal = inverse(localToWorld);
    vec3 worldTangent = normalize(localToWorld * inputVertex.localTangent.xyz);
    vec3 viewDirection = normalize(uboVP.invView[2].xyz);
    vec3 right = cross(worldTangent, viewDirection);
    // 视线平行切线是相机产生的几何奇点，不能在资产导入时排除；
    // 此时选择相机横轴投影，防止 normalize(0) 污染整根 ribbon。
    if (dot(right, right) < 1e-8)
        right = uboVP.invView[0].xyz -
            worldTangent * dot(uboVP.invView[0].xyz, worldTangent);
    vec3 localRight = normalize(worldToLocal * normalize(right));
    inputVertex.localPosition += localRight * inputVertex.texCoord1.x;
    inputVertex.localNormal = normalize(cross(localRight, inputVertex.localTangent.xyz));
    // location 7 的导入数据已消费；材质看到横向 0..1、纵向根→梢 0..1。
    inputVertex.texCoord1.x = (gl_VertexIndex & 1) == 0 ? 0.0 : 1.0;
    // 源实现用 max(物理半径, 像素最小半径) 避免亚像素闪烁；该策略须与覆盖率配套。
    // 本批保持作者半径，不单独放大发丝后误称具备抗锯齿/能量补偿。
}
#endif
#endif

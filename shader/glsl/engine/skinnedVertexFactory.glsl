#ifndef VL_SKINNED_VERTEX_FACTORY_GLSL
#define VL_SKINNED_VERTEX_FACTORY_GLSL

#if defined(VL_SKINNED_MESH) && VL_SKINNED_MESH
#include "skinnedVertexMath.glsl"

#if defined(VL_SKINNED_PRERENDERED) && VL_SKINNED_PRERENDERED
// 逐帧 compute 已经把 current/previous 蒙皮结果写进对象 Set 2 binding 4；
// 顶点阶段只做一次按顶点索引的读取，不再遍历 palette/influence。
// 结构体与 skinnedDeform.comp 的输出结构逐字段一致（64 B/顶点）。
struct SkinDeformedVertex { vec4 position; vec4 previousPosition; vec4 normal; vec4 tangent; };
layout(std430, set = 2, binding = 4) readonly buffer SkinDeformedVertices
{
    SkinDeformedVertex vertices[];
} skinDeformed;

vec3 ApplySkinnedVertexFactory(inout MaterialVertexInput inputVertex)
{
    SkinDeformedVertex deformed = skinDeformed.vertices[gl_VertexIndex];
    inputVertex.localPosition = deformed.position.xyz;
    inputVertex.localNormal = deformed.normal.xyz;
    inputVertex.localTangent = deformed.tangent;
    return deformed.previousPosition.xyz;
}
#else
vec3 ApplySkinnedVertexFactory(inout MaterialVertexInput inputVertex)
{
    vec3 position;
    vec3 normal;
    vec4 tangent;
    vec3 previousPosition;
    SkinVertex(uint(gl_VertexIndex), inputVertex.localPosition, inputVertex.localNormal,
        inputVertex.localTangent, position, normal, tangent, previousPosition);
    inputVertex.localPosition = position;
    inputVertex.localNormal = normal;
    inputVertex.localTangent = tangent;
    return previousPosition;
}
#endif
#endif
#endif

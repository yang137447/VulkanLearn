#pragma once

#include <cstddef>
#include <cstdint>

#include "VertexDataStruct.h"

namespace VL::Skinning
{

// pre-skin buffer 的顶点 ABI 与 shader/glsl/engine/skinnedDeform.comp 的输出、
// skinnedVertexFactory.glsl 的读取端一一对应；任何一侧改动都必须同步。
struct SkinDeformVertexLayout
{
    static constexpr uint32_t PositionOffsetBytes = 0;
    static constexpr uint32_t PreviousPositionOffsetBytes = 16;
    static constexpr uint32_t NormalOffsetBytes = 32;
    static constexpr uint32_t TangentOffsetBytes = 48;
    static constexpr uint32_t StrideBytes = 64;
};

static_assert(sizeof(float) == 4);
// compute 把顶点缓冲当作紧排 float 流读取（28 float / 顶点）。这组偏移是
// shader 常量 PositionOffsetFloats/NormalOffsetFloats/TangentOffsetFloats 的来源。
static_assert(offsetof(Vertex, position) == 0);
static_assert(offsetof(Vertex, normal) == 12);
// Eigen::Vector4f 的 16 字节对齐让 tangent 落在 64，而不是 56；整个顶点为 112 B。
static_assert(offsetof(Vertex, tangent) == 64);
static_assert(sizeof(Vertex) == 112);
static_assert(sizeof(Vertex) % sizeof(float) == 0);
static_assert(offsetof(Vertex, position) / sizeof(float) == 0);
static_assert(offsetof(Vertex, normal) / sizeof(float) == 3);
static_assert(offsetof(Vertex, tangent) / sizeof(float) == 16);
static_assert(sizeof(Vertex) / sizeof(float) == 28);

// 与 shader 的 local_size_x 必须一致。
inline constexpr uint32_t SkinDeformWorkgroupSize = 64;

inline constexpr uint32_t SkinDeformGroupCount(uint32_t vertexCount)
{
    return (vertexCount + SkinDeformWorkgroupSize - 1) / SkinDeformWorkgroupSize;
}

// 每个蒙皮对象一份输出 buffer（跨帧复用）；返回该 buffer 的字节数。
inline constexpr uint64_t SkinDeformBufferSizeBytes(uint32_t vertexCount)
{
    return static_cast<uint64_t>(vertexCount) * SkinDeformVertexLayout::StrideBytes;
}

} // namespace VL::Skinning

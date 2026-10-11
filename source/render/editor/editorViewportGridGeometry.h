#pragma once

#include <cstddef>
#include <vector>

#include <Eigen/Dense>

#include "vertexDataStruct.h"

namespace VL
{
// 编辑器参考网格的纯几何生成：没有 Vulkan 依赖，因此可以在不创建缓冲、
// 不初始化设备的情况下做模块测试。缓冲所有权在 editorViewportGrid
// （"网格长什么样"），pass 状态在 renderGraphConfig 的 editorGrid pass（"怎么被画出来"）。
//
// 顶点字段是共享的 Vertex：position 与 color 有值，normal 只给一个稳定值以满足
// 共享顶点属性布局，不表达光照信息。

// 供几何生成与模块测试引用的网格口径，改动它们会改变网格外观与几何量。
// 单位间距 1 世界单位、每 10 条线加重，与 UE 编辑器视口的默认参考网格一致。
inline constexpr int kEditorGridHalfExtent = 24;
inline constexpr int kEditorGridMajorLineStep = 10;
inline constexpr float kEditorGridSpacing = 1.0f;
inline constexpr float kEditorGridLineHalfWidth = 0.012f;
inline constexpr float kEditorGridMajorLineHalfWidth = 0.02f;
inline constexpr float kEditorGridAxisHalfWidth = 0.03f;
inline constexpr float kEditorGridAxisHeight = 4.0f;
// 地面几何与网格共面时深度值可能完全相等，抬升一个远小于单位间距的量，
// 让 lessOrEqual 的遮挡结果稳定，而不是在共面像素上随机归属。
inline constexpr float kEditorGridGroundLift = 0.002f;
// 管线拓扑固定为 TriangleList，每条线用四边形条带的两个三角形表达。
inline constexpr int kEditorGridVerticesPerQuad = 6;
// 竖直 Y 轴：两片互相垂直的条带，每片正反各出一份绕序（材质必须保持 cullMode Back，
// 否则 TwoSided 会被要求生成 ShadowDepth 组合）。
inline constexpr int kEditorGridVerticalAxisQuadCount = 4;

// 颜色是 tone mapping 之前的线性值，全部低于 bloom 阈值 1.0，
// 因此网格只表达参考系，不会让后处理把线当成高光。
inline const Eigen::Vector4f kEditorGridMinorLineColor(0.30f, 0.30f, 0.33f, 1.0f);
inline const Eigen::Vector4f kEditorGridMajorLineColor(0.56f, 0.56f, 0.60f, 1.0f);
inline const Eigen::Vector4f kEditorGridAxisXColor(0.72f, 0.10f, 0.10f, 1.0f);
inline const Eigen::Vector4f kEditorGridAxisYColor(0.14f, 0.62f, 0.16f, 1.0f);
inline const Eigen::Vector4f kEditorGridAxisZColor(0.16f, 0.24f, 0.82f, 1.0f);

// 地面线数：两个线族各 2 * kEditorGridHalfExtent + 1 条，其中 offset == 0 那条是轴线。
inline constexpr size_t EditorGridGroundLineCount()
{
    return static_cast<size_t>(2 * (2 * kEditorGridHalfExtent + 1));
}

// 生成器实际产出的顶点数：几何生成用它预留容量，模块测试用它校验实际产出。
inline constexpr size_t EditorGridExpectedVertexCount()
{
    return (EditorGridGroundLineCount() +
        static_cast<size_t>(kEditorGridVerticalAxisQuadCount)) *
        static_cast<size_t>(kEditorGridVerticesPerQuad);
}

// 生成一次要提交的全部顶点：XZ 地面网格条带 + 原点三轴 + 竖直 Y 轴。
// 静态世界几何，调用一次即可；纯 CPU 计算，可被模块测试直接编译。
std::vector<Vertex> BuildEditorGridVertices();

} // namespace VL

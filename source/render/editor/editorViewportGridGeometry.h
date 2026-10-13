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
// 世界单位为 cm；采用十进制参考尺度：小格 10 cm、主格 1 m、总宽 4.8 m。
// UE 网格间距可配置，这里不是复刻其视距自适应网格；不能把 1 cm 误当作 1 m。
inline constexpr int kEditorGridHalfExtent = 24;
inline constexpr int kEditorGridMajorLineStep = 10;
inline constexpr float kEditorGridSpacing = 10.0f;
inline constexpr float kEditorGridLineHalfWidth = 0.12f;
inline constexpr float kEditorGridMajorLineHalfWidth = 0.2f;
inline constexpr float kEditorGridAxisHalfWidth = 0.3f;
inline constexpr float kEditorGridAxisHeight = 40.0f;
// 地面几何与网格共面时深度值可能完全相等，抬升一个远小于单位间距的量，
// 让 lessOrEqual 的遮挡结果稳定，而不是在共面像素上随机归属。
inline constexpr float kEditorGridGroundLift = 0.02f;
// 管线拓扑固定为 TriangleList，每条线用四边形条带的两个三角形表达。
inline constexpr int kEditorGridVerticesPerQuad = 6;
// 竖直 Y 轴是**两片**互相垂直的条带（Z 平面一片、X 平面一片）。这里记的是条带数，
// 不是 quad 数：每片条带的正反两面各由一个 quad 表达，铺满两个 quad。
// 之所以要正反两面，是因为材质是 cullMode Back，只出一面时某些相机方位下整片会消失。
// 早先这个常量按 quad 数记（4），那是在 PushQuad 只出一份绕序的前提下成立的；
// PushQuad 改成两面都出之后它必须回到"条带数"，否则顶点数口径会差一倍。
inline constexpr int kEditorGridVerticalAxisStripCount = 2;

// 地面每条线也出正反两份绕序，与 Y 轴同一策略。
//
// 原因是一次实测：材质 `cullMode: Back` 下，地面条带在编辑器默认的俯视视角里全是背面，
// **整族被剔除**，屏幕上只剩 Y 轴那一条线 —— 逐像素比对显示"开/关网格 pass"只差 66 px。
// 把 cullMode 临时改成 Front 后同一测量变成 40343 px（600 倍），确认就是剔除而非颜色、
// 遮挡或几何缺失。与其依赖"当前视角朝上"这种会随相机改变的假设，不如两面都出：
// 可见性由深度测试决定，不再由相机方向决定。
inline constexpr int kEditorGridGroundWindings = 2;

// 颜色是 tone mapping 之前的线性值，全部低于 bloom 阈值 1.0，
// 因此网格只表达参考系，不会让后处理把线当成高光。
//
// 10 cm 次线用中性灰弱化，1 m 主线保持高亮；不要同时压暗主线与坐标轴，
// 否则在有纹理的地面上会丢失尺度参照。这里直接填写线性颜色，不再做 sRGB 解码。
inline const Eigen::Vector4f kEditorGridMinorLineColor(0.25f, 0.25f, 0.25f, 1.0f);
inline const Eigen::Vector4f kEditorGridMajorLineColor(0.78f, 0.80f, 0.86f, 1.0f);
inline const Eigen::Vector4f kEditorGridAxisXColor(0.95f, 0.16f, 0.12f, 1.0f);
inline const Eigen::Vector4f kEditorGridAxisYColor(0.20f, 0.88f, 0.24f, 1.0f);
inline const Eigen::Vector4f kEditorGridAxisZColor(0.22f, 0.36f, 0.98f, 1.0f);

// 地面线数：两个线族各 2 * kEditorGridHalfExtent + 1 条，其中 offset == 0 那条是轴线。
inline constexpr size_t EditorGridGroundLineCount()
{
    return static_cast<size_t>(2 * (2 * kEditorGridHalfExtent + 1));
}

// 生成器实际产出的口径。2026-10-12 用探针**实测**（推导在这件事上错过数次）：
//   groundLines = 98、Y 轴条带 = 2、kEditorGridVerticesPerQuad = 6
//   实际产出    = 1224 顶点；一个逻辑四边形占 2 * 6 = 12 个顶点（PushQuad 出正反两份绕序）
//   -> 1224 / 12 = 102 个逻辑四边形
// 102 的分解：地面 98 条线各一个四边形；Y 轴 2 片条带，每片正反两个四边形（共 4 个）。
//
// 注意"绕序"这一层已经体现在 12（而不是 6）这个步长里，所以下面的四边形计数**不再**乘
// kEditorGridGroundWindings —— 早先多乘了一次，导致 204 对 102 的偏差。
inline constexpr size_t EditorGridGroundLogicalQuadCount()
{
    return EditorGridGroundLineCount();
}

inline constexpr size_t EditorGridVerticalAxisLogicalQuadCount()
{
    // 每片条带正反两个面，各是一个逻辑四边形。
    return static_cast<size_t>(kEditorGridVerticalAxisStripCount) *
        static_cast<size_t>(kEditorGridGroundWindings);
}

inline constexpr size_t EditorGridVertexQuadCount()
{
    return EditorGridGroundLogicalQuadCount() + EditorGridVerticalAxisLogicalQuadCount();
}

inline constexpr size_t EditorGridExpectedVertexCount()
{
    return EditorGridVertexQuadCount() *
        static_cast<size_t>(kEditorGridGroundWindings) *
        static_cast<size_t>(kEditorGridVerticesPerQuad);
}

// 生成一次要提交的全部顶点：XZ 地面网格条带 + 原点三轴 + 竖直 Y 轴。
// 静态世界几何，调用一次即可；纯 CPU 计算，可被模块测试直接编译。
std::vector<Vertex> BuildEditorGridVertices();

} // namespace VL

#include "render/editor/editorViewportGridGeometry.h"

namespace VL
{
namespace
{

Vertex MakeVertex(const Eigen::Vector3f& position, const Eigen::Vector4f& color)
{
    Vertex vertex;
    vertex.position = position;
    // 网格不参与光照，法线只为满足共享顶点属性布局而给一个稳定值。
    vertex.normal = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    vertex.color = color;
    vertex.texCoord = Eigen::Vector2f::Zero();
    return vertex;
}

void PushQuad(
    std::vector<Vertex>& vertices,
    const Vertex& a,
    const Vertex& b,
    const Vertex& c,
    const Vertex& d)
{
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
    vertices.push_back(a);
    vertices.push_back(c);
    vertices.push_back(d);
}

// 地面网格线用矩形条带表达，而不是 GL 线段：线宽是真实世界单位，
// 因此缩放和距离变化时不会退化成一像素，也不需要依赖 lineWidth 特性。
// 两个线族的绕序必须互为镜像：投影带 Y 翻转、前端面按 Clockwise 判定，
// 同向绕序会让其中一族从地面以上的视角被整族剔除。
void AppendGroundLine(
    std::vector<Vertex>& vertices,
    bool alongX,
    float offset,
    float halfWidth,
    const Eigen::Vector4f& color)
{
    const float extent = kEditorGridHalfExtent * kEditorGridSpacing;
    const float y = kEditorGridGroundLift;
    if (alongX)
    {
        PushQuad(
            vertices,
            MakeVertex(Eigen::Vector3f(-extent, y, offset + halfWidth), color),
            MakeVertex(Eigen::Vector3f(extent, y, offset + halfWidth), color),
            MakeVertex(Eigen::Vector3f(extent, y, offset - halfWidth), color),
            MakeVertex(Eigen::Vector3f(-extent, y, offset - halfWidth), color));
        return;
    }

    PushQuad(
        vertices,
        MakeVertex(Eigen::Vector3f(offset - halfWidth, y, -extent), color),
        MakeVertex(Eigen::Vector3f(offset - halfWidth, y, extent), color),
        MakeVertex(Eigen::Vector3f(offset + halfWidth, y, extent), color),
        MakeVertex(Eigen::Vector3f(offset + halfWidth, y, -extent), color));
}

// 竖直的 Y 轴没有专属朝向：用两片互相垂直的条带，使它在任意水平视角下
// 都有可见面积。每片再出一份反向绕序，因为剔除无法关闭（TwoSided 材质会被
// 要求生成 ShadowDepth 组合）：否则相机落在另一象限时这条竖轴会整片消失。
void AppendVerticalAxis(std::vector<Vertex>& vertices)
{
    const float halfWidth = kEditorGridAxisHalfWidth;
    const float top = kEditorGridAxisHeight;
    PushQuad(
        vertices,
        MakeVertex(Eigen::Vector3f(-halfWidth, 0.0f, 0.0f), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(halfWidth, 0.0f, 0.0f), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(halfWidth, top, 0.0f), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(-halfWidth, top, 0.0f), kEditorGridAxisYColor));
    PushQuad(
        vertices,
        MakeVertex(Eigen::Vector3f(-halfWidth, top, 0.0f), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(halfWidth, top, 0.0f), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(halfWidth, 0.0f, 0.0f), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(-halfWidth, 0.0f, 0.0f), kEditorGridAxisYColor));
    PushQuad(
        vertices,
        MakeVertex(Eigen::Vector3f(0.0f, 0.0f, -halfWidth), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(0.0f, 0.0f, halfWidth), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(0.0f, top, halfWidth), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(0.0f, top, -halfWidth), kEditorGridAxisYColor));
    PushQuad(
        vertices,
        MakeVertex(Eigen::Vector3f(0.0f, top, -halfWidth), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(0.0f, top, halfWidth), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(0.0f, 0.0f, halfWidth), kEditorGridAxisYColor),
        MakeVertex(Eigen::Vector3f(0.0f, 0.0f, -halfWidth), kEditorGridAxisYColor));
}

} // namespace

std::vector<Vertex> BuildEditorGridVertices()
{
    std::vector<Vertex> vertices;
    // 容量与实际产出口径共用 EditorGridExpectedVertexCount，二者不会各自漂移。
    vertices.reserve(EditorGridExpectedVertexCount());

    for (int lineIndex = -kEditorGridHalfExtent; lineIndex <= kEditorGridHalfExtent; ++lineIndex)
    {
        const float offset = lineIndex * kEditorGridSpacing;
        const bool isMajor = lineIndex % kEditorGridMajorLineStep == 0;
        const float halfWidth =
            isMajor ? kEditorGridMajorLineHalfWidth : kEditorGridLineHalfWidth;
        // 沿 X 的线里 z = 0 那条是 X 轴，沿 Z 的线里 x = 0 那条是 Z 轴；
        // 轴线本身就是网格的加重中线，因此不需要额外画一遍。
        AppendGroundLine(
            vertices,
            true,
            offset,
            halfWidth,
            lineIndex == 0
                ? kEditorGridAxisXColor
                : (isMajor ? kEditorGridMajorLineColor : kEditorGridMinorLineColor));
        AppendGroundLine(
            vertices,
            false,
            offset,
            halfWidth,
            lineIndex == 0
                ? kEditorGridAxisZColor
                : (isMajor ? kEditorGridMajorLineColor : kEditorGridMinorLineColor));
    }
    AppendVerticalAxis(vertices);
    return vertices;
}

} // namespace VL

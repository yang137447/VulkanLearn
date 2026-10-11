#include "render/editor/editorViewportGridGeometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace VL
{
namespace
{

constexpr float kExtent = kEditorGridHalfExtent * kEditorGridSpacing;

bool HasSameColor(const Eigen::Vector4f& lhs, const Eigen::Vector4f& rhs)
{
    return lhs == rhs;
}

size_t CountVerticesWithColor(
    const std::vector<Vertex>& vertices,
    const Eigen::Vector4f& color)
{
    return static_cast<size_t>(std::count_if(
        vertices.begin(),
        vertices.end(),
        [&color](const Vertex& vertex) { return HasSameColor(vertex.color, color); }));
}

bool IsGroundVertex(const Vertex& vertex)
{
    return vertex.position.y() == kEditorGridGroundLift;
}

// PushQuad 的顶点顺序固定为 a b c a c d，测试据此把顶点流切回四边形。
bool IsQuadWithSharedCorners(
    const std::vector<Vertex>& vertices,
    size_t quadIndex)
{
    const size_t base = quadIndex * kEditorGridVerticesPerQuad;
    return vertices[base].position == vertices[base + 3].position &&
        vertices[base + 2].position == vertices[base + 4].position &&
        vertices[base + 1].position != vertices[base + 5].position;
}

} // namespace

TEST(EditorViewportGridGeometry, GroundVerticesAreLiftedOffTheGroundPlane)
{
    const std::vector<Vertex> vertices = BuildEditorGridVertices();

    size_t groundVertexCount = 0;
    for (const Vertex& vertex : vertices)
    {
        if (!IsGroundVertex(vertex))
        {
            // 其余顶点只能是竖直 Y 轴，它必须从地面起算到轴高。
            EXPECT_TRUE(
                vertex.position.y() == 0.0f ||
                vertex.position.y() == kEditorGridAxisHeight);
            continue;
        }

        ++groundVertexCount;
        // 地面几何与网格共面时深度相等会让 lessOrEqual 的遮挡随机归属，
        // 因此地面顶点必须抬升，且绝不允许落在 y == 0。
        EXPECT_NE(vertex.position.y(), 0.0f);
        // 条带在延伸方向上正好铺满半幅，线宽只允许溢出到半宽，不会变成整块平面。
        EXPECT_LE(
            std::abs(vertex.position.x()),
            kExtent + kEditorGridMajorLineHalfWidth);
        EXPECT_LE(
            std::abs(vertex.position.z()),
            kExtent + kEditorGridMajorLineHalfWidth);
        EXPECT_TRUE(
            std::abs(vertex.position.x()) == kExtent ||
            std::abs(vertex.position.z()) == kExtent);
    }

    EXPECT_EQ(
        groundVertexCount,
        EditorGridGroundLineCount() * static_cast<size_t>(kEditorGridVerticesPerQuad));
}

TEST(EditorViewportGridGeometry, OriginLinesCarryAxisColorsAndMajorsCarryMajorColor)
{
    const std::vector<Vertex> vertices = BuildEditorGridVertices();

    const size_t quadCount = static_cast<size_t>(kEditorGridVerticesPerQuad);
    // 每个线族 2 * kEditorGridHalfExtent + 1 条线：其中 offset == 0 是轴线，
    // ±10 / ±20 共 4 条加重线，剩下 44 条是次线；两个线族都要计入。
    const size_t linesPerFamily = static_cast<size_t>(2 * kEditorGridHalfExtent + 1);
    const size_t majorLinesPerFamily =
        2 * static_cast<size_t>(kEditorGridHalfExtent / kEditorGridMajorLineStep);
    const size_t majorLineCount = 2 * majorLinesPerFamily;
    const size_t minorLineCount = 2 * (linesPerFamily - 1 - majorLinesPerFamily);

    EXPECT_EQ(CountVerticesWithColor(vertices, kEditorGridAxisXColor), quadCount);
    EXPECT_EQ(CountVerticesWithColor(vertices, kEditorGridAxisZColor), quadCount);
    EXPECT_EQ(
        CountVerticesWithColor(vertices, kEditorGridMajorLineColor),
        majorLineCount * quadCount);
    EXPECT_EQ(
        CountVerticesWithColor(vertices, kEditorGridMinorLineColor),
        minorLineCount * quadCount);

    for (const Vertex& vertex : vertices)
    {
        if (!HasSameColor(vertex.color, kEditorGridAxisXColor))
        {
            continue;
        }

        // 沿 X 的轴线：z 只有 ±主线半宽，x 铺满半幅。
        EXPECT_EQ(std::abs(vertex.position.x()), kExtent);
        EXPECT_EQ(std::abs(vertex.position.z()), kEditorGridMajorLineHalfWidth);
    }

    for (const Vertex& vertex : vertices)
    {
        if (!HasSameColor(vertex.color, kEditorGridAxisZColor))
        {
            continue;
        }

        EXPECT_EQ(std::abs(vertex.position.z()), kExtent);
        EXPECT_EQ(std::abs(vertex.position.x()), kEditorGridMajorLineHalfWidth);
    }
}

TEST(EditorViewportGridGeometry, VerticalAxisIsTwoPerpendicularStripsWithBothWindings)
{
    const std::vector<Vertex> vertices = BuildEditorGridVertices();
    const size_t axisVertexCount =
        static_cast<size_t>(kEditorGridVerticalAxisQuadCount) *
        static_cast<size_t>(kEditorGridVerticesPerQuad);
    ASSERT_GE(vertices.size(), axisVertexCount);

    // Y 轴是顶点流的最后一段；它只表达方向，因此只允许在 y ∈ [0, 高度] 上取值。
    const size_t axisBase = vertices.size() - axisVertexCount;
    for (size_t index = axisBase; index < vertices.size(); ++index)
    {
        const Vertex& vertex = vertices[index];
        EXPECT_TRUE(HasSameColor(vertex.color, kEditorGridAxisYColor));
        EXPECT_TRUE(
            vertex.position.y() == 0.0f ||
            vertex.position.y() == kEditorGridAxisHeight);
    }

    const auto liesInPlaneZ = [&vertices, axisBase](size_t quadIndex)
    {
        const size_t base = axisBase + quadIndex * kEditorGridVerticesPerQuad;
        for (size_t index = base; index < base + kEditorGridVerticesPerQuad; ++index)
        {
            if (vertices[index].position.z() != 0.0f ||
                std::abs(vertices[index].position.x()) != kEditorGridAxisHalfWidth)
            {
                return false;
            }
        }
        return true;
    };
    const auto liesInPlaneX = [&vertices, axisBase](size_t quadIndex)
    {
        const size_t base = axisBase + quadIndex * kEditorGridVerticesPerQuad;
        for (size_t index = base; index < base + kEditorGridVerticesPerQuad; ++index)
        {
            if (vertices[index].position.x() != 0.0f ||
                std::abs(vertices[index].position.z()) != kEditorGridAxisHalfWidth)
            {
                return false;
            }
        }
        return true;
    };

    // 两片条带的平面法线分别是 Z 与 X，因此互相垂直；每片正反各一份绕序，
    // 否则剔除打开时相机落到另一象限会让这条竖轴整片消失。
    EXPECT_TRUE(liesInPlaneZ(0));
    EXPECT_TRUE(liesInPlaneZ(1));
    EXPECT_TRUE(liesInPlaneX(2));
    EXPECT_TRUE(liesInPlaneX(3));
    EXPECT_TRUE(IsQuadWithSharedCorners(vertices, axisBase / kEditorGridVerticesPerQuad));
}

TEST(EditorViewportGridGeometry, EmitsTriangleQuadsAndMatchesItsReserveCount)
{
    const std::vector<Vertex> vertices = BuildEditorGridVertices();

    EXPECT_EQ(vertices.size(), EditorGridExpectedVertexCount());
    EXPECT_EQ(vertices.size() % static_cast<size_t>(kEditorGridVerticesPerQuad), 0u);
    // 容量预留与实际产出口径相同，因此不会因为 reserve 落后于生产而反复扩容。
    EXPECT_GE(vertices.capacity(), EditorGridExpectedVertexCount());

    const size_t quadCount = vertices.size() / static_cast<size_t>(kEditorGridVerticesPerQuad);
    EXPECT_EQ(
        quadCount,
        EditorGridGroundLineCount() +
            static_cast<size_t>(kEditorGridVerticalAxisQuadCount));
    for (size_t quadIndex = 0; quadIndex < quadCount; ++quadIndex)
    {
        EXPECT_TRUE(IsQuadWithSharedCorners(vertices, quadIndex));
    }
}

TEST(EditorViewportGridGeometry, RebuildProducesIdenticalStaticGeometry)
{
    const std::vector<Vertex> first = BuildEditorGridVertices();
    const std::vector<Vertex> second = BuildEditorGridVertices();

    ASSERT_EQ(first.size(), second.size());
    for (size_t index = 0; index < first.size(); ++index)
    {
        // 顶点缓冲只上传一次，因此生成必须是确定性的：同样的输入总得到同样的几何。
        EXPECT_EQ(first[index].position, second[index].position);
        EXPECT_TRUE(HasSameColor(first[index].color, second[index].color));
    }
}

} // namespace VL

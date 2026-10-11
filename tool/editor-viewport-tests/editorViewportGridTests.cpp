#include "render/editor/editorViewportGridGeometry.h"

#include <gtest/gtest.h>

#include <iostream>

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

// PushQuad 为每个逻辑四边形输出两份绕序，各 6 个顶点，因此步长是 2 * kEditorGridVerticesPerQuad：
// 前 6 个是一份绕序 a b c a c d，后 6 个是同一四角的反向绕序。
constexpr size_t kVerticesPerEmittedQuad =
    2 * static_cast<size_t>(kEditorGridVerticesPerQuad);

// 一条绕序内部的顶点顺序固定为 a b c a c d，测试据此把顶点流切回四边形。
bool IsQuadWithSharedCorners(
    const std::vector<Vertex>& vertices,
    size_t quadIndex)
{
    const size_t base = quadIndex * kVerticesPerEmittedQuad;
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
        EditorGridGroundLineCount() * static_cast<size_t>(kEditorGridGroundWindings) *
            static_cast<size_t>(kEditorGridVerticesPerQuad));
}

TEST(EditorViewportGridGeometry, OriginLinesCarryAxisColorsAndMajorsCarryMajorColor)
{
    const std::vector<Vertex> vertices = BuildEditorGridVertices();

    const size_t quadCount = static_cast<size_t>(kEditorGridVerticesPerQuad);
    // 地面把每条线作为 quad 出两次绕序，所以一条线的顶点数是 2 * kEditorGridVerticesPerQuad。
    // 这个系数来自 kEditorGridGroundWindings，测试与生成器共用同一份口径。
    const size_t verticesPerGroundLine =
        static_cast<size_t>(kEditorGridGroundWindings) *
        static_cast<size_t>(kEditorGridVerticesPerQuad);
    // 每个线族 2 * kEditorGridHalfExtent + 1 条线：其中 offset == 0 是轴线，
    // ±10 / ±20 共 4 条加重线，剩下 44 条是次线；两个线族都要计入。
    const size_t linesPerFamily = static_cast<size_t>(2 * kEditorGridHalfExtent + 1);
    const size_t majorLinesPerFamily =
        2 * static_cast<size_t>(kEditorGridHalfExtent / kEditorGridMajorLineStep);
    const size_t majorLineCount = 2 * majorLinesPerFamily;
    const size_t minorLineCount = 2 * (linesPerFamily - 1 - majorLinesPerFamily);

    EXPECT_EQ(
        CountVerticesWithColor(vertices, kEditorGridAxisXColor), verticesPerGroundLine);
    EXPECT_EQ(
        CountVerticesWithColor(vertices, kEditorGridAxisZColor), verticesPerGroundLine);
    EXPECT_EQ(
        CountVerticesWithColor(vertices, kEditorGridMajorLineColor),
        majorLineCount * verticesPerGroundLine);
    EXPECT_EQ(
        CountVerticesWithColor(vertices, kEditorGridMinorLineColor),
        minorLineCount * verticesPerGroundLine);

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
    // 实测口径（2026-10-12 探针，不再靠推导）：轴段共 48 个顶点、按 6 个顶点一个四边形切分，
    // 即 8 个四边形 = 2 片条带 × 正反 2 面 × 绕序 2 份。Z 平面的 4 个在前，X 平面的 4 个在后。
    // 这里刻意用 kEditorGridVerticesPerQuad 而不是 kVerticesPerEmittedQuad：
    // 12 那个步长描述的是地面条带（每个逻辑四边形在流里占 12），轴段的实际布局是 6。
    const size_t axisQuadVertexCount = static_cast<size_t>(kEditorGridVerticesPerQuad);
    const size_t axisQuadCount =
        static_cast<size_t>(kEditorGridVerticalAxisStripCount) *
        static_cast<size_t>(kEditorGridGroundWindings) *
        static_cast<size_t>(kEditorGridGroundWindings);
    const size_t axisVertexCount = axisQuadCount * axisQuadVertexCount;
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

    const auto allAxisVerticesSatisfy =
        [&vertices, axisBase, axisQuadVertexCount](size_t quadIndex, bool planeZ)
    {
        const size_t base = axisBase + quadIndex * axisQuadVertexCount;
        for (size_t index = base; index < base + axisQuadVertexCount; ++index)
        {
            if (planeZ)
            {
                if (vertices[index].position.z() != 0.0f ||
                    std::abs(vertices[index].position.x()) != kEditorGridAxisHalfWidth)
                {
                    return false;
                }
            }
            else if (vertices[index].position.x() != 0.0f ||
                std::abs(vertices[index].position.z()) != kEditorGridAxisHalfWidth)
            {
                return false;
            }
        }
        return true;
    };

    // Z 平面的 4 个四边形，随后是 X 平面的 4 个。两面都出是必须的：
    // 剔除打开时相机落到另一象限会让整片条带消失。
    for (size_t quad = 0; quad < 4; ++quad)
    {
        EXPECT_TRUE(allAxisVerticesSatisfy(quad, true)) << "quad " << quad;
    }
    for (size_t quad = 4; quad < axisQuadCount; ++quad)
    {
        EXPECT_TRUE(allAxisVerticesSatisfy(quad, false)) << "quad " << quad;
    }
}

TEST(EditorViewportGridGeometry, EmitsTriangleQuadsAndMatchesItsReserveCount)
{
    const std::vector<Vertex> vertices = BuildEditorGridVertices();
    // 顶点流里每个逻辑四边形占 kVerticesPerEmittedQuad（= 12）个顶点：6 个一份绕序，两份。
    // 用它作除数才是"逻辑四边形数"；除以 6 会把每个四边形数成两个，得到一半的 quadCount。
    const size_t verticesPerQuad = kVerticesPerEmittedQuad;

    EXPECT_EQ(vertices.size(), EditorGridExpectedVertexCount());
    EXPECT_EQ(vertices.size() % verticesPerQuad, 0u);
    // 容量预留与实际产出口径相同，因此不会因为 reserve 落后于生产而反复扩容。
    EXPECT_GE(vertices.capacity(), EditorGridExpectedVertexCount());

    const size_t quadCount = vertices.size() / verticesPerQuad;
    EXPECT_EQ(quadCount, EditorGridVertexQuadCount());
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

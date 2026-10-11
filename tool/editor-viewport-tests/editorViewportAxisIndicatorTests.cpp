#include "editor/ui/editorViewportAxisIndicator.h"

#include <gtest/gtest.h>

#include <cmath>

namespace VL::EditorUi
{
namespace
{

// 一组规范正交基：相机在原点朝 -Z 看，右手为 +X，上为 +Y。
// 这是引擎的默认朝向（Camera 构造即 look from (0,2,2) at origin，up 为 +Y）。
const Eigen::Vector3f kRight(1.0f, 0.0f, 0.0f);
const Eigen::Vector3f kUp(0.0f, 1.0f, 0.0f);
const Eigen::Vector3f kForward(0.0f, 0.0f, -1.0f);

} // namespace

// 世界 +X 在屏幕上必须是向右的横线：这是"指示器真的在描述朝向"的最小证据。
TEST(EditorViewportAxisIndicator, WorldXProjectsToScreenRight)
{
    const auto triad = ProjectAxisIndicatorTriad(kRight, kUp, kForward);
    const AxisIndicatorProjection& x = triad[0];
    EXPECT_FALSE(x.degenerate);
    EXPECT_NEAR(x.x, 1.0f, 1e-4f);
    EXPECT_NEAR(x.y, 0.0f, 1e-4f);
    // 屏幕角度 0 度 = 从左向右。
    EXPECT_NEAR(AxisIndicatorRotationDegrees(x), 0.0f, 1e-3f);
}

// 世界 +Y 在屏幕上必须是向上的，而屏幕 y 轴向下，所以投影的 y 分量为负、角度为 -90 度。
// 这条断言专门钉住符号约定：写反了画面会上下颠倒，而画面上未必立刻看得出来。
TEST(EditorViewportAxisIndicator, WorldYProjectsUpwardsWithNegativeScreenY)
{
    const auto triad = ProjectAxisIndicatorTriad(kRight, kUp, kForward);
    const AxisIndicatorProjection& y = triad[1];
    EXPECT_FALSE(y.degenerate);
    EXPECT_NEAR(y.x, 0.0f, 1e-4f);
    EXPECT_NEAR(y.y, -1.0f, 1e-4f);
    EXPECT_NEAR(AxisIndicatorRotationDegrees(y), -90.0f, 1e-3f);
}

// 与视线平行的轴在屏幕上没有长度。它必须被标成退化，否则面板会照着浮点噪声画出一根
// 方向随机的线。这里用默认朝向下的世界 -Z（= 视线方向）来触发。
TEST(EditorViewportAxisIndicator, AxisAlongTheViewDirectionIsDegenerate)
{
    const Eigen::Vector3f back(0.0f, 0.0f, 1.0f); // 世界 +Z 与 forward 反向，仍与视线平行
    const AxisIndicatorProjection projection =
        ProjectAxisIndicator(back, kRight, kUp, kForward);
    EXPECT_TRUE(projection.degenerate);
    // 退化时角度必须是确定值，不能随噪声变化。
    EXPECT_EQ(AxisIndicatorRotationDegrees(projection), 0.0f);

    float offsetX = 1.0f;
    float offsetY = 1.0f;
    AxisIndicatorLabelOffset(projection, offsetX, offsetY);
    EXPECT_EQ(offsetX, 0.0f);
    EXPECT_EQ(offsetY, 0.0f);
}

// 深度要能区分"朝向观察者"与"背离观察者"：面板据此决定绘制顺序，让朝前的轴盖住朝后的。
// forward 指向屏幕内，因此 dot(axis, forward) 越小越靠近观察者。
TEST(EditorViewportAxisIndicator, DepthOrdersAxesTowardsTheViewer)
{
    const auto triad = ProjectAxisIndicatorTriad(kRight, kUp, kForward);
    // 默认朝向下三条轴的深度：+X 与 +Y 为 0（都在屏幕平面内），+Z 为 -1（朝向观察者）。
    EXPECT_NEAR(triad[0].depth, 0.0f, 1e-4f);
    EXPECT_NEAR(triad[1].depth, 0.0f, 1e-4f);
    EXPECT_NEAR(triad[2].depth, -1.0f, 1e-4f);
    EXPECT_LT(triad[2].depth, triad[0].depth);
}

// 标签落在轴的方向上、且距离锚点固定。方向错会让字母飘到别的轴上。
TEST(EditorViewportAxisIndicator, LabelSitsAlongTheAxisDirection)
{
    const auto triad = ProjectAxisIndicatorTriad(kRight, kUp, kForward);

    float xOffset = 0.0f;
    float yOffset = 0.0f;
    AxisIndicatorLabelOffset(triad[0], xOffset, yOffset);
    EXPECT_NEAR(xOffset, kAxisIndicatorLabelRadiusPx, 1e-3f);
    EXPECT_NEAR(yOffset, 0.0f, 1e-3f);

    AxisIndicatorLabelOffset(triad[1], xOffset, yOffset);
    EXPECT_NEAR(xOffset, 0.0f, 1e-3f);
    EXPECT_NEAR(yOffset, -kAxisIndicatorLabelRadiusPx, 1e-3f);

    // 半径必须大于杆长，否则字母会压在杆上。
    EXPECT_GT(kAxisIndicatorLabelRadiusPx, kAxisIndicatorRadiusPx);
}

// 相机绕 Y 轴转 90 度（右转）后，世界 +Z 应当落到屏幕右侧。这是"指示器跟随相机"的判据：
// 一个静态指示器会在这条断言上失败。
TEST(EditorViewportAxisIndicator, YawByNinetyDegreesMovesWorldZToScreenRight)
{
    // 相机右转 90 度：right 变成 -Z，forward 变成 -X。
    const Eigen::Vector3f right(0.0f, 0.0f, -1.0f);
    const Eigen::Vector3f forward(-1.0f, 0.0f, 0.0f);
    const auto triad = ProjectAxisIndicatorTriad(right, kUp, forward);

    const AxisIndicatorProjection& z = triad[2];
    EXPECT_FALSE(z.degenerate);
    EXPECT_NEAR(z.x, -1.0f, 1e-4f);
    EXPECT_NEAR(z.y, 0.0f, 1e-4f);
}

} // namespace VL::EditorUi

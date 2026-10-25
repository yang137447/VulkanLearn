#include "editor/ui/tressfxHairPreview.h"

#include <gtest/gtest.h>

#include <string_view>

// 毛发视口的两个可测判定：skinning 守卫文案、以及“左键拖 = 转模型”的角度累计。
// 它们都是纯函数/纯值类型，不依赖 RmlUi、World 或 Vulkan。
namespace
{
using VL::EditorUi::TressfxHairPreviewBlockedMessage;
using VL::EditorUi::TressfxHairRotation;
using VL::EditorUi::TressfxHairSkinnedAssetMessage;
}

TEST(TressfxHairPreview, SkinnedAssetIsBlockedWithAnExplicitExplanation)
{
    const std::string_view message = TressfxHairPreviewBlockedMessage(true, true);
    EXPECT_EQ(message, TressfxHairSkinnedAssetMessage);
    // 文案必须同时说明原因与归属，否则用户只会看到"没有预览"。
    EXPECT_NE(message.find("simulation.skinning"), std::string_view::npos);
    EXPECT_NE(message.find("Blueprint TressFX component"), std::string_view::npos);
}

TEST(TressfxHairPreview, UnskinnedTressfxAndOtherAssetsArePreviewable)
{
    EXPECT_TRUE(TressfxHairPreviewBlockedMessage(true, false).empty());
    EXPECT_TRUE(TressfxHairPreviewBlockedMessage(false, true).empty());
    EXPECT_TRUE(TressfxHairPreviewBlockedMessage(false, false).empty());
}

TEST(TressfxHairPreview, HorizontalDragTurnsYawAndVerticalDragTiltsPitch)
{
    TressfxHairRotation rotation;
    rotation.Drag(10.0f, 0.0f);
    EXPECT_FLOAT_EQ(rotation.yaw, 5.0f);
    EXPECT_FLOAT_EQ(rotation.pitch, 0.0f);
    rotation.Drag(0.0f, -8.0f);
    EXPECT_FLOAT_EQ(rotation.yaw, 5.0f);
    EXPECT_FLOAT_EQ(rotation.pitch, -4.0f);
    const auto euler = rotation.Euler();
    EXPECT_FLOAT_EQ(euler[0], rotation.pitch);
    EXPECT_FLOAT_EQ(euler[1], rotation.yaw);
    EXPECT_FLOAT_EQ(euler[2], 0.0f);
}

TEST(TressfxHairPreview, YawWrapsIntoOneTurnAndPitchStaysAwayFromGimbalLock)
{
    TressfxHairRotation rotation;
    rotation.Drag(2000.0f, 0.0f);   // 1000 度
    EXPECT_GE(rotation.yaw, 0.0f);
    EXPECT_LT(rotation.yaw, 360.0f);
    rotation.Drag(-4000.0f, 0.0f);
    EXPECT_GE(rotation.yaw, 0.0f);
    EXPECT_LT(rotation.yaw, 360.0f);

    rotation.Drag(0.0f, 100000.0f);
    EXPECT_FLOAT_EQ(rotation.pitch, 89.0f);
    rotation.Drag(0.0f, -100000.0f);
    EXPECT_FLOAT_EQ(rotation.pitch, -89.0f);
}

TEST(TressfxHairPreview, ResetReturnsToTheAuthoredPose)
{
    TressfxHairRotation rotation;
    rotation.Drag(120.0f, 80.0f);
    ASSERT_NE(rotation.yaw, 0.0f);
    ASSERT_NE(rotation.pitch, 0.0f);
    rotation.Reset();
    EXPECT_FLOAT_EQ(rotation.yaw, 0.0f);
    EXPECT_FLOAT_EQ(rotation.pitch, 0.0f);
}

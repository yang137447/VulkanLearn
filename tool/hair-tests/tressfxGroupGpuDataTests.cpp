#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hair/tressfxGroupGpuData.h"
#include "hair/tressfxGroupPalette.h"

namespace
{
using VL::Hair::BuildTressfxGroupGpuData;
using VL::Hair::TressfxDefaultGroupColor;
using VL::Hair::TressfxGroupColorForName;
}

TEST(TressfxGroupPalette, NameColorsAreStableAndDeterministic)
{
    const auto first = TressfxGroupColorForName("fringe");
    EXPECT_EQ(first, TressfxGroupColorForName("fringe"));
    // 调色板跨会话固定；改变 hash/表会让已保存资产的预览颜色漂移。
    EXPECT_EQ(first, (VL::Hair::TressfxGroupColor{200, 140, 110}));
    EXPECT_NE(first, TressfxGroupColorForName("side"));
}

TEST(TressfxGroupPalette, DefaultGroupIsTheNeutralGraySharedWithTheUi)
{
    EXPECT_EQ(TressfxDefaultGroupColor(), (VL::Hair::TressfxGroupColor{120, 128, 136}));
}

TEST(TressfxGroupGpuData, EmptyIdsBecomeDefaultAndColorTableStartsWithGray)
{
    const auto data = BuildTressfxGroupGpuData(4, {}, {"fringe", "side"});
    ASSERT_EQ(data.guideGroupIds.size(), 4u);
    for (uint32_t id : data.guideGroupIds)
        EXPECT_EQ(id, 0u);
    ASSERT_EQ(data.groupColors.size(), 3u);
    EXPECT_FLOAT_EQ(data.groupColors[0].r, 120.0f / 255.0f);
    EXPECT_FLOAT_EQ(data.groupColors[0].g, 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(data.groupColors[0].b, 136.0f / 255.0f);
    EXPECT_FLOAT_EQ(data.groupColors[0].a, 1.0f);
    EXPECT_FLOAT_EQ(data.groupColors[1].a, 1.0f);
    EXPECT_FLOAT_EQ(data.groupColors[2].a, 1.0f);
    const auto fringe = TressfxGroupColorForName("fringe");
    EXPECT_FLOAT_EQ(data.groupColors[1].r, static_cast<float>(fringe.r) / 255.0f);
    EXPECT_FLOAT_EQ(data.groupColors[1].g, static_cast<float>(fringe.g) / 255.0f);
    EXPECT_FLOAT_EQ(data.groupColors[1].b, static_cast<float>(fringe.b) / 255.0f);
}

TEST(TressfxGroupGpuData, ImportedIdsAreCopiedPerGuide)
{
    const auto data = BuildTressfxGroupGpuData(3, {0u, 2u, 1u}, {"fringe", "side"});
    ASSERT_EQ(data.guideGroupIds.size(), 3u);
    EXPECT_EQ(data.guideGroupIds[0], 0u);
    EXPECT_EQ(data.guideGroupIds[1], 2u);
    EXPECT_EQ(data.guideGroupIds[2], 1u);
    EXPECT_EQ(data.groupColors.size(), 3u);
}

TEST(TressfxGroupGpuData, WrongSizedImportedIdsFallBackToDefault)
{
    const auto data = BuildTressfxGroupGpuData(2, {1u}, {"fringe"});
    ASSERT_EQ(data.guideGroupIds.size(), 2u);
    EXPECT_EQ(data.guideGroupIds[0], 0u);
    EXPECT_EQ(data.guideGroupIds[1], 0u);
}

TEST(TressfxGroupGpuData, ReservedColorCapacityPadsShrunkTablesWithDefaultGray)
{
    const auto data = BuildTressfxGroupGpuData(2, {0u, 2u}, {"fringe", "side"},
        VL::Hair::TressfxGroupPreviewColorCapacity);
    ASSERT_EQ(data.groupColors.size(),
        static_cast<size_t>(VL::Hair::TressfxGroupPreviewColorCapacity));
    // 组数减少后，旧组号短暂落在尾部时读到默认灰，而不是未初始化/纯黑。
    EXPECT_FLOAT_EQ(data.groupColors.back().r, 120.0f / 255.0f);
    EXPECT_FLOAT_EQ(data.groupColors.back().g, 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(data.groupColors.back().b, 136.0f / 255.0f);
    EXPECT_FLOAT_EQ(data.groupColors.back().a, 1.0f);
}

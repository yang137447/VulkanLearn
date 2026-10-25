#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "editor/ui/tressfxHairSimulationControls.h"

namespace
{
using namespace VL::EditorUi;

const TressfxSimulationControl* FindControl(const std::string& field)
{
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    for (std::size_t index = 0; index < count; ++index)
        if (field == controls[index].field) return &controls[index];
    return nullptr;
}
} // namespace

// 面板控件表必须覆盖且只覆盖可实时覆盖的 9 个作者字段，默认值与
// TressfxSimulationConfig 的成员初值一致；多一个字段会让面板显示资产并不拥有的值，
// 少一个字段则作者只能改 JSON。
TEST(TressfxHairSimulationControls, CoverEveryLiveParameterWithAuthoredDefaults)
{
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    ASSERT_EQ(count, 9u);
    const std::map<std::string, std::array<double, 3>> expected = {
        {"gravity", {0.0, -980.0, 0.0}},
        {"wind", {0.0, 0.0, 0.0}},
        {"damping", {0.035, 0.0, 0.0}},
        {"iterations", {3.0, 0.0, 0.0}},
        {"bendStiffness", {0.3, 0.0, 0.0}},
        {"globalShapeStiffness", {0.0, 0.0, 0.0}},
        {"globalShapeEffectiveRange", {0.0, 0.0, 0.0}},
        {"vspCoefficient", {0.0, 0.0, 0.0}},
        {"vspAccelThresholdMax", {140.0, 0.0, 0.0}},
    };
    EXPECT_EQ(expected.size(), count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto found = expected.find(controls[index].field);
        ASSERT_NE(found, expected.end()) << controls[index].field;
        EXPECT_EQ(controls[index].defaults, found->second) << controls[index].field;
        EXPECT_GE(controls[index].components, 1u);
        EXPECT_LE(controls[index].components, 3u);
        if (controls[index].hasRange)
        {
            EXPECT_LT(controls[index].minValue, controls[index].maxValue);
            EXPECT_TRUE(std::isfinite(controls[index].minValue));
            EXPECT_TRUE(std::isfinite(controls[index].maxValue));
        }
    }
}

TEST(TressfxHairSimulationControls, ReadsAuthoredValuesAndFallsBackToDefaults)
{
    const auto* damping = FindControl("damping");
    const auto* gravity = FindControl("gravity");
    const auto* wind = FindControl("wind");
    ASSERT_NE(damping, nullptr);
    ASSERT_NE(gravity, nullptr);
    ASSERT_NE(wind, nullptr);
    const nlohmann::json simulation = {
        {"damping", 0.02},
        {"gravity", {0.0, -500.0, 0.0}},
    };
    EXPECT_DOUBLE_EQ(TressfxSimulationValue(simulation, *damping)[0], 0.02);
    EXPECT_DOUBLE_EQ(TressfxSimulationValue(simulation, *gravity)[1], -500.0);
    // 未作者字段回退默认值，面板不能把“缺字段”显示成 0。
    EXPECT_DOUBLE_EQ(TressfxSimulationValue(simulation, *wind)[0], 0.0);
    EXPECT_DOUBLE_EQ(TressfxSimulationValue(nlohmann::json::object(), *damping)[0], 0.035);
    // 类型不符同样回退，而不是抛出。
    EXPECT_DOUBLE_EQ(TressfxSimulationValue(
        nlohmann::json{{"gravity", "not-a-vector"}}, *gravity)[1], -980.0);
}

TEST(TressfxHairSimulationControls, PanelValuesMatchDraftWithinTolerance)
{
    const nlohmann::json simulation = {
        {"gravity", {0.0, -980.0, 0.0}},
        {"damping", 0.035},
        {"iterations", 3},
    };
    nlohmann::json panel = nlohmann::json::object();
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto value = TressfxSimulationValue(simulation, controls[index]);
        panel[controls[index].field] = nlohmann::json::array({value[0], value[1], value[2]});
    }
    EXPECT_TRUE(TressfxSimulationValuesMatch(simulation, panel));
    panel["damping"] = nlohmann::json::array({0.02, 0.0, 0.0});
    EXPECT_FALSE(TressfxSimulationValuesMatch(simulation, panel));
    // 只有浮点末位差异时仍视为相同：保存不应因为格式化精度再发一次 modify。
    panel["damping"] = nlohmann::json::array({0.0350000001, 0.0, 0.0});
    EXPECT_TRUE(TressfxSimulationValuesMatch(simulation, panel));
}

TEST(TressfxHairSimulationControls, TextFormatKeepsPrecisionWithoutPadding)
{
    EXPECT_EQ(TressfxSimulationText(0.02), "0.02");
    EXPECT_EQ(TressfxSimulationText(140.0), "140");
    EXPECT_EQ(TressfxSimulationText(-980.0), "-980");
}

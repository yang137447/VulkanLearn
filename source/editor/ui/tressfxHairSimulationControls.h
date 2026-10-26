#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace VL::EditorUi
{
inline bool TressfxGpuSimulationDefaults(const nlohmann::json& document)
{
    if (!document.contains("simulationDefaults")) return false;
    const auto defaults = document.value("simulationDefaults", nlohmann::json::object());
    return defaults.value("enabled", true) && defaults.value("backend", "cpu") == "gpu";
}

// 毛发资产视口 Simulation 段的控件表。字段名与作者字段、运行期命令完全一致；
// 范围只服务滑条展示（精确输入仍走文本），最终值域由运行期命令与资产校验收口。
// 默认值必须与 `TressfxSimulationConfig` 的成员初值一致，否则面板会显示一个
// 资产并不拥有的“当前值”。
struct TressfxSimulationControl
{
    const char* field = "";
    const char* label = "";
    const char* unit = "";
    uint32_t components = 1;
    bool hasRange = false;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    std::array<double, 3> defaults{0.0, 0.0, 0.0};
};

inline const TressfxSimulationControl* TressfxSimulationControls(std::size_t& count)
{
    static const TressfxSimulationControl controls[] = {
        {"gravity", "Gravity", "cm/s^2", 3, false, 0.0f, 0.0f, {0.0, -980.0, 0.0}},
        {"wind", "Wind", "force", 3, false, 0.0f, 0.0f, {0.0, 0.0, 0.0}},
        {"damping", "Damping", "", 1, true, 0.0f, 0.1f, {0.035, 0.0, 0.0}},
        {"iterations", "Iterations", "", 1, true, 1.0f, 16.0f, {3.0, 0.0, 0.0}},
        {"bendStiffness", "Bend", "", 1, true, 0.0f, 1.0f, {0.3, 0.0, 0.0}},
        {"globalShapeStiffness", "Global Shape", "", 1, true, 0.0f, 1.0f, {0.0, 0.0, 0.0}},
        {"globalShapeEffectiveRange", "Shape Range", "", 1, true, 0.0f, 1.0f, {0.0, 0.0, 0.0}},
        {"vspCoefficient", "VSP", "", 1, true, 0.0f, 1.0f, {0.0, 0.0, 0.0}},
        {"vspAccelThresholdMax", "VSP Threshold", "cm", 1, true, 1.0f, 2000.0f, {140.0, 0.0, 0.0}},
    };
    count = sizeof(controls) / sizeof(controls[0]);
    return controls;
}

// 从 simulationDefaults/overrides 的参数对象读取当前值；缺字段时回退类型默认值，
// 让面板在“字段还没作者”时仍可编辑，而不是显示 0 或空。
inline std::array<double, 3> TressfxSimulationValue(
    const nlohmann::json& simulation, const TressfxSimulationControl& control)
{
    std::array<double, 3> result = control.defaults;
    if (!simulation.is_object() || !simulation.contains(control.field)) return result;
    const auto& value = simulation.at(control.field);
    if (control.components == 1)
    {
        if (value.is_number()) result[0] = value.get<double>();
        return result;
    }
    if (!value.is_array() || value.size() != 3) return result;
    for (std::size_t index = 0; index < 3; ++index)
        if (value[index].is_number()) result[index] = value[index].get<double>();
    return result;
}

// 面板数值读数：与作者值同一精度量级，不输出多余尾零。
inline std::string TressfxSimulationText(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(6) << value;
    return stream.str();
}

// 保存前比对：草稿里的 simulation 值是否已经等于面板当前值。相等时不必再发
// 一次 modify（候选与草稿相同不会推进 revision），直接进入保存确认。
inline bool TressfxSimulationValuesMatch(const nlohmann::json& simulation,
    const nlohmann::json& panelValues)
{
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    if (!panelValues.is_object()) return false;
    for (std::size_t index = 0; index < count; ++index)
    {
        const TressfxSimulationControl& control = controls[index];
        if (!panelValues.contains(control.field)) continue;
        const auto& authored = panelValues.at(control.field);
        if (!authored.is_array() || authored.size() != 3) return false;
        const std::array<double, 3> expected = TressfxSimulationValue(simulation, control);
        for (std::size_t component = 0; component < 3; ++component)
        {
            if (!authored[component].is_number()) return false;
            if (std::abs(authored[component].get<double>() - expected[component]) >
                1.0e-6 * std::max(1.0, std::abs(expected[component])))
                return false;
        }
    }
    return true;
}
} // namespace VL::EditorUi

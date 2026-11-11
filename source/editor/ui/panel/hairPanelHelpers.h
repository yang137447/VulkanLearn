#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "editor/ui/panel/editorPanelInternalHelpers.h"
#include "editor/ui/tressfxHairSimulationControls.h"

namespace VL::EditorUi::HairPanelDetail
{

using Panel::Detail::ParseNumber;

// 面板当前值（可能是用户拖动后的会话值）优先于作者文档；缺字段时回退控件默认值。
inline std::array<double, 3> PanelSimulationValue(
    const nlohmann::json& values, const TressfxSimulationControl& control)
{
    std::array<double, 3> result = control.defaults;
    if (!values.is_object() || !values.contains(control.field)) return result;
    const auto& array = values.at(control.field);
    if (!array.is_array() || array.size() != 3) return result;
    for (std::size_t component = 0; component < 3; ++component)
        if (array[component].is_number()) result[component] = array[component].get<double>();
    return result;
}

struct HairGroupOverrideSpec
{
    const char* field;
    const char* label;
};

// 只有这些字段允许按组覆盖；顺序就是检查器的显示顺序，与资产契约一致。
inline constexpr HairGroupOverrideSpec HairGroupOverrideSpecs[] = {
    {"damping", "Damping"},
    {"bendStiffness", "Bend"},
    {"globalShapeStiffness", "Global Shape"},
    {"globalShapeEffectiveRange", "Shape Range"},
    {"vspCoefficient", "VSP"},
    {"vspAccelThresholdMax", "VSP Threshold"},
};

inline bool ParseHairGuideRange(const std::string& startText, const std::string& endText,
    std::pair<uint32_t, uint32_t>& range, std::string& error)
{
    uint32_t start = 0;
    uint32_t end = 0;
    if (!ParseNumber(startText, start) || !ParseNumber(endText, end))
    {
        error = "A guide range requires two non-negative integers.";
        return false;
    }
    range = {start, end};
    return true;
}

// 色块只表达名称 hash 颜色；不写资产，也不使用 rgba()（RmlUi 会把小数 alpha 截成 0）。
inline std::string HairGroupSwatch(const VL::Editor::TressfxGroupColor& color)
{
    return "<span class=\"hair-group-swatch\" style=\"background-color: rgb(" +
        std::to_string(color.r) + "," + std::to_string(color.g) + "," +
        std::to_string(color.b) + ");\"></span>";
}

// 资产默认值是当前 TF 文档解析后的值，不是写死的 UI 常量。
inline double HairGroupAssetDefault(const nlohmann::json& simulation, const std::string& field)
{
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    for (std::size_t index = 0; index < count; ++index)
        if (field == controls[index].field)
            return TressfxSimulationValue(simulation, controls[index])[0];
    return 0.0;
}

// 已保存组的 asset default 取 placement 解析后的 TressfxSimulationConfig；
// 没有预览快照（或新建/改名组尚未保存）时才回退到文档 simulationDefaults。
inline bool ResolvedGroupSimulationValue(
    const TressfxSimulationConfig& config, const std::string& field, double& value)
{
    if (field == "damping") value = config.damping;
    else if (field == "bendStiffness") value = config.bendStiffness;
    else if (field == "globalShapeStiffness") value = config.globalShapeStiffness;
    else if (field == "globalShapeEffectiveRange") value = config.globalShapeEffectiveRange;
    else if (field == "vspCoefficient") value = config.vspCoefficient;
    else if (field == "vspAccelThresholdMax") value = config.vspAccelThresholdMax;
    else return false;
    return true;
}

inline double HairGroupAssetDefault(
    const EditorSceneMeshSectionSnapshot* section,
    bool savedGroup,
    int32_t savedGroupIndex,
    const nlohmann::json& simulation,
    const std::string& field)
{
    if (savedGroup && section != nullptr && section->tressfxGroupConfigs &&
        savedGroupIndex >= 0 &&
        static_cast<size_t>(savedGroupIndex) < section->tressfxGroupConfigs->size())
    {
        double resolved = 0.0;
        if (ResolvedGroupSimulationValue(
                section->tressfxGroupConfigs->at(static_cast<size_t>(savedGroupIndex)),
                field, resolved))
            return resolved;
    }
    return HairGroupAssetDefault(simulation, field);
}

inline std::string HairGroupAssetDefaultText(
    const EditorSceneMeshSectionSnapshot* section,
    bool savedGroup,
    int32_t savedGroupIndex,
    const nlohmann::json& simulation,
    const std::string& field)
{
    std::string text = TressfxSimulationText(HairGroupAssetDefault(
        section, savedGroup, savedGroupIndex, simulation, field));
    // 未保存组没有 placement 解析结果，必须显式提示显示的是文档回退值。
    if (!savedGroup)
        text += "; draft group - save and reload for placement-resolved value";
    return text;
}

inline bool HairGroupsMatchDocument(const nlohmann::json& document, bool reset,
    const nlohmann::json& value)
{
    const bool hasGroups = document.contains("groups") && !document.at("groups").is_null();
    // 空的 `groups: []` 不是“无组”的规范写法；保存应把它改成删除键，
    // 否则编辑器会一直提示没有变化，而校验层仍可能拒绝该数组。
    if (reset) return !hasGroups;
    return hasGroups && document.at("groups") == value;
}

inline Eigen::Vector2f ScreenCentroid(const std::vector<Eigen::Vector2f>& points)
{
    Eigen::Vector2f centroid = Eigen::Vector2f::Zero();
    if (points.empty()) return centroid;
    for (const Eigen::Vector2f& point : points) centroid += point;
    return centroid / static_cast<float>(points.size());
}

} // namespace VL::EditorUi::HairPanelDetail

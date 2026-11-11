#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include "editor/brush/brushProfile.h"
#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "editor/ui/panel/actorMarkup.h"
#include "editor/ui/tressfxHairSimulationControls.h"
#include "ui/uiAction.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi::Panel::Detail
{

using UiWidgets::Text;
using UiWidgets::Option;
using UiWidgets::WidgetAction;
using UiWidgets::WidgetRefreshScope;

// 目录条目给 UI 一个同步的“这是不是 tressfx、有没有 simulation.skinning”判定，
// 使打开资产时能立刻决定是否请求预览；快照到达后再作为权威来源复核。
inline const EditorJsonAssetEntry* FindCatalogEntry(
    const std::shared_ptr<const EditorJsonAssetCatalogSnapshot>& catalog,
    const std::string& assetPath)
{
    if (!catalog) return nullptr;
    for (const auto& entry : catalog->assets)
        if (entry.assetPath == assetPath) return &entry;
    return nullptr;
}

inline bool SameOfflineControls(const UiViewModelSnapshot& a, const UiViewModelSnapshot& b){
    return std::tie(a.offlineMode, a.offlineState, a.offlineSamples,
        a.offlineCompletedSamples, a.offlineBounces, a.offlineLightSamples,
        a.offlineSeed, a.offlineAov, a.offlineExposure, a.offlineError, a.offlineUiNotice) ==
        std::tie(b.offlineMode, b.offlineState, b.offlineSamples,
        b.offlineCompletedSamples, b.offlineBounces, b.offlineLightSamples,
        b.offlineSeed, b.offlineAov, b.offlineExposure, b.offlineError, b.offlineUiNotice);
}

inline bool SameTextureOptions(const std::vector<EditorTextureAssetSnapshot>& a,
    const std::vector<EditorTextureAssetSnapshot>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t index = 0; index < a.size(); ++index)
        if (a[index].assetPath != b[index].assetPath || a[index].valid != b[index].valid)
            return false;
    return true;
}

using Panel::ActorCountLabel;
using Panel::ActorPropertyRowMarkup;
using Panel::DocumentCommandIds;
using Panel::DocumentStatus;
using Panel::FileLabel;
using Panel::MaterialPanelId;
using Panel::Number;
using Panel::OutlinerActorRow;
using Panel::OutlinerSlotRow;
using Panel::PanelLabel;
using Panel::PreviewStatus;
using Panel::RenderStateOptions;
using Panel::ResourcePanelId;
using Panel::TerrainLayerCardMarkup;
using Panel::TerrainLayerColor;
using Panel::TerrainLayerFallbackViewportHeight;
using Panel::TerrainLayerFallbackViewportWidth;
using Panel::TerrainLayerGridCellWidth;
using Panel::TerrainLayerGridColumns;
using Panel::TerrainLayerGridRowHeight;
using Panel::TerrainLayerListRowHeight;
using Panel::TerrainLayerMatchesSearch;
using Panel::TerrainLayerRange;
using Panel::TerrainLayerSourceLabel;
using Panel::TerrainLayerVirtualizeThreshold;
using Panel::TerrainNumber;

inline int TerrainBrushShapeIndex(Editor::BrushShape shape)
{
    return shape == Editor::BrushShape::Square ? 1 : 0;
}

inline int TerrainBrushFalloffIndex(Editor::BrushFalloff falloff)
{
    return static_cast<int>(falloff);
}

inline float TerrainBrushFalloffValue(Editor::BrushFalloff falloff)
{
    // 当前地形核心只消费 0..1 的插值权重；Lut 尚未接入几何采样，
    // 复用它最接近的平滑权重，避免预设选中后参数实际不生效。
    switch (falloff)
    {
    case Editor::BrushFalloff::Linear: return 0.6f;
    case Editor::BrushFalloff::Constant: return 0.0f;
    case Editor::BrushFalloff::Smooth:
    case Editor::BrushFalloff::Lut:
    default: return 1.0f;
    }
}

inline bool TerrainBrushValueMatches(float value, float expected)
{
    return std::abs(value - expected) <=
        1.0e-5f * std::max(1.0f, std::abs(expected));
}

inline bool TerrainBrushValuesMatch(
    const Editor::BrushProfile& profile,
    float radiusCm,
    float strength,
    float falloff,
    int shape)
{
    return TerrainBrushValueMatches(radiusCm, profile.radiusCm) &&
        TerrainBrushValueMatches(strength, profile.strength) &&
        TerrainBrushValueMatches(falloff, TerrainBrushFalloffValue(profile.falloff)) &&
        shape == TerrainBrushShapeIndex(profile.shape);
}

inline bool TerrainSculptBrushMatches(
    const Editor::BrushProfile& profile,
    const TerrainSculptBrushState& state)
{
    return TerrainBrushValuesMatch(profile, state.radiusCm,
        state.strength, state.falloff, state.shape);
}

inline bool TerrainPaintBrushMatches(
    const Editor::PaintBrushPreset& preset,
    const TerrainPaintBrushState& state)
{
    return TerrainBrushValueMatches(state.radiusCm, preset.profile.radiusCm) &&
        TerrainBrushValueMatches(state.hardness, preset.hardness) &&
        TerrainBrushValueMatches(state.opacity, preset.opacity) &&
        TerrainBrushValueMatches(state.flow, preset.flow) &&
        TerrainBrushValueMatches(state.noise, preset.profile.noise) &&
        state.noiseSeed == preset.profile.noiseSeed &&
        state.shape == TerrainBrushShapeIndex(preset.profile.shape);
}

inline std::string MotionNumber(double value)
{
    std::ostringstream stream;
    stream << std::setprecision(4) << value;
    return stream.str();
}

inline std::string MotionQueryReadout(const std::vector<float>& values)
{
    std::string text;
    for (size_t i = 0; i < std::min(size_t(3), values.size()); ++i)
    {
        if (i) text += ", ";
        text += MotionNumber(values[i]);
    }
    return "[" + text + (values.size() > 3 ? ", ...]" : "]");
}

// 弹层位置按整像素写入：RmlUi 的 left/top 会参与布局缓存比较，抖动的小数会让静止帧反复失效。
inline std::string Pixels(float value)
{
    return std::to_string(std::lround(value)) + "px";
}

template<class T>
bool ParseNumber(const std::string& text, T& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc() &&
        result.ptr == text.data() + text.size() && std::isfinite(static_cast<double>(value));
}

inline BlueprintGizmoMode SceneGizmoMode(const std::string& tool)
{
    return tool == "rotate" ? BlueprintGizmoMode::Rotate :
        tool == "scale" ? BlueprintGizmoMode::Scale : BlueprintGizmoMode::Translate;
}

inline bool SceneGizmoSupportsActor(EditorSceneActorType type)
{
    // Environment/Wind 没有可写的 Actor Transform；其余场景对象都参与 UE 式
    // Move/Rotate/Scale 中的至少一种。SquareAreaLight 只有位置和朝向法线，
    // 不支持 authored scale/rotation，因此只显示平移 Gizmo。
    return type != EditorSceneActorType::Environment &&
        type != EditorSceneActorType::SceneWind;
}

inline bool SceneGizmoSupportsMode(EditorSceneActorType type, BlueprintGizmoMode mode)
{
    if (mode == BlueprintGizmoMode::Translate) return SceneGizmoSupportsActor(type);
    if (mode == BlueprintGizmoMode::Rotate)
        return SceneGizmoSupportsActor(type) && type != EditorSceneActorType::SquareAreaLight;
    return type == EditorSceneActorType::Mesh || type == EditorSceneActorType::Terrain ||
        type == EditorSceneActorType::Script;
}

inline Eigen::Quaternionf SceneGizmoRotation(const Eigen::Vector3f& degrees)
{
    // 与 World 的 YXZ 作者旋转约定一致；只用于 Local 轴方向，不改变 TRS 语义。
    const Eigen::Vector3f radians = degrees * 0.017453292519943295f;
    const Eigen::Quaternionf y(Eigen::AngleAxisf(radians.y(), Eigen::Vector3f::UnitY()));
    const Eigen::Quaternionf x(Eigen::AngleAxisf(radians.x(), Eigen::Vector3f::UnitX()));
    return y * x * Eigen::AngleAxisf(radians.z(), Eigen::Vector3f::UnitZ());
}

// 文本输入与范围控件都是 ElementFormControl，但取值路径不同：
// 范围控件的 GetValue() 走 RmlUi WidgetSlider 的 "%f" 序列化（6 位小数），
// 小于 1e-6 的拖动结果会被截成 0；精确值只保存在 value 属性里。
// 这里对范围控件直接读 value 属性，让两类控件都返回可解析的有限数，
// 也保证拖动不会落进 HandleChange 的 "A complete finite number is required." 分支。
inline std::string ControlValue(Rml::Element& element)
{
    auto* control = dynamic_cast<Rml::ElementFormControl*>(&element);
    if (control == nullptr) return std::string();
    auto* input = dynamic_cast<Rml::ElementFormControlInput*>(&element);
    if (input != nullptr && input->GetAttribute<Rml::String>("type", "") == "range")
        return Number(input->GetAttribute<float>("value", 0.0f));
    return control->GetValue();
}

using Panel::CameraExposureFieldEditable;
using Panel::DetailsSectionId;
using Panel::ActorSectionKey;

// 运行时命令驱动的数值只在真正变化时提交，与运行时页 QueueChangedFloatAction 的语义一致。
inline bool ChangedFloat(float value, float currentValue)
{
    return std::abs(value - currentValue) > 0.0001f;
}

// actor-property 的文本输入嵌在属性行里；点击行的空白/标签区域时把焦点交给
// 第一个文本输入，避免用户必须精确点中输入框才能编辑。
inline Rml::Element* FirstActorTextInput(Rml::Element& root)
{
    for (int index = 0; index < root.GetNumChildren(); ++index)
    {
        Rml::Element* child = root.GetChild(index);
        if (child == nullptr) continue;
        if (child->GetTagName() == "input" &&
            child->GetAttribute<Rml::String>("type", "") == "text")
            return child;
        if (Rml::Element* nested = FirstActorTextInput(*child)) return nested;
    }
    return nullptr;
}

using Panel::SunIntensityMax;
using Panel::SunIntensityMin;

inline EditorMaterialParameterValue CommandValue(const EditorParameterValue& value)
{
    switch (value.type)
    {
    case EditorParameterType::Float: return value.values[0];
    case EditorParameterType::Vec2: return EditorVec2{value.values[0], value.values[1]};
    case EditorParameterType::Vec3: return EditorVec3{value.values[0], value.values[1], value.values[2]};
    case EditorParameterType::Vec4: return EditorVec4{value.values[0], value.values[1], value.values[2], value.values[3]};
    case EditorParameterType::Color: return EditorColor{value.values};
    }
    return 0.0f;
}

inline EditorMaterialParameterType CommandType(EditorParameterType type)
{
    switch (type)
    {
    case EditorParameterType::Float: return EditorMaterialParameterType::Float;
    case EditorParameterType::Vec2: return EditorMaterialParameterType::Vec2;
    case EditorParameterType::Vec3: return EditorMaterialParameterType::Vec3;
    case EditorParameterType::Vec4: return EditorMaterialParameterType::Vec4;
    case EditorParameterType::Color: return EditorMaterialParameterType::Color;
    }
    return EditorMaterialParameterType::Float;
}

inline Editor::Selection::MaterialInstanceModelContext SelectionModel(
    const MaterialInstanceModelSnapshot& model)
{
    Editor::Selection::MaterialInstanceModelContext result;
    result.worldGeneration = model.worldGeneration;
    result.scenePath = model.scenePath;
    result.objectId = model.objectId;
    result.displayName = model.displayName;
    result.objectIdentity = model.objectIdentity;
    result.actorIdentity = model.actorIdentity;
    for (const auto& material : model.materials)
    {
        Editor::Selection::MaterialInstanceModelMaterial item;
        item.objectId = material.objectId == 0 ? model.objectId : material.objectId;
        item.materialSlotIndex = material.materialSlotIndex;
        item.materialSlotName = material.materialSlotName;
        item.materialInstancePath = material.materialInstancePath;
        item.displayName = material.materialSlotName;
        result.materials.push_back(std::move(item));
    }
    return result;
}


} // namespace VL::EditorUi::Panel::Detail

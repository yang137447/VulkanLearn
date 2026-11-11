#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "editor/ui/panel/editorPanelInternalHelpers.h"
#include "animation/motionDebug.h"
#include "debug/drawDebug.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <tuple>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>

#include "core/renderSession.h"
#include "editor/brush/brushProfile.h"
#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/editorTextMatch.h"
#include "editor/ui/editorViewportAxisIndicator.h"
#include "editor/ui/editorActorVisualization.h"
#include "editor/ui/scriptComponentControls.h"
#include "editor/ui/tressfxGroupEditor.h"
#include "hair/tressfxGroupBinding.h"
#include "editor/ui/tressfxHairSimulationControls.h"
#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxGroupPreviewState.h"
#include "editor/ui/scriptPhysicsControls.h"
#include "editor/ui/scriptPropertyControls.h"
#include "script/scriptTypeIdentity.h"
#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/layout/layoutWidgets.h"
#include "ui/widgets/navigation/tabWidgets.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"
#include "ui/uiColorTransform.h"

namespace VL::EditorUi
{
namespace
{
using namespace Panel::Detail;
} // namespace

// 选择态与"已挂载 actor 的可见性"合成一个重挂签名：两者都只改已挂载行的
// class/文本/眼睛图标，不改变行集合，但对 markup 是可见变化。
// 可见性按稳定身份生成，因此名字相同但换了对象（World 换代）时签名仍然变化。
std::string MaterialInstanceAssetEditorPanel::OutlinerMountedVisuals()
{
    return Panel::OutlinerMountedVisuals(OutlinerView());
}

void MaterialInstanceAssetEditorPanel::RefreshActorVisualization()
{
    Panel::RefreshActorVisualization(OutlinerView());
}

bool MaterialInstanceAssetEditorPanel::SceneEditingActive() const
{
    return runtimeView.scripts.state == ScriptPlayState::Editing &&
        !runtimeView.offlineMode;
}

void MaterialInstanceAssetEditorPanel::RefreshOutliner()
{
    Panel::RefreshOutlinerView(OutlinerView());
}

void MaterialInstanceAssetEditorPanel::RefreshOutlinerVisuals()
{
    Panel::RefreshOutlinerVisuals(OutlinerView());
}

bool MaterialInstanceAssetEditorPanel::OutlinerRowsNeedRebuild(
    const std::vector<EditorSceneActorSnapshot>& actors)
{
    return Panel::OutlinerRowsNeedRebuild(OutlinerView(), actors);
}

void MaterialInstanceAssetEditorPanel::RebuildOutlinerRows(
    const std::vector<EditorSceneActorSnapshot>& actors)
{
    Panel::RebuildOutlinerRows(OutlinerView(), actors);
}

EditorSceneActorWindow MaterialInstanceAssetEditorPanel::OutlinerWindow()
{
    return Panel::OutlinerWindow(OutlinerView());
}

// 树行高度由 RCSS 统一决定（.tree-row 的内容是 .tree-toggle 与 .list-item），
// 这里读已挂载行的 margin box 校正兜底值——正如 Content Browser 读首个可见条目。
// 首帧或刚替换的 DOM 还没有布局，读到 0 时保持上一次的值。
void MaterialInstanceAssetEditorPanel::MeasureOutlinerRowHeight(){
    Panel::MeasureOutlinerRowHeight(OutlinerView());
}

// 只把窗口内的行挂进 DOM，上下各一个 spacer 保留未挂载行的高度：
// 滚动范围因此与整棵树一致，"窗口化"不等于"截断到前 N 行"。
std::string MaterialInstanceAssetEditorPanel::OutlinerRowsMarkup(
    const EditorSceneActorWindow& window)
{
    return Panel::OutlinerRowsMarkup(outlinerRows, OutlinerActors(),
        snapshot.selectedActorIdentity, snapshot.selectedMaterialSlotIndex, window);
}

const std::vector<EditorSceneActorSnapshot>& MaterialInstanceAssetEditorPanel::OutlinerActors() const
{
    return Panel::OutlinerActors(snapshot);
}

const EditorSceneActorSnapshot* MaterialInstanceAssetEditorPanel::SelectedActor() const
{
    return Panel::FindSelectedActor(snapshot);
}

const EditorSceneActorSnapshot* MaterialInstanceAssetEditorPanel::PreviewWindActor() const
{
    return Panel::FindPreviewWindActor(snapshot);
}

void MaterialInstanceAssetEditorPanel::SelectOutlinerActor(const EditorSceneActorSnapshot& actor)
{
    if (sceneSelectionSink == nullptr) return;
    FlushPendingEdits();
    ShowSceneDetails();
    if (actor.type == EditorSceneActorType::Mesh ||
        actor.type == EditorSceneActorType::Terrain)
    {
        // Terrain 的 chunk 复用 mesh 的 section/材质槽语义，因此选择路由与 Mesh 相同：
        // 整模型选择 + 视口描边，不新增地形专属的资产编辑面板。
        sceneSelectionSink->RequestModelSelection(SelectionModel(MeshSceneActorModel(actor)));
        return;
    }
    sceneSelectionSink->RequestActorSelection(actor.identity);
}

void MaterialInstanceAssetEditorPanel::RefreshDetails()
{
    Panel::RefreshDetailsView(DetailsView());
}

std::string MaterialInstanceAssetEditorPanel::ActorSectionMarkup(
    const EditorSceneActorSnapshot& actor)
{
    return Panel::BuildActorSectionMarkup(actor, collapsedSections, runtimeView,
        [this](const EditorSceneActorSnapshot& value) { return SceneLogicComponentsMarkup(value); });
}

std::string MaterialInstanceAssetEditorPanel::PrimaryDirectionalLightControls(
    const EditorSceneActorSnapshot& actor) const
{
    return Panel::PrimaryDirectionalLightControls(actor, runtimeView);
}

// 作者字段与模式高亮都跟随冻结快照；焦点控件（正在拖动/输入）不会被回写。
void MaterialInstanceAssetEditorPanel::RefreshActorValues(const EditorSceneActorSnapshot* actor)
{
    if (actor == nullptr) return;
    for (const EditorSceneActorField& field : SceneActorFields(*actor))
        SetMarkup(field.id, Text(field.value));
    for (const auto& field : SceneActorEditableFields(*actor))
        for (uint32_t component = 0; component < field.components; ++component)
        {
            const std::string id = SceneActorPropertyControlId(field.property, component);
            const std::string number = SceneActorNumber(field.value[component]);
            SetControlValue(id, number);
            SetDisabled(id, runtimeView.scripts.state != ScriptPlayState::Editing);
            // 数值输入与滑条共享同一属性；只刷文本会让滑条的 value 属性停在
            // DOM 创建时的旧值，下一次点击/拖动就会用旧值覆盖刚输入的数值。
            if (field.hasRange)
            {
                SetControlValue(id + "-slider", number);
                SetDisabled(id + "-slider", runtimeView.scripts.state != ScriptPlayState::Editing);
            }
        }
    if (actor->type == EditorSceneActorType::Script)
    {
        const bool editing = runtimeView.scripts.state == ScriptPlayState::Editing &&
            !actor->scriptSpawned && !runtimeView.offlineMode;
        const bool inlineEdit = editing && actor->scriptBlueprintPath.empty();
        SetControlValue("script-blueprint-name", actor->displayName);
        for (const char* id : {"script-blueprint-name", "script-blueprint-duplicate", "script-blueprint-delete"})
            SetDisabled(id, !editing);
        for (const char* id : {"script-component-add", "script-component-kind", "script-component-mesh"})
            SetDisabled(id, !inlineEdit);
        const auto& physics = actor->scriptPhysics;
        const std::string motion = physics.is_null() ? "none" : physics.value("motion", "static");
        SetControlValue("script-physics-motion", motion);
        SetDisabled("script-physics-motion", !inlineEdit);
        widgetDocument.SetProperty("script-physics-body", "display", physics.is_null() ? "none" : "block");
        const std::string shape = physics.is_null() ? "box" : physics.value("shape", "box");
        SetControlValue("script-physics-shape", shape);
        SetDisabled("script-physics-shape", !inlineEdit || motion == "character");
        SetDisabled("script-physics-trigger", !inlineEdit || motion == "character");
        if (auto* trigger = document->GetElementById("script-physics-trigger"))
        {
            const bool checked = !physics.is_null() && physics.value("trigger", false);
            if (checked && !trigger->HasAttribute("checked")) trigger->SetAttribute("checked", "checked");
            else if (!checked && trigger->HasAttribute("checked")) trigger->RemoveAttribute("checked");
        }
        widgetDocument.SetProperty("script-physics-halfExtents-row", "display", shape == "box" ? "block" : "none");
        SetControlValue("script-physics-component", physics.is_object() ? physics.value("component", "") : "");
        SetDisabled("script-physics-component", !inlineEdit);
        widgetDocument.SetProperty("script-physics-component-row", "display", shape == "mesh" ? "block" : "none");
        RefreshScriptConstraintControls(physics.is_object() ? physics : nlohmann::json::object(),
            "script-", !inlineEdit || motion == "character");
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const auto id = "script-physics-extents-" + std::to_string(axis);
            SetControlValue(id, physics.contains("halfExtents") ? physics.at("halfExtents")[axis].dump() : "50");
            SetDisabled(id, !inlineEdit);
        }
        for (const auto& field : ScriptPhysicsFields())
        {
            const auto id = "script-physics-" + std::string(field.name);
            const float fallback = std::string(field.name) == "mass" && motion == "character" ? 70 : field.defaultValue;
            SetControlValue(id, physics.contains(field.name) ? physics.at(field.name).dump() : Number(fallback));
            SetDisabled(id, !inlineEdit);
            widgetDocument.SetProperty(id + "-row", "display",
                ScriptPhysicsFieldVisible(physics, field.name) ? "block" : "none");
        }
        for (size_t component = 0; component < actor->scriptComponents.size(); ++component)
        {
            const auto& node = actor->scriptComponents[component];
            const auto componentId = ScriptComponentId(node);
            RefreshScriptCollisionControls(component, node, "script-", !inlineEdit);
            const bool inherited = !actor->scriptBlueprintPath.empty();
            const auto parentId = ComponentControlId(component, "parent");
            SetControlValue(parentId, node.value("parent", ""));
            SetDisabled(parentId, !inlineEdit);
            SetDisabled("script-component-remove-" + std::to_string(component), !inlineEdit);
            for (const auto& field : ScriptComponentInstanceFields(node))
            {
                const bool allowed = !inherited ||
                    ScriptComponentAllowsInstance(actor->scriptComponentInstanceEditable, componentId, field);
                const auto value = ScriptComponentFieldValue(node, field);
                const bool overridden = actor->scriptComponentOverrides.contains(componentId) &&
                    actor->scriptComponentOverrides.at(componentId).contains(field);
                for (size_t axis = 0; axis < (value.is_array() ? value.size() : 1u); ++axis)
                {
                    const auto id = ComponentControlId(component, field, axis);
                    const auto& current = value.is_array() ? value[axis] : value;
                    SetControlValue(id, current.is_string() ? current.get<std::string>() : current.dump());
                    SetDisabled(id, !editing || !allowed);
                    widgetDocument.SetClass(id, "script-overridden", overridden);
                }
                SetDisabled(ComponentResetControlId(component, field), !editing || !allowed || !overridden);
            }
        }
        size_t index = 0;
        for (const auto& property : actor->scriptProperties.items())
        {
            const bool exposed = std::find(actor->scriptInstanceEditable.begin(),
                actor->scriptInstanceEditable.end(), property.key()) != actor->scriptInstanceEditable.end();
            const auto& value = property.value();
            const auto schema = ScriptPropertyField(runtimeView.scripts.classes,
                actor->scriptClass, property.key(), value);
            RefreshScriptPropertyControl(*document, widgetDocument, "script-property-" + std::to_string(index),
                {"script-property", index}, schema, value, !editing || !exposed,
                actor->scriptOverrides.contains(property.key()), nlohmann::json::array(),
                actor->scriptOverrides.value(property.key(), nlohmann::json(nullptr)));
            SetDisabled("script-reset-" + std::to_string(index), !editing || !exposed ||
                !actor->scriptOverrides.contains(property.key()));
            ++index;
        }
        SetDisabled("script-asset-create", runtimeView.scripts.state != ScriptPlayState::Editing);
        RefreshSceneLogicComponents(*actor);
    }
    if (actor->type == EditorSceneActorType::Camera)
    {
        const auto& exposure = actor->camera.exposure;
        // 模式来自作者快照；只切换行可见性，保留数值 DOM、焦点和原有 physical 配置。
        widgetDocument.SetClass("actor-exposure-manual", "selected", exposure.mode != ExposureMode::Auto);
        widgetDocument.SetClass("actor-exposure-auto", "selected", exposure.mode == ExposureMode::Auto);
        widgetDocument.SetProperty("actor-exposure-physical-row", "display",
            exposure.mode == ExposureMode::PhysicalCamera ? "block" : "none");
        SetMarkup("actor-exposure-physical-ev", Text(Number(static_cast<float>(EffectiveEv100(exposure)))));
        for (const auto& field : SceneActorEditableFields(*actor))
            if (CameraExposureField(field.property) != nullptr)
                widgetDocument.SetProperty(SceneActorPropertyControlId(field.property, 0) + "-row",
                    "display", CameraExposureFieldEditable(field.property, exposure.mode) ? "block" : "none");
    }
    if (!actor->primaryDirectionalLight) return;
    // 文本输入承载精确值；滑条只承载 0..10 的显示区间，越界值在这里按显示区间截断
    // （只影响滑条位置，不改变存储值）。拖动中的滑条是焦点元素，SetControlValue 不回写。
    const float intensity = actor->light.intensity;
    SetControlValue("actor-sun-intensity", SceneActorNumber(intensity));
    SetControlValue("actor-sun-intensity-slider",
        SceneActorNumber(std::min(std::max(intensity, SunIntensityMin), SunIntensityMax)));
    // CSM 是高亮来自冻结的运行时快照，不是面板自己的第二份状态。
    widgetDocument.SetClass("actor-csm-enable", "selected", runtimeView.csmCastShadows);
    widgetDocument.SetClass("actor-csm-disable", "selected", !runtimeView.csmCastShadows);
}

void MaterialInstanceAssetEditorPanel::ApplySectionState()
{
    Panel::ApplySectionStateView(widgetDocument, collapsedSections);
}

void MaterialInstanceAssetEditorPanel::ToggleSection(const std::string& key)
{
    if (key.empty()) return;
    if (collapsedSections.erase(key) == 0) collapsedSections.insert(key);
    ApplySectionState();
}

} // namespace VL::EditorUi

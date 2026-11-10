// HandleChange 的动作分发表；与动作表共用同一套数值 helper。
#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "editor/ui/panel/editorPanelInternalHelpers.h"
#include "editor/ui/panel/hairPanelHelpers.h"
#include "animation/motionDebug.h"
#include "debug/drawDebug.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <exception>
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
namespace Panel
{
using namespace Panel::Detail;
using namespace HairPanelDetail;


void HandlePanelChange(MaterialInstanceAssetEditorPanel& panel, Rml::Element& element,
    const std::string& action, const Rml::Event* event)
{
    if (panel.terrainAssetEditor.HandleChange(element, action)) return;
    if (action == "terrain-layer-search")
    {
        panel.terrainLayerSearch = ControlValue(element);
        panel.terrainLayerScrollResetPending = true;
        panel.terrainLayerWindowDirty = true;
        panel.RefreshTerrainLayerLibrary();
        return;
    }
    if (action == "brush-library-search")
    {
        panel.brushLibrary.SetQuery(ControlValue(element));
        panel.brushLibrary.Build();
        return;
    }
    if (action == "brush-library-category")
    {
        panel.brushLibrary.SetCategory(ControlValue(element));
        panel.brushLibrary.Build();
        return;
    }
    if (action == "hair-group-rename")
    {
        if (panel.hairGroupSelected < 0) return;
        std::string error;
        if (!panel.hairGroups.RenameGroup(static_cast<std::size_t>(panel.hairGroupSelected),
            ControlValue(element), error))
        {
            // 重命名失败只写状态，控制器里的当前草稿保持原值。
            panel.hairGroupStatus = error;
            return;
        }
        panel.hairPaintUndo.clear();
        panel.hairGroupStatus = "Group renamed in the asset draft. Save to Hair Asset to write it.";
        return;
    }
    if (action == "hair-brush-radius")
    {
        double value = 0.0;
        if (!ParseNumber(ControlValue(element), value))
        {
            panel.hairBrushStatus = "Brush radius must be a finite number of pixels.";
            return;
        }
        panel.hairBrushRadiusPx = static_cast<float>(std::clamp(value, 2.0, 500.0));
        panel.hairBrushStatus = "Brush radius: " +
            SceneActorNumber(panel.hairBrushRadiusPx) + " px (world radius follows cursor depth).";
        return;
    }
    if (action == "hair-brush-mode")
    {
        panel.hairBrushMode = ControlValue(element) == "remove"
            ? VL::Hair::GroupPaintMode::Remove : VL::Hair::GroupPaintMode::Add;
        panel.hairBrushStatus = panel.hairBrushMode == VL::Hair::GroupPaintMode::Remove
            ? "Brush mode: Remove selected guides from the target group."
            : "Brush mode: Add selected guides to the target group.";
        return;
    }
    if (action == "hair-brush-replace")
    {
        panel.hairBrushReplaceExisting = element.HasAttribute("checked");
        panel.hairBrushStatus = panel.hairBrushReplaceExisting
            ? "Replace existing is on: guides move from other groups."
            : "Replace existing is off: guides in other groups are skipped.";
        return;
    }
    if (action == "hair-group-create-field")
    {
        const std::size_t component = element.GetAttribute<std::size_t>("data-component", 0);
        const std::string value = ControlValue(element);
        if (component == 0) panel.hairGroupCreateName = value;
        else if (component == 1) panel.hairGroupCreateStart = value;
        else if (component == 2) panel.hairGroupCreateEnd = value;
        else return;
        panel.hairGroupCreateError.clear();
        return;
    }
    if (action == "hair-group-range-field")
    {
        const std::size_t component = element.GetAttribute<std::size_t>("data-component", 0);
        const std::string value = ControlValue(element);
        if (component == 0) panel.hairGroupRangeStart = value;
        else if (component == 1) panel.hairGroupRangeEnd = value;
        else return;
        panel.hairGroupRangeError.clear();
        return;
    }
    if (action == "hair-group-override-toggle")
    {
        if (panel.hairGroupSelected < 0) return;
        const std::string field = element.GetAttribute<std::string>("data-key", "");
        if (!VL::Editor::TressfxGroupEditor::IsAllowedOverrideField(field)) return;
        const std::size_t selected = static_cast<std::size_t>(panel.hairGroupSelected);
        if (!element.HasAttribute("checked"))
        {
            panel.hairGroups.SetOverride(selected, field, std::nullopt);
            panel.hairPaintUndo.clear();
            panel.hairGroupStatus = "Override removed; the group inherits the asset default.";
            return;
        }
        const auto* active = panel.resourceEditor.ActiveTab();
        const auto* document = active ? panel.TressfxDocument(active->path) : nullptr;
        const nlohmann::json simulation = document != nullptr && document->document.contains("simulationDefaults")
            ? document->document.at("simulationDefaults") : nlohmann::json::object();
        const EditorSceneMeshSectionSnapshot* assetSection = panel.HairBrushSection();
        const int32_t savedIndex = panel.SavedHairGroupIndex(assetSection, panel.hairGroupSelected);
        // 勾选 override 先从当前资产默认值起步，避免用一个隐藏的硬编码初值覆盖。
        if (!panel.hairGroups.SetOverride(selected, field, HairGroupAssetDefault(
                assetSection, savedIndex >= 0, savedIndex, simulation, field)))
        {
            panel.hairGroupStatus = "Unable to enable this override.";
            return;
        }
        panel.hairPaintUndo.clear();
        panel.hairGroupStatus = "Override enabled from the current asset default; edit the value or clear overrides.";
        return;
    }
    if (action == "hair-group-override-value")
    {
        if (panel.hairGroupSelected < 0) return;
        const std::string field = element.GetAttribute<std::string>("data-key", "");
        double value = 0.0;
        if (!ParseNumber(ControlValue(element), value))
        {
            panel.hairGroupStatus = "A complete finite number is required.";
            return;
        }
        if (!panel.hairGroups.SetOverride(static_cast<std::size_t>(panel.hairGroupSelected), field, value))
        {
            panel.hairGroupStatus = "This field cannot be overridden per group.";
            return;
        }
        panel.hairPaintUndo.clear();
        panel.hairGroupStatus = "Override edited in the asset draft.";
        return;
    }
    if (action == "hair-preview-wind" && panel.runtimeActionSink != nullptr)
    {
        if (!panel.IsActiveHairPreview() || panel.runtimeView.offlineMode) return;
        const auto* wind = panel.PreviewWindActor();
        if (wind == nullptr) return;
        float number = 0.0f;
        if (!ParseNumber(ControlValue(element), number))
        {
            panel.localStatus = "A complete finite number is required.";
            return;
        }
        const std::size_t index = element.GetAttribute<std::size_t>("data-index", 0);
        const uint32_t component = element.GetAttribute<uint32_t>("data-component", 0);
        EditorActorProperty property = EditorActorProperty::WindStrength;
        if (index == 0)
        {
            if (component >= 3) return;
            property = EditorActorProperty::WindDirection;
        }
        else if (index == 1) property = EditorActorProperty::WindStrength;
        else if (index == 2) property = EditorActorProperty::WindGustAmplitude;
        else return;
        if (panel.hairPreviewWindBound)
        {
            // 本地即时值优先于晚一帧到达的 actor 快照，拖动时滑块不会被旧值拉回。
            if (index == 0) panel.hairPreviewWind.direction[component] = number;
            else if (index == 1) panel.hairPreviewWind.strengthCmPerSec = number;
            else panel.hairPreviewWind.gustAmplitude = number;
        }
        UiAction request;
        request.type = UiActionType::SetActorProperty;
        request.actorProperty = {wind->worldGeneration, wind->identity, property, component, number};
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        return;
    }
    if (action == "hair-sim")
    {
        // 拖动/输入只更新会话值并投递实时覆盖；保存是独立动作，不会自动写资产。
        std::size_t count = 0;
        const TressfxSimulationControl* controls = TressfxSimulationControls(count);
        const std::size_t index = element.GetAttribute<size_t>("data-index", 0);
        const std::size_t component = element.GetAttribute<size_t>("data-component", 0);
        if (index >= count || component >= controls[index].components) return;
        // WidgetSlider 先发 change(value)，返回后才写 value 属性；读属性会落后一拍，
        // 下一帧同步又把滑块拉回旧值。这里只消费该事件的新值，文本仍保留半成品输入。
        const bool sliderChange = event != nullptr &&
            element.GetAttribute<Rml::String>("type", "") == "range" &&
            event->GetParameters().count("value") != 0;
        const std::string text = sliderChange
            ? Number(event->GetParameter<float>("value", 0.0f)) : ControlValue(element);
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        // 文本控件的半成品输入不能当成 0；只接受整段都是数字的值。
        if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) return;
        auto values = PanelSimulationValue(panel.hairSimulationValues, controls[index]);
        values[component] = value;
        panel.hairSimulationValues[controls[index].field] =
            nlohmann::json::array({values[0], values[1], values[2]});
        const auto* active = panel.resourceEditor.ActiveTab();
        const auto* authored = active ? panel.TressfxDocument(active->path) : nullptr;
        panel.hairSimulationStatus = authored && TressfxGpuSimulationDefaults(authored->document)
            ? "Live preview in this viewport (not saved)." : "Default value edited (not saved).";
        panel.SubmitHairSimulationLive(index, values);
        return;
    }
    if (action.rfind("debug-draw-", 0) == 0)
    {
        if (!panel.runtimeActionSink || !panel.runtimeView.debugDraw || !panel.runtimeView.motionWorldGeneration || panel.runtimeView.offlineMode) return;
        UiAction request; request.type = UiActionType::ControlDebugDraw;
        request.previewCamera.worldGeneration = panel.runtimeView.motionWorldGeneration;
        auto& control = request.debugDrawControl;
        control.enabled = element.HasAttribute("checked");
        if (action == "debug-draw-enabled")
        {
            if (control.enabled == panel.runtimeView.debugDraw->enabled) return;
            control.action = Debug::ControlAction::Enabled;
        }
        else if (action == "debug-draw-owner")
        {
            control.action = Debug::ControlAction::Owner; control.name = ControlValue(element);
            // RmlUi 可在后续 Update 才发布 options 初始化的 change；相同快照值不是用户修改。
            if (control.name == panel.runtimeView.debugDraw->ownerFilter) return;
        }
        else if (action == "debug-draw-category")
        {
            const size_t index = element.GetAttribute<size_t>("data-index", 0);
            if (index >= panel.runtimeView.debugDraw->categories.size()) return;
            if (control.enabled == panel.runtimeView.debugDraw->categories[index].enabled) return;
            control.action = Debug::ControlAction::Category; control.name = panel.runtimeView.debugDraw->categories[index].name;
        }
        else return;
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        return;
    }
    if (action == "motion-debug")
    {
        if (panel.runtimeActionSink && panel.runtimeView.motionWorldGeneration && !panel.runtimeView.offlineMode)
        {
            UiAction request;
            request.type = UiActionType::ControlMotionDebug;
            request.previewCamera.worldGeneration = panel.runtimeView.motionWorldGeneration;
            request.intValue = element.HasAttribute("checked") ? 1 : 0;
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    if (action == "motion-replay-seek")
    {
        if (panel.runtimeActionSink && panel.runtimeView.motionDebug && panel.runtimeView.motionWorldGeneration &&
            !panel.runtimeView.offlineMode && panel.runtimeView.motionDebug->replayFrames > 0)
        {
            int frame = 0;
            try { frame = std::stoi(ControlValue(element)); }
            catch (...) { return; }
            UiAction request;
            request.type = UiActionType::ControlMotionReplay;
            request.previewCamera.worldGeneration = panel.runtimeView.motionWorldGeneration;
            request.intValue = std::clamp(frame, 0, panel.runtimeView.motionDebug->replayFrames - 1);
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    if (action == "motion-timeline-play" || action == "motion-timeline-pause" ||
        action == "motion-timeline-seek")
    {
        if (panel.runtimeActionSink && panel.runtimeView.motionDebug && panel.runtimeView.motionWorldGeneration &&
            !panel.runtimeView.offlineMode && panel.runtimeView.motionDebug->playbackDuration > 0)
        {
            UiAction request;
            request.type = UiActionType::ControlScriptAnimation;
            request.previewCamera.worldGeneration = panel.runtimeView.motionWorldGeneration;
            request.stringValue = panel.runtimeView.motionDebug->componentIdentity;
            if (action == "motion-timeline-seek")
            {
                request.intValue = 0;
                try { request.floatValue = std::stof(ControlValue(element)); }
                catch (...) { return; }
            }
            else
            {
                request.intValue = 1;
                request.floatValue = action == "motion-timeline-play" ? 1.0f : 0.0f;
            }
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    if (action == "animation-clip" || action == "animation-time")
    {
        panel.HandleClick(element, action);
        return;
    }
    const size_t index = element.GetAttribute<size_t>("data-index", 0);
    const std::string value = ControlValue(element);
    panel.localStatus.clear();
    if (action == "viewport-snap-enabled")
    {
        panel.sceneSnapEnabled = element.HasAttribute("checked");
        panel.RefreshViewportToolbar();
        return;
    }
    if (action == "viewport-snap-move" || action == "viewport-snap-rotate" ||
        action == "viewport-snap-scale")
    {
        float number = 0.0f;
        if (!ParseNumber(value, number) || number <= 0.0f)
        {
            panel.localStatus = "Snap values must be finite and greater than zero.";
            panel.RefreshViewportToolbar();
            return;
        }
        if (action == "viewport-snap-move") panel.sceneMoveSnap = number;
        else if (action == "viewport-snap-rotate") panel.sceneRotateSnap = number;
        else panel.sceneScaleSnap = number;
        panel.RefreshViewportToolbar();
        return;
    }
    if (action == "viewport-brush-radius" || action == "viewport-brush-strength" ||
        action == "viewport-brush-falloff")
    {
        float number = 0.0f;
        if (!ParseNumber(value, number))
        {
            panel.localStatus = "Brush values must be finite numbers.";
            panel.RefreshTerrainBrushUi();
            return;
        }
        if (action == "viewport-brush-radius")
        {
            if (number <= 0.0f)
            {
                panel.localStatus = "Sculpt brush radius must be greater than zero.";
                panel.RefreshTerrainBrushUi();
                return;
            }
            panel.terrainSculptBrush.radiusCm = number;
        }
        else if (action == "viewport-brush-strength")
        {
            if (number <= 0.0f || number > 1.0f)
            {
                panel.localStatus = "Sculpt brush strength must be in (0, 1].";
                panel.RefreshTerrainBrushUi();
                return;
            }
            panel.terrainSculptBrush.strength = number;
        }
        else
        {
            if (number < 0.0f || number > 1.0f)
            {
                panel.localStatus = "Sculpt brush falloff must be in [0, 1].";
                panel.RefreshTerrainBrushUi();
                return;
            }
            panel.terrainSculptBrush.falloff = number;
        }
        panel.SynchronizeSculptBrushPresetId();
        panel.RefreshTerrainBrushUi();
        return;
    }
    if (action == "terrain-paint-radius" || action == "terrain-paint-hardness" ||
        action == "terrain-paint-opacity" || action == "terrain-paint-flow")
    {
        float number = 0.0f;
        if (!ParseNumber(value, number))
        {
            panel.localStatus = "Paint brush values must be finite numbers.";
            panel.RefreshTerrainBrushUi();
            return;
        }
        if (action == "terrain-paint-radius")
        {
            if (number <= 0.0f)
            {
                panel.localStatus = "Paint brush radius must be greater than zero.";
                panel.RefreshTerrainBrushUi();
                return;
            }
            panel.terrainPaintBrush.radiusCm = number;
        }
        else
        {
            if (number < 0.0f || number > 1.0f)
            {
                panel.localStatus = action == "terrain-paint-hardness"
                    ? "Paint hardness must be in [0, 1]."
                    : action == "terrain-paint-opacity"
                    ? "Paint opacity must be in [0, 1]."
                    : "Paint flow must be in [0, 1].";
                panel.RefreshTerrainBrushUi();
                return;
            }
            if (action == "terrain-paint-hardness") panel.terrainPaintBrush.hardness = number;
            else if (action == "terrain-paint-opacity") panel.terrainPaintBrush.opacity = number;
            else panel.terrainPaintBrush.flow = number;
        }
        panel.SynchronizePaintBrushPresetId();
        panel.RefreshTerrainBrushUi();
        return;
    }
    if (action.rfind("blueprint-asset-", 0) == 0) panel.HandleBlueprintAssetEvent(element, action, false);
    else if (panel.HandleSceneLogicEvent(element, action, false)) {}
    else if (panel.HandleSceneCompositePropertyEvent(element, action, false)) {}
    else if (action == "script-placement-mode")
    {
        if (value != "place" && value != "create") return;
        panel.scriptPlacementCreateAsset = value == "create";
        panel.RefreshScriptControls();
    }
    else if (action == "script-time-scale")
    {
        if (!panel.runtimeActionSink || !panel.runtimeView.scripts.available || panel.runtimeView.offlineMode ||
            panel.IsAssetEditorVisible() || panel.HasModal()) return;
        float scale;
        if (!ParseNumber(value, scale) || scale < 0 || scale > 100)
        { panel.localStatus = "Time Scale must be a finite number from 0 to 100."; return; }
        if (scale == panel.runtimeView.scripts.timeScale) return;
        UiAction request;
        request.type = UiActionType::SetScriptTimeScale;
        request.floatValue = scale;
        request.actorProperty.worldGeneration = panel.runtimeView.scripts.worldGeneration;
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "script-blueprint-name")
    {
        const auto* actor = panel.SelectedActor();
        if (actor && value != actor->displayName) panel.RequestScriptEdit(UiActionType::RenameScriptBlueprint, {{"name", value}});
    }
    else if (action.rfind("script-physics-", 0) == 0)
    {
        const auto field = action.substr(15);
        const auto* actor = panel.SelectedActor();
        if (!actor) return;
        nlohmann::json payload{{"field", field}};
        if (field == "motion" || field == "shape" || field == "component" ||
            field == "constraint-type" || field == "constraint-target") payload["value"] = value;
        else if (field == "constraint-axis") payload["value"] = nlohmann::json::parse(value);
        else if (field == "trigger") payload["value"] = element.HasAttribute("checked");
        else
        {
            float number;
            if (!ParseNumber(value, number)) { panel.localStatus = "A complete finite number is required."; return; }
            payload["value"] = number;
            if (field == "halfExtents" || field == "constraint-anchor")
                payload["axis"] = element.GetAttribute<size_t>("data-component", 0);
        }
        if (field.rfind("constraint-", 0) == 0) payload["index"] = index;
        panel.RequestScriptEdit(UiActionType::SetScriptPhysics, std::move(payload));
    }
    else if (action.rfind("script-component-", 0) == 0 && panel.runtimeActionSink != nullptr)
    {
        const auto* actor = panel.SelectedActor();
        if (!actor || actor->type != EditorSceneActorType::Script ||
            index >= actor->scriptComponents.size() ||
            actor->scriptSpawned || panel.runtimeView.scripts.state != ScriptPlayState::Editing) return;
        const auto field = action.substr(17);
        const auto& node = actor->scriptComponents[index];
        // 资产拥有的毛发字段不允许从关卡写：即使旧控件或脚本仍发出事件也直接丢弃。
        // 绑定/权重只在蓝图资产页作者，关卡只能改放置与资产白名单授权的实例覆写。
        if (node.value("type", "") == "tressfx" &&
            (field == "modelPath" || field == "rootBone" || field == "weights")) return;
        if (field.rfind("collision", 0) == 0 && field != "collisionMask")
        {
            if (!actor->scriptBlueprintPath.empty()) return;
            nlohmann::json payload{{"component", ScriptComponentId(node)}, {"field", field}};
            if (field == "collisionShape") payload["value"] = value;
            else
            {
                float number;
                if (!ParseNumber(value, number)) return;
                payload["value"] = number;
                if (field == "collisionHalfExtents")
                    payload["axis"] = element.GetAttribute<size_t>("data-component", 0);
            }
            panel.RequestScriptEdit(UiActionType::SetScriptComponent, std::move(payload));
            return;
        }
        if (!actor->scriptBlueprintPath.empty() && !ScriptComponentAllowsInstance(
            actor->scriptComponentInstanceEditable, ScriptComponentId(node), field)) return;
        nlohmann::json candidate;
        if (field == "parent" || field == "modelPath" || field == "materialOverride")
        {
            if (node.value(field, "") == value) return;
            candidate = value;
        }
        else if (field == "autoActivate" || field == "visible" || field == "doCollisionTest" ||
            field == "enableCameraLag" || field == "enableCameraRotationLag" || field == "usePawnControlRotation" ||
            field == "inheritPitch" || field == "inheritYaw" || field == "inheritRoll" ||
            field == "absoluteRotation" || field == "drawDebug" || field == "useCameraLagSubstepping") candidate = value == "true";
        else if (field == "fov" || field == "near_clip" || field == "far_clip" || field == "priority" ||
            field == "targetArmLength" || field == "probeRadius" || field == "collisionMask" ||
            field == "cameraLagSpeed" || field == "cameraLagMaxDistance" || field == "cameraRotationLagSpeed" ||
            field == "cameraLagMaxTimeStep" || IsScriptLightNumber(field))
        {
            float number;
            if (!ParseNumber(value, number)) return;
            candidate = number;
        }
        else
        {
            float number;
            if ((field != "position" && field != "rotation" && field != "scale" && field != "color" &&
                 field != "socketOffset" && field != "rotationOffset" && field != "targetOffset") || !ParseNumber(value, number)) return;
            const auto axis = element.GetAttribute<size_t>("data-component", 0);
            if (axis >= 3) return;
            const int fallback = field == "scale" || field == "color" ? 1 : 0;
            candidate = node.value(field, nlohmann::json::array({fallback, fallback, fallback}));
            if (candidate[axis] == number) return;
            candidate[axis] = number;
        }
        if (node.contains(field) && node.at(field) == candidate) return;
        UiAction request;
        request.type = UiActionType::SetScriptComponent;
        request.actorProperty.worldGeneration = actor->worldGeneration;
        request.actorProperty.identity = actor->identity;
        request.script = {{"component", node.value("id", "root")}, {"field", field}, {"value", candidate}};
        if (candidate.is_array())
        {
            const auto axis = element.GetAttribute<size_t>("data-component", 0);
            request.script["axis"] = axis;
            request.script["value"] = candidate[axis];
        }
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "script-property" && panel.runtimeActionSink != nullptr)
    {
        const auto* actor = panel.SelectedActor();
        if (!actor || actor->type != EditorSceneActorType::Script ||
            index >= actor->scriptProperties.size() || panel.runtimeView.scripts.state != ScriptPlayState::Editing) return;
        auto property = actor->scriptProperties.begin();
        std::advance(property, index);
        if (std::find(actor->scriptInstanceEditable.begin(), actor->scriptInstanceEditable.end(),
            property.key()) == actor->scriptInstanceEditable.end()) return;
        try
        {
            const auto schema = ScriptPropertyField(panel.runtimeView.scripts.classes,
                actor->scriptClass, property.key(), property.value());
            const bool vector = ScriptPropertyUsesComponents(schema, property.value());
            const auto axis = element.GetAttribute<size_t>("data-component", 0);
            if (vector && axis >= property.value().size()) return;
            nlohmann::json candidate;
            if (schema.at("type") == "quaternion")
            {
                if (!ParseScriptQuaternion(value, candidate)) return;
            }
            else candidate = property.value().is_string() ? nlohmann::json(value)
                : nlohmann::json::parse(value);
            if (candidate == (vector ? property.value()[axis] : property.value())) return;
            UiAction request;
            request.type = UiActionType::SetScriptProperty;
            request.actorProperty.worldGeneration = actor->worldGeneration;
            request.actorProperty.identity = actor->identity;
            request.script = {{"property", property.key()}, {"value", candidate}};
            if (vector) request.script["axis"] = axis;
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        catch (const nlohmann::json::exception&) { panel.localStatus = "A complete property value is required."; }
    }
    else if (action == "save-file" && index < panel.saveRows.size() && panel.saveWaiting.empty())
    {
        const auto& path = panel.saveRows[index].assetPath;
        if (element.HasAttribute("checked")) panel.saveChecked.insert(path);
        else panel.saveChecked.erase(path);
        panel.RefreshSaveDialog();
    }
    else if (action == "outliner-search")
    {
        // 搜索只改变可见性，不提交业务命令；折叠规则与 Content Browser 共用。
        panel.outlinerQuery = value;
    }
    else if (action == "outliner-type")
    {
        panel.outlinerTypeFilter = value.empty() ? "all" : value;
    }
    else if (action == "outliner-sort")
    {
        // 排序只改变组内顺序；分组顺序、选中态与展开集合都不参与，因此也不被重排。
        SceneActorSortOrder order = panel.outlinerSortOrder;
        if (ParseSceneActorSortOrder(value, order)) panel.outlinerSortOrder = order;
    }
    else if (action == "actor-property" && panel.runtimeActionSink != nullptr)
    {
        const auto* actor = panel.SelectedActor();
        float number = 0.0f;
        if (actor == nullptr || !ParseNumber(value, number))
        {
            panel.localStatus = "A complete finite number is required.";
            return;
        }
        const auto property = static_cast<EditorActorProperty>(index);
        const uint32_t component = element.GetAttribute<uint32_t>("data-component", 0);
        for (const auto& field : SceneActorEditableFields(*actor))
        {
            if (field.property != property || component >= field.components) continue;
            if (const char* exposureField = CameraExposureField(property))
            {
                if (!CameraExposureFieldEditable(property, actor->camera.exposure.mode)) return;
                try
                {
                    // UI 给出即时错误；GT 仍按当时的完整作者状态校验，避免连续事件互相覆盖。
                    EditCameraExposure(actor->camera.exposure, exposureField, number);
                }
                catch (const std::invalid_argument& error)
                {
                    panel.localStatus = error.what();
                    return;
                }
            }
            if (number == field.value[component]) return;
            UiAction request;
            request.type = UiActionType::SetActorProperty;
            request.actorProperty = {actor->worldGeneration, actor->identity, property, component, number};
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
            return;
        }
    }
    else if (action == "actor-sun-intensity" && panel.runtimeActionSink != nullptr)
    {

        // 作者属性带 actor 身份和 World 代数；console 的 sun 调试动作仍独立保留。
        float number = 0.0f;
        if (!ParseNumber(value, number))
        {
            panel.localStatus = "A complete finite number is required.";
            return;
        }
        // 沿用主方向光控件的 lux 范围，精确输入与拖动使用同一编辑边界。
        if (number < SunIntensityMin || number > SunIntensityMax)
        {
            panel.localStatus = "Sun intensity must be between " + SceneActorNumber(SunIntensityMin) +
                " and " + SceneActorNumber(SunIntensityMax) + ".";
            return;
        }
        const EditorSceneActorSnapshot* actor = panel.SelectedActor();
        if (actor == nullptr || !ChangedFloat(number, actor->light.intensity)) return;
        UiAction request;
        request.type = UiActionType::SetActorProperty;
        request.actorProperty = {actor->worldGeneration, actor->identity,
            EditorActorProperty::Intensity, 0, number};
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "parameter" && panel.snapshot.activeDocument &&
        index < panel.snapshot.activeDocument->parameters.size())
    {
        const auto& parameter = panel.snapshot.activeDocument->parameters[index];
        const uint32_t component = element.GetAttribute<uint32_t>("data-component", 0);
        float number = 0.0f;
        if (component >= parameter.effectiveValue.ComponentCount() || !ParseNumber(value, number))
        {
            panel.localStatus = "A complete finite number is required.";
            return;
        }
        const auto previous = panel.pendingValues.find(parameter.name);
        EditorParameterValue candidate = previous == panel.pendingValues.end()
            ? parameter.effectiveValue : previous->second;
        candidate.values[component] = number;
        panel.pendingValues[parameter.name] = candidate;
        panel.QueueEdit("parameter:" + parameter.name, {EditorCommandType::SetMaterialParameterOverride,
            SetMaterialParameterOverridePayload{panel.snapshot.activeDocument->assetPath,
                parameter.name, CommandType(candidate.type), CommandValue(candidate)}});
    }
    else if (action == "state" && panel.snapshot.activeDocument && index < panel.snapshot.activeDocument->renderStates.size())
    {
        const auto& state = panel.snapshot.activeDocument->renderStates[index];
        const auto field = ParseEditorMaterialRenderStateField(state.name);
        EditorMaterialRenderStateValue candidate;
        switch (field)
        {
        case EditorMaterialRenderStateField::RenderMode: candidate = ParseEditorMaterialRenderMode(value); break;
        case EditorMaterialRenderStateField::CullMode: candidate = ParseEditorMaterialCullMode(value); break;
        case EditorMaterialRenderStateField::ShadingModel: candidate = ParseEditorMaterialShadingModel(value); break;
        }
        panel.QueueEdit("state:" + state.name, {EditorCommandType::SetMaterialRenderStateOverride,
            SetMaterialRenderStateOverridePayload{panel.snapshot.activeDocument->assetPath, field, candidate}});
    }
    else if (action == "texture" && panel.snapshot.activeDocument && index < panel.snapshot.activeDocument->textures.size())
    {
        const auto& texture = panel.snapshot.activeDocument->textures[index];
        panel.QueueEdit("texture:" + texture.slotName, {EditorCommandType::SetMaterialTextureOverride,
            SetMaterialTextureOverridePayload{panel.snapshot.activeDocument->assetPath, texture.slotName, value}});
    }
    else if (panel.runtimeActionSink != nullptr)
    {
        UiAction request;
        if (action == "offline-setting")
        {
            int number = 0;
            const std::string field = element.GetAttribute<std::string>("data-field", "");
            if (!ParseNumber(value, number))
            {
                panel.localStatus = "A complete integer is required.";
                return;
            }
            if (const char* error = OfflineSettingValidationError(field, number))
            {
                panel.localStatus = error;
                return;
            }
            request.type = UiActionType::SetOfflineSetting;
            request.stringValue = field;
            request.intValue = number;
        }
        else if (action == "offline-aov")
        {
            int mode = 0;
            if (!ParseNumber(value, mode) || mode < 0 || mode > 2) return;
            request.type = UiActionType::SetOfflineAov;
            request.intValue = mode;
        }
        else if (action == "offline-exposure")
        {
            float exposure = 0.0f;
            if (!ParseNumber(value, exposure) || exposure <= 0.0f)
            {
                panel.localStatus = "Exposure must be a positive finite number.";
                return;
            }
            request.type = UiActionType::SetOfflineExposure;
            request.floatValue = exposure;
        }
        else return;
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
}


} // namespace Panel
} // namespace VL::EditorUi


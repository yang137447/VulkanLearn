// HandleClick 的动作分发表；状态仍由 MaterialInstanceAssetEditorPanel 独占。
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


void HandlePanelClick(MaterialInstanceAssetEditorPanel& panel, Rml::Element& element,
    const std::string& action)
{
    if (panel.terrainAssetEditor.HandleClick(element, action)) return;
    if (action == "terrain-asset-save" || action == "terrain-asset-revert")
    {
        const auto* tab = panel.resourceEditor.ActiveTab();
        const auto* terrain = tab && tab->kind == EditorAssetKind::Terrain
            ? panel.TerrainDocument(tab->path) : nullptr;
        if (terrain == nullptr) return;
        EditorCommandEnvelope command;
        command.type = action == "terrain-asset-save"
            ? EditorCommandType::SaveTerrainDocument
            : EditorCommandType::RevertTerrainDocument;
        command.expectedDocumentRevision = terrain->documentRevision;
        command.payload = MaterialInstanceAssetPathPayload{tab->path};
        panel.Submit(std::move(command));
        return;
    }
    if (action == "motion-timeline-play" || action == "motion-timeline-pause")
    {
        if (panel.runtimeActionSink && panel.runtimeView.motionDebug && panel.runtimeView.motionWorldGeneration &&
            !panel.runtimeView.offlineMode && panel.runtimeView.motionDebug->playbackDuration > 0)
        {
            if ((action == "motion-timeline-play" && panel.runtimeView.motionDebug->playbackPlaying) ||
                (action == "motion-timeline-pause" && !panel.runtimeView.motionDebug->playbackPlaying)) return;
            UiAction request;
            request.type = UiActionType::ControlScriptAnimation;
            request.previewCamera.worldGeneration = panel.runtimeView.motionWorldGeneration;
            request.stringValue = panel.runtimeView.motionDebug->componentIdentity;
            request.intValue = 1;
            request.floatValue = action == "motion-timeline-play" ? 1.0f : 0.0f;
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    if (action == "motion-replay")
    {
        if (panel.runtimeActionSink && panel.runtimeView.motionDebug && panel.runtimeView.motionWorldGeneration &&
            !panel.runtimeView.offlineMode && panel.runtimeView.motionDebug->replayFrames > 0)
        {
            UiAction request;
            request.type = UiActionType::ControlMotionReplay;
            request.previewCamera.worldGeneration = panel.runtimeView.motionWorldGeneration;
            request.intValue = element.HasAttribute("checked") ?
                std::max(0, panel.runtimeView.motionDebug->replayFrames - 1) : -1;
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
        return;
    }
    const size_t index = element.GetAttribute<size_t>("data-index", 0);
    if (action == "motion-query-toggle")
    {
        panel.motionMatchingQueryExpanded = !panel.motionMatchingQueryExpanded;
        panel.widgetDocument.SetClass("viewport-motion-matching", "query-expanded", panel.motionMatchingQueryExpanded);
        panel.widgetDocument.SetClass("motion-query-toggle", "active", panel.motionMatchingQueryExpanded);
    }
    else if (action == "debug-draw-clear")
    {
        if (panel.runtimeActionSink && panel.runtimeView.debugDraw && panel.runtimeView.motionWorldGeneration && !panel.runtimeView.offlineMode)
        {
            UiAction request; request.type = UiActionType::ControlDebugDraw;
            request.previewCamera.worldGeneration = panel.runtimeView.motionWorldGeneration;
            request.debugDrawControl.action = Debug::ControlAction::ClearAll;
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
    }
    else if (action.rfind("blueprint-asset-", 0) == 0) panel.HandleBlueprintAssetEvent(element, action, true);
    else if (panel.HandleSceneLogicEvent(element, action, true)) {}
    else if (panel.HandleSceneCompositePropertyEvent(element, action, true)) {}
    else if (action == "script-physics-constraint-add" || action == "script-physics-constraint-remove")
    {
        const auto* actor = panel.SelectedActor();
        if (!actor || !actor->scriptBlueprintPath.empty()) return;
        if (action == "script-physics-constraint-add")
        {
            auto* target = panel.document->GetElementById("script-physics-constraint-target");
            const auto value = target ? ControlValue(*target) : "";
            if (value.empty()) { panel.localStatus = "A target Blueprint ID is required."; return; }
            panel.RequestScriptEdit(UiActionType::SetScriptPhysics, {{"field", "constraint-add"}, {"value", value}});
        }
        else panel.RequestScriptEdit(UiActionType::SetScriptPhysics,
            {{"field", "constraint-remove"}, {"value", index}});
    }
    else if (action == "workspace-undo" || action == "workspace-redo" ||
        action == "scene-undo" || action == "scene-redo")
        panel.RequestUndoRedo(action == "workspace-redo" || action == "scene-redo");
    else if (action == "script-blueprint-duplicate") panel.RequestScriptEdit(UiActionType::DuplicateScriptBlueprint);
    else if (action == "script-blueprint-delete") panel.RequestScriptEdit(UiActionType::DeleteScriptBlueprint);
    else if (action == "script-component-add")
    {
        const auto kind = ControlValue(*panel.document->GetElementById("script-component-kind"));
        nlohmann::json node = NewScriptComponent(kind);
        if (kind == "mesh") node["modelPath"] = ControlValue(*panel.document->GetElementById("script-component-mesh"));
        panel.RequestScriptEdit(UiActionType::AddScriptComponent, {{"node", node}});
    }
    else if (action == "script-component-remove")
    {
        const auto* actor = panel.SelectedActor();
        if (actor && index < actor->scriptComponents.size())
            panel.RequestScriptEdit(UiActionType::RemoveScriptComponent,
                {{"component", actor->scriptComponents[index].value("id", "root")}});
    }
    else if (action == "script-component-reset")
    {
        const auto* actor = panel.SelectedActor();
        if (!actor || index >= actor->scriptComponents.size() || actor->scriptBlueprintPath.empty()) return;
        const auto component = ScriptComponentId(actor->scriptComponents[index]);
        const auto field = element.GetAttribute<std::string>("data-field", "");
        if (!ScriptComponentAllowsInstance(actor->scriptComponentInstanceEditable, component, field) ||
            !actor->scriptComponentOverrides.contains(component) ||
            !actor->scriptComponentOverrides.at(component).contains(field)) return;
        panel.RequestScriptEdit(UiActionType::SetScriptComponent,
            {{"component", component}, {"field", field}, {"reset", true}});
    }
    else if (action == "script-placement-toggle")
    {
        panel.scriptPlacementVisible = !panel.scriptPlacementVisible;
        panel.RefreshScriptControls();
    }
    else if ((action == "script-compile" || action == "script-auto") && panel.runtimeActionSink)
    {
        if (!panel.runtimeView.scripts.available || !panel.runtimeView.scripts.buildAvailable || panel.runtimeView.offlineMode ||
            panel.IsAssetEditorVisible() || panel.HasModal()) return;
        if (action == "script-compile" && panel.runtimeView.scripts.buildBusy) return;
        UiAction request;
        request.type = action == "script-compile" ? UiActionType::CompileScripts : UiActionType::SetAutoCompileScripts;
        request.intValue = panel.runtimeView.scripts.autoCompile ? 0 : 1;
        request.actorProperty.worldGeneration = panel.runtimeView.scripts.worldGeneration;
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if ((action == "script-control" || action == "script-reload") && panel.runtimeActionSink != nullptr)
    {
        UiAction request;
        request.type = action == "script-reload" ? UiActionType::ReloadScripts : UiActionType::ControlScripts;
        request.intValue = element.GetAttribute<int>("data-value", 0);
        request.actorProperty.worldGeneration = panel.runtimeView.scripts.worldGeneration;
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "script-add" && panel.runtimeActionSink != nullptr)
    {
        if (!panel.runtimeView.scripts.available || panel.runtimeView.scripts.state != ScriptPlayState::Editing ||
            (!panel.scriptPlacementCreateAsset && (panel.runtimeView.offlineMode || panel.IsAssetEditorVisible())) ||
            panel.runtimeView.scripts.classes.empty() || panel.HasModal()) return;
        UiAction request;
        request.actorProperty.worldGeneration = panel.runtimeView.scripts.worldGeneration;
        if (panel.scriptPlacementCreateAsset)
        {
            request.type = UiActionType::CreateScriptBlueprintAsset;
            request.stringValue = ControlValue(*panel.document->GetElementById("script-new-path"));
            request.script = {{"class", ControlValue(*panel.document->GetElementById("script-class"))},
                {"name", ControlValue(*panel.document->GetElementById("script-name"))}};
        }
        else
        {
            if (panel.runtimeView.scripts.classes.empty()) return;
            request.type = UiActionType::AddScriptBlueprint;
            request.script = {{"class", ControlValue(*panel.document->GetElementById("script-class"))},
                {"name", ControlValue(*panel.document->GetElementById("script-name"))},
                {"modelPath", ControlValue(*panel.document->GetElementById("script-mesh"))}};
        }
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if ((action == "script-reset" || action == "script-asset-create") && panel.runtimeActionSink != nullptr)
    {
        const auto* actor = panel.SelectedActor();
        if (!actor || actor->type != EditorSceneActorType::Script ||
            panel.runtimeView.scripts.state != ScriptPlayState::Editing) return;
        UiAction request;
        request.actorProperty.worldGeneration = actor->worldGeneration;
        request.actorProperty.identity = actor->identity;
        if (action == "script-reset")
        {
            if (index >= actor->scriptProperties.size()) return;
            auto property = actor->scriptProperties.begin();
            std::advance(property, index);
            if (std::find(actor->scriptInstanceEditable.begin(), actor->scriptInstanceEditable.end(),
                property.key()) == actor->scriptInstanceEditable.end()) return;
            if (!actor->scriptOverrides.contains(property.key())) return;
            request.type = UiActionType::SetScriptProperty;
            request.script = {{"property", property.key()}, {"reset", true}};
        }
        else
        {
            request.type = UiActionType::CreateScriptBlueprintAsset;
            request.stringValue = ControlValue(*panel.document->GetElementById("script-asset-path"));
        }
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "material-frame") panel.RequestPreviewCamera(ModelPreviewNavigation::Frame);
    else if (action == "json-close") panel.ShowMaterialDetails();
    else if (action == "resource-frame") panel.RequestPreviewCamera(ModelPreviewNavigation::Frame);
    else if (action == "hair-reset") panel.RequestHairReset();
    else if (action == "hair-save") panel.SubmitHairSimulationSave();
    else if (action == "hair-group-default") panel.SelectHairGroup(-1);
    else if (action == "hair-group-select") panel.SelectHairGroup(static_cast<int>(index));
    else if (action == "hair-group-colors")
    {
        panel.hairGroupColors = !panel.hairGroupColors;
        panel.hairGroupStatus = panel.hairGroupColors
            ? "Group colors enabled for this preview session."
            : "Group colors disabled; the material appearance is restored.";
        panel.SubmitTressfxGroupPreview();
    }
    else if (action == "hair-group-solo")
    {
        panel.hairGroupSolo = !panel.hairGroupSolo;
        panel.hairGroupStatus = panel.hairGroupSolo
            ? "Solo enabled: only the selected group is drawn."
            : "Solo disabled.";
        panel.SubmitTressfxGroupPreview();
    }
    else if (action == "hair-group-paint" || action == "hair-group-paint-start")
    {
        if (panel.hairGroupPaintActive && action == "hair-group-paint")
            panel.ToggleHairGroupPaint(false);
        else
            panel.ToggleHairGroupPaint(true);
    }
    else if (action == "hair-lasso-toggle")
    {
        // 切换工具时放弃未提交手势：套索点与圆形 ghost 都不能跨工具混用。
        if (panel.hairBrushStrokeActive) panel.CancelHairBrushStroke(false);
        panel.hairLassoActive = !panel.hairLassoActive;
        panel.hairLassoDragging = false;
        panel.hairLassoScreenPoints.clear();
        panel.hairBrushStatus = panel.hairLassoActive
            ? "Lasso: drag to enclose roots; release commits, Esc cancels."
            : "Brush mode: left drag adds, Shift adds, Alt removes; Enter or release commits.";
    }
    else if (action == "hair-group-create-open")
    {
        panel.hairGroupCreateActive = true;
        panel.hairGroupCreateName.clear();
        panel.hairGroupCreateStart.clear();
        panel.hairGroupCreateEnd.clear();
        panel.hairGroupCreateError.clear();
        panel.hairGroupStatus = "Enter a name; the group starts empty and is filled by the brush.";
    }
    else if (action == "hair-group-create-cancel")
    {
        panel.hairGroupCreateActive = false;
        panel.hairGroupCreateName.clear();
        panel.hairGroupCreateStart.clear();
        panel.hairGroupCreateEnd.clear();
        panel.hairGroupCreateError.clear();
        panel.hairGroupStatus = "New group cancelled.";
    }
    else if (action == "hair-group-create-submit")
    {
        std::string error;
        if (!panel.hairGroups.AddEmptyGroup(panel.hairGroupCreateName, error))
        {
            panel.hairGroupCreateError = error;
            return;
        }
        panel.hairPaintUndo.clear();
        // 新组追加在声明顺序末尾；后续组号/颜色由 vector 下标重新生成。
        panel.hairGroupSelected = static_cast<int>(panel.hairGroups.Groups().size()) - 1;
        panel.hairGroupCreateActive = false;
        panel.hairGroupCreateName.clear();
        panel.hairGroupCreateStart.clear();
        panel.hairGroupCreateEnd.clear();
        panel.hairGroupCreateError.clear();
        panel.hairGroupStatus = "Empty group created. Paint guides, then Save to Hair Asset.";
    }
    else if (action == "hair-group-range-open")
    {
        if (panel.hairGroupSelected < 0) return;
        panel.hairGroupAddRangeActive = true;
        panel.hairGroupRangeStart.clear();
        panel.hairGroupRangeEnd.clear();
        panel.hairGroupRangeError.clear();
    }
    else if (action == "hair-group-range-cancel")
    {
        panel.hairGroupAddRangeActive = false;
        panel.hairGroupRangeStart.clear();
        panel.hairGroupRangeEnd.clear();
        panel.hairGroupRangeError.clear();
    }
    else if (action == "hair-group-range-submit")
    {
        if (panel.hairGroupSelected < 0) return;
        std::pair<uint32_t, uint32_t> range;
        std::string error;
        if (!ParseHairGuideRange(panel.hairGroupRangeStart, panel.hairGroupRangeEnd, range, error) ||
            !panel.hairGroups.AddRange(static_cast<std::size_t>(panel.hairGroupSelected), range, error))
        {
            panel.hairGroupRangeError = error;
            return;
        }
        panel.hairPaintUndo.clear();
        panel.hairGroupAddRangeActive = false;
        panel.hairGroupRangeStart.clear();
        panel.hairGroupRangeEnd.clear();
        panel.hairGroupRangeError.clear();
        panel.hairGroupStatus = "Guide range added to the asset draft.";
    }
    else if (action == "hair-group-range-remove")
    {
        if (panel.hairGroupSelected < 0) return;
        const int removedGroup = panel.hairGroupSelected;
        const std::size_t before = panel.hairGroups.Groups().size();
        std::string error;
        if (!panel.hairGroups.RemoveRange(static_cast<std::size_t>(panel.hairGroupSelected), index, error))
        {
            panel.hairGroupStatus = error;
            return;
        }
        panel.hairPaintUndo.clear();
        const int after = static_cast<int>(panel.hairGroups.Groups().size());
        if (after < static_cast<int>(before))
            panel.hairGroupSelected = removedGroup > 0 ? removedGroup - 1 : -1;
        if (panel.hairGroupSelected >= after) panel.hairGroupSelected = after - 1;
        panel.hairGroupAddRangeActive = false;
        panel.hairGroupStatus = after < static_cast<int>(before)
            ? "Removed the last guide range and deleted the empty group; preview updated."
            : "Guide range removed; preview updated.";
    }
    else if (action == "hair-group-clear-overrides")
    {
        if (panel.hairGroupSelected < 0) return;
        panel.hairGroups.ClearOverrides(static_cast<std::size_t>(panel.hairGroupSelected));
        panel.hairPaintUndo.clear();
        panel.hairGroupStatus = "Group overrides cleared in the asset draft.";
    }
    else if (action == "hair-group-delete")
    {
        if (panel.hairGroupSelected < 0) return;
        const int removedGroup = panel.hairGroupSelected;
        panel.hairGroups.DeleteGroup(static_cast<std::size_t>(removedGroup));
        panel.hairPaintUndo.clear();
        const int after = static_cast<int>(panel.hairGroups.Groups().size());
        panel.hairGroupSelected = removedGroup > 0 ? removedGroup - 1 : -1;
        if (panel.hairGroupSelected >= after) panel.hairGroupSelected = after - 1;
        panel.hairGroupAddRangeActive = false;
        panel.hairGroupStatus = "Group deleted from the draft; preview updated. Save to Hair Asset to persist it.";
    }
    else if (action == "hair-group-save") panel.SubmitHairGroupsSave();
    else if (action == "animation-browser-clip")
    {
        const auto* active = panel.resourceEditor.ActiveTab();
        const size_t index = element.GetAttribute<size_t>("data-index", panel.runtimeView.animationClips.size());
        if (active && active->kind == EditorAssetKind::Retarget && index < panel.runtimeView.animationClips.size() &&
            panel.CurrentModelPreviewGeneration() && panel.runtimeView.animationWorldGeneration == panel.CurrentModelPreviewGeneration() &&
            panel.runtimeActionSink)
        {
            UiAction request;
            request.type = UiActionType::ControlAnimationPreview;
            request.previewCamera.worldGeneration = panel.CurrentModelPreviewGeneration();
            request.stringValue = "previewClip";
            request.floatValue = static_cast<float>(index);
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
    }
    else if (action == "animation-play" || action == "animation-pause" ||
        action == "animation-clip" || action == "animation-time")
    {
        if (panel.runtimeActionSink && panel.HasCurrentModelPreview() &&
            panel.runtimeView.animationWorldGeneration == panel.CurrentModelPreviewGeneration())
        {
            UiAction request;
            request.type = UiActionType::ControlAnimationPreview;
            request.previewCamera.worldGeneration = panel.CurrentModelPreviewGeneration();
            request.stringValue = action == "animation-clip" ?
                (!panel.requestedRetargetPath.empty() ? "previewClip" : "clip") :
                action == "animation-time" ? "seek" : "play";
            request.floatValue = action == "animation-play" ? 1.0f : 0.0f;
            if (action == "animation-clip" || action == "animation-time")
            {
                const auto* active = panel.resourceEditor.ActiveTab();
                const std::string prefix = active && active->kind == EditorAssetKind::Retarget ?
                    "retarget-animation" : "resource-animation";
                auto* control = panel.document->GetElementById(prefix +
                    (action == "animation-clip" ? "-clip" : "-time"));
                try { request.floatValue = std::stof(ControlValue(*control)); }
                catch (...) { return; }
            }
            panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
        }
    }
    else if (action == "resource-texture-fit") { panel.ResetTextureView(); panel.RefreshResourceEditor(); }
    else if (action == "resource-reload") panel.RequestResourceReload();
    else if (action == "resource-tool")
    {
        panel.resourceTransformTool = element.GetAttribute<std::string>("data-tool", "select");
        panel.RefreshResourceEditor();
    }
    else if (action == "resource-space")
    {
        panel.resourceTransformSpace = element.GetAttribute<std::string>("data-space", "world");
        panel.RefreshResourceEditor();
    }
    else if (action == "browser-refresh")
    {
        if (panel.jsonAssetRequestSink != nullptr) panel.jsonAssetRequestSink->RequestRefreshAssetCatalog();
    }
    else if (action == "workspace-content" || action == "workspace-renderer")
    {
        panel.docking.Activate(action == "workspace-renderer" ? "renderer" : "content");
    }
    else if (action == "save") panel.RequestSave();
    else if (action == "save-all") panel.RequestSaveAll();
    else if (action == "save-confirm") panel.ConfirmSave();
    else if (action == "save-cancel")
    {
        if (panel.saveWaiting.empty()) panel.saveDialogVisible = false;
        panel.RefreshSaveDialog();
    }
    else if (action == "save-select-all")
    {
        for (const auto& row : panel.saveRows) panel.saveChecked.insert(row.assetPath);
        panel.RefreshSaveDialog();
    }
    else if (action == "context-browser-save") panel.OpenSaveDialog(panel.contextMenuTarget);
    else if (action == "context-class-source") panel.OpenBlueprintClassSource(panel.contextMenuTarget);
    else if (action == "context-class-create") panel.BeginBlueprintClassAsset(panel.contextMenuTarget);
    else if (action == "menu-open") panel.ToggleMenu(element.GetAttribute<std::string>("data-menu", ""));
    else if (action == "menu-window")
    {
        // 面板可见性由停靠视图裁决；被拒绝时 docking 会给出人类可读的 notice。
        const auto& panels = UiWidgets::DockWorkspace::Panels();
        if (index < panels.size())
            panel.docking.SetPanelVisible(panels[index], !panel.docking.IsPanelVisible(panels[index]));
    }
    else if (action == "menu-level" && index < panel.menuLevelPaths.size())
    {
        // 与 Content Browser 双击关卡走同一条 LoadWorld 事务；UI 不直接触碰 World。
        panel.OpenBrowserAsset(panel.menuLevelPaths[index]);
    }
    else if (action == "menu-reset-layout") panel.docking.Reset();
    else if (action == "menu-restore-layout")
    {
        panel.docking.RestoreSavedLayout();
    }
    else if (action == "about-open") panel.aboutVisible = true;
    else if (action == "about-close") panel.aboutVisible = false;
    else if (action == "terrain-mode-layers")
    {
        panel.terrainPanelMode = MaterialInstanceAssetEditorPanel::MaterialInstanceAssetEditorPanel::TerrainPanelMode::Layers;
        panel.RefreshTerrainBrushUi();
        panel.RefreshTerrainLayerLibrary();
    }
    else if (action == "terrain-layer-view-grid" || action == "terrain-layer-view-list")
    {
        panel.terrainLayerListView = action == "terrain-layer-view-list";
        panel.terrainLayerScrollResetPending = true;
        panel.terrainLayerWindowDirty = true;
        panel.RefreshTerrainLayerLibrary();
    }
    else if (action == "brush-library-select")
    {
        panel.ApplyTerrainBrushPreset(element.GetAttribute<std::string>("data-brush-id", ""));
    }
    else if (action == "terrain-layer-select")
    {
        const EditorSceneActorSnapshot* actor = panel.SelectedTerrainActor();
        const size_t count = actor == nullptr ? 0 : actor->terrainPlan->desc.layers.size();
        const size_t index = element.GetAttribute<size_t>("data-index", count);
        if (index < count)
        {
            panel.terrainPaintLayer = static_cast<uint32_t>(index);
            panel.RefreshTerrainLayerLibrary();
        }
    }
    else if (action == "viewport-tool-landscape")
    {
        panel.landscapeToolActive = !panel.landscapeToolActive;
        if (panel.landscapeToolActive)
        {
            panel.landscapePaintMode = false;
            panel.terrainPanelMode = MaterialInstanceAssetEditorPanel::MaterialInstanceAssetEditorPanel::TerrainPanelMode::Sculpt;
            if (panel.sceneGizmoDragging) panel.CancelSceneGizmoDrag();
            panel.viewportTransformTool = "select";
        }
        else
        {
            panel.CancelTerrainBrushStroke();
            panel.ClearTerrainBrushHit();
            panel.landscapePaintMode = false;
            panel.terrainPanelMode = MaterialInstanceAssetEditorPanel::MaterialInstanceAssetEditorPanel::TerrainPanelMode::Layers;
        }
        panel.RefreshViewportToolbar();
    }
    else if (action == "viewport-landscape-sculpt" || action == "terrain-mode-sculpt")
    {
        panel.CancelTerrainBrushStroke();
        panel.ClearTerrainBrushHit();
        panel.landscapePaintMode = false;
        panel.landscapeToolActive = true;
        panel.viewportTransformTool = "select";
        panel.terrainPanelMode = MaterialInstanceAssetEditorPanel::MaterialInstanceAssetEditorPanel::TerrainPanelMode::Sculpt;
        panel.RefreshViewportToolbar();
    }
    else if (action == "viewport-landscape-paint" || action == "terrain-mode-paint")
    {
        panel.ActivateTerrainPaintMode();
    }
    else if (action == "viewport-paint-erase")
    {
        panel.landscapePaintErase = true;
        panel.RefreshTerrainBrushUi();
    }
    else if (action == "viewport-paint-mode")
    {
        panel.landscapePaintErase = false;
        panel.RefreshTerrainBrushUi();
    }
    else if (action == "viewport-brush-mode")
    {
        panel.terrainSculptMode = std::clamp(element.GetAttribute<int>("data-mode", 0), 0, 2);
        panel.RefreshTerrainBrushUi();
    }
    else if (action == "viewport-landscape-write")
    {
        if (panel.terrainBrushStrokeActive || panel.runtimeActionSink == nullptr) return;
        if (panel.landscapePaintMode)
            panel.SendTerrainPaintAction(TerrainBrushOperation::SavePaint, false);
        else
            panel.SendTerrainBrushAction(TerrainBrushOperation::SaveHeightmap, false);
    }
    else if (action == "viewport-tool")
    {
        // 工具条、Details 与 Gizmo 共用这一份模式状态；切工具前先把手势恢复到
        // 原始 TRS，不能让一次半途拖动跨模式提交。
        if (panel.sceneGizmoDragging) panel.CancelSceneGizmoDrag();
        if (panel.landscapeToolActive)
        {
            panel.landscapeToolActive = false;
            panel.landscapePaintMode = false;
            panel.terrainPanelMode = MaterialInstanceAssetEditorPanel::MaterialInstanceAssetEditorPanel::TerrainPanelMode::Layers;
            panel.CancelTerrainBrushStroke();
            panel.ClearTerrainBrushHit();
        }
        panel.viewportTransformTool = element.GetAttribute<std::string>("data-tool", "select");
        panel.RefreshViewportToolbar();
    }
    else if (action == "viewport-space")
    {
        // World/Local 只影响 Gizmo 轴方向，不改变渲染相机或场景坐标。
        if (panel.sceneGizmoDragging) panel.CancelSceneGizmoDrag();
        panel.viewportTransformSpace = element.GetAttribute<std::string>("data-space", "world");
        panel.RefreshViewportToolbar();
    }
    else if (action == "actor-exposure-manual" || action == "actor-exposure-auto")
    {
        const auto* actor = panel.SelectedActor();
        if (panel.runtimeActionSink == nullptr || actor == nullptr || actor->type != EditorSceneActorType::Camera) return;
        const ExposureMode mode = action == "actor-exposure-auto" ? ExposureMode::Auto : ExposureMode::Ev100;
        if (actor->camera.exposure.mode == mode) return;
        UiAction request;
        request.type = UiActionType::SetActorProperty;
        request.actorProperty = {actor->worldGeneration, actor->identity,
            EditorActorProperty::CameraExposureMode, 0, mode == ExposureMode::Auto ? 2.0f : 0.0f};
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "actor-csm-enable" || action == "actor-csm-disable")
    {
        if (panel.runtimeActionSink == nullptr) return;
        const bool enabled = action == "actor-csm-enable";
        if (enabled == panel.runtimeView.csmCastShadows) return;
        const auto* actor = panel.SelectedActor();
        if (!actor || !actor->primaryDirectionalLight) return;
        UiAction request;
        request.type = UiActionType::SetActorProperty;
        request.actorProperty = {actor->worldGeneration, actor->identity,
            EditorActorProperty::CastShadows, 0, enabled ? 1.0f : 0.0f};
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "revert")
    {
        if (const auto* tab = panel.resourceEditor.ActiveTab(); tab &&
            tab->kind == EditorAssetKind::Terrain && panel.IsResourceEditorVisible())
        {
            const auto* terrain = panel.TerrainDocument(tab->path);
            if (terrain == nullptr) return;
            EditorCommandEnvelope command;
            command.type = EditorCommandType::RevertTerrainDocument;
            command.expectedDocumentRevision = terrain->documentRevision;
            command.payload = MaterialInstanceAssetPathPayload{tab->path};
            panel.Submit(std::move(command));
            return;
        }
        panel.pendingEdits.clear();
        panel.pendingValues.clear();
        panel.SubmitPathCommand(EditorCommandType::RevertMaterialInstanceDocument);
    }
    else if (action == "validate")
    {
        if (!panel.pendingEdits.empty())
        {
            panel.QueueEdit("validate", {EditorCommandType::ValidateMaterialInstanceDocument,
                MaterialInstanceAssetPathPayload{panel.pendingAssetPath}});
            panel.FlushPendingEdits();
        }
        else panel.SubmitPathCommand(EditorCommandType::ValidateMaterialInstanceDocument);
    }
    else if (action == "apply-preview")
    {
        // 修改 batch 已触发自动预览；不能再用旧 revision 紧跟着请求一次 Apply。
        if (!panel.pendingEdits.empty()) panel.FlushPendingEdits();
        else panel.SubmitPathCommand(EditorCommandType::ApplyMaterialInstancePreview);
    }
    else if (action == "restore-preview")
    {
        // batch 的自动 Apply 在命令队列尾部；同帧 Restore 会被它覆盖，必须等权威快照。
        if (!panel.pendingEdits.empty())
        {
            panel.FlushPendingEdits();
            panel.localStatus = "Edits submitted. Restore baseline after preview completes.";
        }
        else panel.SubmitPathCommand(EditorCommandType::RestoreMaterialInstancePreviewBaseline);
    }
    else if (action == "reload")
    {
        if (const auto* tab = panel.resourceEditor.ActiveTab(); tab &&
            tab->kind == EditorAssetKind::Terrain && panel.IsResourceEditorVisible())
        {
            const auto* terrain = panel.TerrainDocument(tab->path);
            if (terrain == nullptr) return;
            EditorCommandEnvelope command;
            command.type = EditorCommandType::ReloadTerrainDocument;
            command.expectedDocumentRevision = terrain->documentRevision;
            command.payload = ReloadMaterialInstanceDocumentPayload{
                tab->path, EditorDirtyDocumentPolicy::RequireClean};
            panel.Submit(std::move(command));
            return;
        }
        if (!panel.snapshot.activeDocument) return;
        if (!panel.pendingEdits.empty())
        {
            panel.localStatus = "Save or revert pending edits before reloading.";
            return;
        }
        EditorCommandEnvelope command;
        command.type = EditorCommandType::ReloadMaterialInstanceDocument;
        command.expectedDocumentRevision = panel.snapshot.activeDocument->revision;
        command.payload = ReloadMaterialInstanceDocumentPayload{
            panel.snapshot.activeDocument->assetPath, EditorDirtyDocumentPolicy::RequireClean};
        panel.Submit(std::move(command));
    }
    else if (action == "reset-all" && panel.snapshot.activeDocument)
    {
        panel.pendingEdits.clear();
        panel.pendingValues.clear();
        for (const auto& parameter : panel.snapshot.activeDocument->parameters)
            panel.pendingValues[parameter.name] = parameter.defaultValue;
        panel.QueueEdit("all", {EditorCommandType::ResetMaterialInstanceOverrides,
            ResetMaterialInstanceOverridesPayload{panel.snapshot.activeDocument->assetPath, EditorResetScope::All}});
    }
    else if (action == "reset-parameter" && panel.snapshot.activeDocument && index < panel.snapshot.activeDocument->parameters.size())
    {
        const auto& parameter = panel.snapshot.activeDocument->parameters[index];
        panel.pendingValues[parameter.name] = parameter.defaultValue;
        panel.QueueEdit("parameter:" + parameter.name, {EditorCommandType::ClearMaterialParameterOverride,
            ClearMaterialParameterOverridePayload{panel.snapshot.activeDocument->assetPath, parameter.name}});
    }
    else if (action == "reset-state" && panel.snapshot.activeDocument && index < panel.snapshot.activeDocument->renderStates.size())
    {
        const auto& state = panel.snapshot.activeDocument->renderStates[index];
        panel.QueueEdit("state:" + state.name, {EditorCommandType::ClearMaterialRenderStateOverride,
            ClearMaterialRenderStateOverridePayload{panel.snapshot.activeDocument->assetPath,
                ParseEditorMaterialRenderStateField(state.name)}});
    }
    else if (action == "reset-texture" && panel.snapshot.activeDocument && index < panel.snapshot.activeDocument->textures.size())
    {
        const auto& texture = panel.snapshot.activeDocument->textures[index];
        panel.QueueEdit("texture:" + texture.slotName, {EditorCommandType::ClearMaterialTextureOverride,
            ClearMaterialTextureOverridePayload{panel.snapshot.activeDocument->assetPath, texture.slotName}});
    }
    else if (action == "section")
    {
        // 展开状态只在面板里，切换不提交业务命令，也不写资产或停靠布局。
        panel.ToggleSection(element.GetAttribute<std::string>("data-section", ""));
    }
    else if (action == "tab" && index < panel.snapshot.documentTabs.size())
    {
        panel.ShowMaterialDetails();
        panel.FlushPendingEdits();
        EditorCommandEnvelope command;
        command.type = EditorCommandType::SelectMaterialInstanceDocument;
        command.payload = MaterialInstanceAssetPathPayload{panel.snapshot.documentTabs[index].assetPath};
        panel.Submit(std::move(command));
    }
    else if (action == "close")
    {
        const std::string targetPanel = element.GetAttribute<std::string>("data-key", "");
        const std::string targetPath = panel.docking.ResourcePathForPanel(targetPanel);
        if (!targetPath.empty() && targetPanel.rfind("resource:material:", 0) != 0)
        {
            panel.RequestCloseResource(targetPath);
            return;
        }
        size_t closeIndex = index;
        if (!targetPanel.empty())
        {
            closeIndex = panel.snapshot.documentTabs.size();
            for (size_t candidate = 0; candidate < panel.snapshot.documentTabs.size(); ++candidate)
                if (panel.snapshot.documentTabs[candidate].assetPath == targetPath) closeIndex = candidate;
        }
        if (closeIndex >= panel.snapshot.documentTabs.size()) return;
        const auto& tab = panel.snapshot.documentTabs[closeIndex];
        if (tab.dirty || (!panel.pendingEdits.empty() && panel.pendingAssetPath == tab.assetPath))
        {
            panel.FlushPendingEdits();
            panel.pendingClosePath = tab.assetPath;
            panel.pendingCloseKind = EditorAssetKind::MaterialInstance;
            panel.saveThenClose = false;
            panel.saveCloseSubmitted = false;
        }
        else panel.CloseDocument(tab.assetPath, EditorDirtyDocumentPolicy::RequireClean);
    }
    else if (action == "close-cancel")
    {
        panel.pendingClosePath.clear();
        panel.saveThenClose = false;
        panel.saveCloseSubmitted = false;
        panel.discardBlueprintThenClose = panel.discardBlueprintCloseSubmitted = false;
    }
    else if (action == "close-discard")
    {
        if (panel.pendingCloseKind == EditorAssetKind::Blueprint || panel.pendingCloseKind == EditorAssetKind::Tressfx)
        {
            panel.discardBlueprintThenClose = true;
            panel.discardBlueprintCloseSubmitted = false;
            return;
        }
        panel.CloseDocument(panel.pendingClosePath, EditorDirtyDocumentPolicy::DiscardChanges);
        panel.pendingClosePath.clear();
        panel.saveThenClose = false;
        panel.saveCloseSubmitted = false;
    }
    else if (action == "close-save")
    {
        if (panel.pendingCloseKind == EditorAssetKind::Blueprint || panel.pendingCloseKind == EditorAssetKind::Tressfx)
        {
            panel.saveStartedSequence = 0;
            for (const auto& feedback : panel.snapshot.saveFeedback)
                panel.saveStartedSequence = std::max(panel.saveStartedSequence, feedback.sequence);
            panel.saveThenClose = true;
            panel.saveCloseSubmitted = false;
            return;
        }
        // 非活动页先选中以获得权威 revision；保存失败时不能关闭文档。
        if (!panel.snapshot.activeDocument || panel.snapshot.activeDocument->assetPath != panel.pendingClosePath)
        {
            EditorCommandEnvelope command;
            command.type = EditorCommandType::SelectMaterialInstanceDocument;
            command.payload = MaterialInstanceAssetPathPayload{panel.pendingClosePath};
            panel.Submit(std::move(command));
        }
        panel.saveThenClose = true;
        panel.saveCloseSubmitted = false;
    }
    else if (action == "outliner-expand" && index < panel.OutlinerActors().size())
    {
        const std::string& identity = panel.OutlinerActors()[index].identity;
        if (panel.outlinerExpansion.expandedActors.erase(identity) == 0)
            panel.outlinerExpansion.expandedActors.insert(identity);
    }
    else if ((action == "outliner-actor" || action == "viewport-actor") && index < panel.OutlinerActors().size())
    {
        panel.SelectOutlinerActor(panel.OutlinerActors()[index]);
    }
    else if (action == "viewport-actor-visuals")
    {
        panel.showActorVisualization = !panel.showActorVisualization;
        panel.RefreshActorVisualization();
    }
    else if (action == "outliner-visible" && index < panel.OutlinerActors().size())
    {
        // 眼睛不改选择、不改展开、不改滚动：它只发一条运行时命令，且身份用冻结快照里的
        // 稳定字符串，不用 DOM 上的数据或 sceneActors 下标。
        const EditorSceneActorSnapshot& actor = panel.OutlinerActors()[index];
        if (panel.runtimeActionSink == nullptr || !SceneActorHasVisibilityToggle(actor.type)) return;
        UiAction request;
        request.type = UiActionType::SetActorVisible;
        request.stringValue = actor.identity;
        request.intValue = actor.visible ? 0 : 1;
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "outliner-slot" && index < panel.OutlinerActors().size())
    {
        const EditorSceneActorSnapshot& actor = panel.OutlinerActors()[index];
        panel.SelectModelMaterial(MeshSceneActorModel(actor),
            element.GetAttribute<uint32_t>("data-component", 0));
    }
    else if (action == "slot" && panel.sceneSelectionSink != nullptr && panel.snapshot.selectedModel &&
        index < panel.snapshot.selectedModel->materials.size())
    {
        panel.SelectModelMaterial(*panel.snapshot.selectedModel, index);
    }
    // 上下文菜单条目：目标一律取打开菜单时记下的稳定地址，不读面板的当前选择，
    // 否则"右键 B、菜单却改 A"就会静默发生。
    else if (action == "context-outliner-visible") panel.ToggleContextActorVisibility();
    else if (action == "context-actor-material") panel.OpenContextActorMaterial(index);
    else if (action == "context-browser-open") panel.OpenContextBrowserAsset();
    else if (action == "context-view-mode" && panel.runtimeActionSink != nullptr)
    {
        UiAction request;
        request.type = UiActionType::SetDebugViewMode;
        request.intValue = static_cast<int>(index);
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
    else if (action == "context-save-all")
    {
        panel.RequestSaveAll();
    }
    else if (panel.runtimeActionSink != nullptr)
    {
        UiAction request;
        if (action == "immersive-view") request.type = UiActionType::ToggleDeveloperUi;
        else if (action == "exit-editor") request.type = UiActionType::Quit;
        else if (action == "viewport-view-mode")
        {
            // 视口工具条里只有 view mode 有真实后端：Lit=0 完整画面，Unlit=1 基础色。
            request.type = UiActionType::SetDebugViewMode;
            request.intValue = element.GetAttribute<int>("data-mode", 0);
        }
        else if (action == "offline-mode")
        {
            request.type = UiActionType::SetOfflineMode;
            request.intValue = element.GetAttribute<int>("data-value", 0);
        }
        else if (action == "offline-control")
        {
            request.type = UiActionType::ControlOfflineSession;
            request.intValue = element.GetAttribute<int>("data-value", 0);
        }
        else if (action == "offline-export") request.type = UiActionType::ExportOffline;
        else return;
        panel.runtimeActionSink->RequestRuntimeAction(std::move(request));
    }
}


} // namespace Panel
} // namespace VL::EditorUi

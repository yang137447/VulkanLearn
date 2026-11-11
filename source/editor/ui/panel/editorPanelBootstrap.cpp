#include "editor/ui/panel/editorPanelBootstrap.h"

#include <utility>

#include <RmlUi/Core/ElementDocument.h>

#include "editor/ui/editorContentBrowserController.h"
#include "editor/ui/editorDockingController.h"
#include "editor/ui/editorJsonInspectorController.h"
#include "editor/ui/editorLogPanel.h"
#include "editor/ui/editorPerformancePanel.h"
#include "editor/ui/retargetAssetEditorPanel.h"
#include "editor/ui/terrainAssetEditorController.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/widgets/editor/brushLibraryPanel.h"

namespace VL::EditorUi::Panel
{

bool ValidateEditorPanelDocument(Rml::ElementDocument& candidate, std::string& error)
{
    for (const char* id : {
        "scene-models", "material-slots", "material-slots-section", "asset-list", "material-editor", "actor-details-content",
        "viewport-actor-overlay", "viewport-actor-lines", "viewport-actor-icons", "viewport-actor-visuals",
        "material-preview-image", "material-preview-status", "material-frame",
        "document-title", "document-status", "editor-notice", "details-content",
        "offline-status", "offline-notice", "close-dialog", "close-document-name",
        "mode-realtime", "mode-offline", "menu-file-save", "menu-file-revert",
        "menu-file-reload", "menu-edit-reset", "menu-edit-validate",
        "apply-preview", "restore-preview",
        "offline-samples", "offline-bounces", "offline-lightSamples", "offline-seed",
        "offline-aov", "offline-exposure", "offline-pause", "offline-resume",
        "offline-stop", "offline-restart", "offline-export", "asset-search",
        "close-document-status", "close-save", "editor-navigation",
        "editor-inspector", "editor-content", "workspace-content-tab", "workspace-renderer-tab",
        "content-browser-page", "renderer-page", "performance-page", "workspace-performance-tab",
        "performance-fps", "performance-cpu-ms", "performance-gpu-ms", "performance-status",
        "performance-passes", "editor-viewport-surface", "editor-viewport-image",
        "workspace-log-tab", "log-page", "log-scroll", "log-rows", "log-count",
        "workspace-terrain-tab", "terrain-page", "terrain-page-body", "terrain-empty", "terrain-summary",
        "terrain-mode-sculpt", "terrain-mode-paint", "terrain-mode-layers",
        "terrain-layer-search", "terrain-layer-view-grid", "terrain-layer-view-list",
        "terrain-panel-filter-row",
        "terrain-workspace", "terrain-brush-column", "terrain-brush-library-title",
        "terrain-brush-library", "terrain-layer-library",
        "terrain-layer-visible-count", "terrain-layer-scroll", "terrain-layer-cards",
        "terrain-layer-top-spacer", "terrain-layer-bottom-spacer",
        "terrain-layer-empty", "terrain-layer-status", "terrain-layer-hover-preview",
        "terrain-layer-hover-image",
        "terrain-surface-panel", "terrain-surface-size", "terrain-surface-height-scale",
        "terrain-surface-layer-count", "terrain-surface-chunk-count",
        "terrain-surface-section-count",
        "log-output", "log-scripts", "log-output-tab", "log-scripts-tab",
        "log-severity", "log-source", "log-search", "log-autoscroll", "log-clear",
        "log-scripts-search", "log-scripts-summary", "log-scripts-rows",
        "log-scripts-page", "log-scripts-prev", "log-scripts-next",
        "workspace-viewport-tab", "workspace-outliner-tab", "workspace-details-tab",
        "workspace-resource-tab", "workspace-resource-icon", "workspace-resource-label",
        "resource-editor", "resource-editor-toolbar", "resource-reload", "resource-preview-panel",
        "blueprint-asset-editor", "blueprint-asset-save", "blueprint-asset-revert",
        "terrain-asset-editor", "terrain-asset-save", "terrain-asset-revert",
        "retarget-asset-editor", "retarget-title", "retarget-identity", "retarget-status",
        "retarget-bind", "retarget-calibrated", "retarget-front", "retarget-side", "retarget-top",
        "retarget-anchor", "retarget-surface", "retarget-lines", "retarget-mappings",
        "retarget-focus", "retarget-fit", "retarget-zoom", "retarget-all-bones",
        "retarget-selection", "retarget-measurement", "retarget-prev", "retarget-next", "retarget-page",
        "retarget-reference", "retarget-animation", "retarget-reference-toolbar", "retarget-reference-workspace",
        "retarget-hierarchy", "retarget-hierarchy-source", "retarget-hierarchy-target", "retarget-details",
        "retarget-editor-workspace", "retarget-center", "retarget-sidebar", "retarget-animation-assets",
        "retarget-animation-workspace", "retarget-preview-surface", "retarget-preview-image", "retarget-preview-status",
        "retarget-animation-tools", "retarget-animation-play", "retarget-animation-pause", "retarget-animation-clip",
        "retarget-animation-time", "retarget-animation-status", "retarget-animation-frame",
        "resource-preview-title", "resource-preview-kind", "resource-preview-surface",
        "resource-preview-icon", "resource-preview-status", "resource-preview-path",
        "resource-preview-image",
        "resource-animation-tools", "resource-animation-play", "resource-animation-pause",
        "resource-animation-clip", "resource-animation-time", "resource-animation-status",
        "viewport-motion-debug", "viewport-motion-status",
        "viewport-motion-matching", "motion-matching-title", "motion-matching-database",
        "motion-matching-query", "motion-matching-decision", "motion-matching-rows",
        "motion-timeline-play", "motion-timeline-pause", "motion-timeline-seek", "motion-timeline-status",
        "motion-replay", "motion-replay-seek", "motion-replay-status",
        "motion-query-toggle", "motion-query-details", "motion-query-channels",
        "motion-query-database", "motion-query-payload", "motion-query-component",
        "resource-motion-features", "resource-motion-status", "resource-motion-values",
        "resource-texture-image",
        "resource-texture-fit",
        "resource-frame",
        "resource-model-tools", "resource-tool-select", "resource-tool-move", "resource-tool-rotate",
        "resource-tool-scale", "resource-space-world", "resource-space-local", "resource-model-info",
        "resource-model-name", "resource-model-path", "resource-model-status", "resource-model-fields",
        "resource-model-description",
        "resource-details-panel", "resource-json-inspector", "resource-json-title",
        "resource-json-kind", "resource-json-path", "resource-json-description",
        "resource-json-status", "resource-json-fields", "resource-json-search",
        "resource-json-page", "resource-json-prev", "resource-json-next",
        "dock-parking", "dock-dynamic-tabs", "dock-shell", "dock-preview", "dock-drag-shield",
        "workspace-undo", "workspace-redo", "workspace-reset",
        "asset-sources", "asset-breadcrumbs", "asset-count", "asset-empty", "browser-selection",
        "browser-back", "browser-forward", "browser-up", "browser-sources-toggle",
        "browser-grid", "browser-list", "browser-recursive", "browser-sort", "browser-size",
        "browser-sources-panel", "browser-type", "browser-refresh",
        "material-instance-details", "json-inspector", "json-title", "json-kind", "json-path",
        "json-description", "json-status", "json-fields", "json-search", "json-page", "json-prev", "json-next",
        "editor-menubar", "menu-file", "menu-edit", "menu-window", "menu-help",
        "menu-file-popup", "menu-window-popup", "menu-window-panels", "menu-file-levels",
        "menu-file-save", "menu-file-revert", "menu-file-reload",
        "menu-edit-reset", "menu-edit-validate", "menu-edit-undo", "menu-edit-redo",
        "menu-window-reset", "menu-window-restore",
        "about-dialog", "about-close",
        "editor-context-menu", "editor-context-menu-title", "editor-context-menu-items",
        "outliner-search", "outliner-type", "outliner-sort", "outliner-count", "outliner-empty",
        "viewport-toolbar", "viewport-tool-select", "viewport-tool-move", "viewport-tool-rotate",
        "viewport-tool-scale", "viewport-space-world", "viewport-space-local",
        "viewport-snap-enabled", "viewport-snap-move", "viewport-snap-rotate", "viewport-snap-scale",
        "viewport-gizmo-overlay", "viewport-gizmo-capture",
        "viewport-brush-ring", "viewport-brush-ring-center",
        "viewport-tool-landscape", "viewport-landscape-controls",
        "viewport-landscape-sculpt", "viewport-landscape-paint",
        "viewport-landscape-write",
        "viewport-brush-settings", "viewport-brush-mode-0", "viewport-brush-mode-1",
        "viewport-brush-mode-2", "viewport-brush-radius", "viewport-brush-strength",
        "viewport-brush-falloff", "terrain-sculpt-settings", "viewport-paint-settings",
        "viewport-paint-mode", "terrain-paint-radius", "terrain-paint-hardness",
        "terrain-paint-opacity", "terrain-paint-flow",
        "viewport-paint-erase",
        "viewport-view-lit", "viewport-view-unlit", "viewport-view-wireframe",
        "viewport-axis-indicator", "viewport-axis-bar-x", "viewport-axis-bar-y",
        "viewport-axis-bar-z", "viewport-axis-label-x", "viewport-axis-label-y",
        "viewport-axis-label-z"})
    {
        if (candidate.GetElementById(id) == nullptr)
        {
            error = std::string("Editor workspace is missing element: ") + id;
            return false;
        }
    }
    return true;
}

void SetPanelMarkup(UiWidgets::WidgetDocument& widgetDocument, const std::string& id,
    std::string markup)
{
    widgetDocument.SetMarkup(id, std::move(markup));
}

void SetPanelControlValue(UiWidgets::WidgetDocument& widgetDocument, const std::string& id,
    const std::string& value)
{
    widgetDocument.SetControlValue(id, value);
}

void SetPanelDisabled(UiWidgets::WidgetDocument& widgetDocument, const std::string& id,
    bool disabled)
{
    widgetDocument.SetDisabled(id, disabled);
}

bool AttachEditorPanel(EditorPanelBootstrapContext& context, Rml::ElementDocument& candidate,
    std::string& error)
{
    if (!ValidateEditorPanelDocument(candidate, error)) return false;
    DetachEditorPanel(context);
    Rml::ElementDocument*& document = *context.document;
    document = &candidate;
    document->AddEventListener("click", context.listener);
    document->AddEventListener("change", context.listener);
    document->AddEventListener("dblclick", context.listener);
    document->AddEventListener("mousedown", context.listener, true);
    document->AddEventListener("mousemove", context.listener, true);
    document->AddEventListener("mouseup", context.listener, true);
    document->AddEventListener("mousescroll", context.listener, true);
    document->AddEventListener("dragstart", context.listener);
    document->AddEventListener("dragdrop", context.listener);
    document->AddEventListener("dragend", context.listener);
    document->AddEventListener("keydown", context.listener, true);
    if (!context.docking->Attach(candidate, error))
    {
        DetachEditorPanel(context);
        return false;
    }
    // 编辑器启动必须有一个可见的关卡编辑入口；停靠布局可能记住了 Content/Details 标签，
    // 但不能让默认启动场景变成"只有资产浏览器没有视口"。
    context.docking->SetPanelVisible("viewport", true);
    context.docking->Activate("viewport");
    context.widgetDocument->Attach(candidate);
    if (!context.brushLibrary->Attach(candidate, "terrain-brush-library", error))
    {
        DetachEditorPanel(context);
        return false;
    }
    context.retargetEditor->Attach(candidate);
    if (!context.contentBrowser->Attach(candidate, error))
    {
        DetachEditorPanel(context);
        return false;
    }
    if (!context.jsonInspector->Attach(candidate, error))
    {
        DetachEditorPanel(context);
        return false;
    }
    if (!context.resourceJsonInspector->Attach(candidate, error, "resource-json"))
    {
        DetachEditorPanel(context);
        return false;
    }
    context.terrainAssetEditor->SetCommandSink(context.commandSink);
    if (!context.terrainAssetEditor->Attach(candidate, error))
    {
        DetachEditorPanel(context);
        return false;
    }
    if (!context.logPanel->Attach(candidate, error))
    {
        DetachEditorPanel(context);
        return false;
    }
    if (!context.performancePanel->Attach(candidate, error))
    {
        DetachEditorPanel(context);
        return false;
    }
    context.detailsIdentity->clear();
    context.actorDetailsIdentity->clear();
    context.terrainSummarySignature->clear();
    context.blueprintAssetIdentity->clear();
    if (context.refreshWorkspace) context.refreshWorkspace();
    if (context.refreshLists) context.refreshLists();
    if (context.refreshDetails) context.refreshDetails();
    if (context.updateRuntimeView) context.updateRuntimeView(*context.runtimeView);
    return true;
}

void DetachEditorPanel(EditorPanelBootstrapContext& context)
{
    if (context.cancelBlueprintViewportDrag) context.cancelBlueprintViewportDrag();
    if (context.cancelSceneGizmoDrag) context.cancelSceneGizmoDrag();
    if (context.cancelTerrainBrushStroke) context.cancelTerrainBrushStroke();
    if (context.clearTerrainBrushHit) context.clearTerrainBrushHit();
    *context.landscapeToolActive = false;
    context.actorDragPath->clear();
    context.textureViewSource->clear();
    if (context.resetTextureView) context.resetTextureView();
    *context.previewDragButton = -1;
    *context.runtimeControlsCurrent = false;
    *context.motionMatchingRowsMounted = false;
    *context.motionMatchingVisible = false;
    *context.motionMatchingQueryExpanded = false;
    context.motionMatchingQueryChannels->clear();
    *context.motionMatchingRefreshTime = -1;
    context.motionMatchingRefreshKey->clear();
    *context.textureOptionsDirty = true;
    context.textureOptionSelections->clear();
    context.brushLibrary->Detach();
    *context.brushLibraryCatalogDomain = -1;
    if (*context.document != nullptr)
    {
        (*context.document)->RemoveEventListener("click", context.listener);
        (*context.document)->RemoveEventListener("change", context.listener);
        (*context.document)->RemoveEventListener("dblclick", context.listener);
        (*context.document)->RemoveEventListener("mousedown", context.listener, true);
        (*context.document)->RemoveEventListener("mousemove", context.listener, true);
        (*context.document)->RemoveEventListener("mouseup", context.listener, true);
        (*context.document)->RemoveEventListener("mousescroll", context.listener, true);
        (*context.document)->RemoveEventListener("dragstart", context.listener);
        (*context.document)->RemoveEventListener("dragdrop", context.listener);
        (*context.document)->RemoveEventListener("dragend", context.listener);
        (*context.document)->RemoveEventListener("keydown", context.listener, true);
        context.contentBrowser->Detach();
        context.logPanel->Detach();
        context.performancePanel->Detach();
        context.jsonInspector->Detach();
        context.resourceJsonInspector->Detach();
        context.docking->Detach();
        *context.document = nullptr;
    }
    context.terrainAssetEditor->Detach();
    context.widgetDocument->Detach();
    context.retargetEditor->Detach();
    context.axisIndicatorSignature->clear();
    context.terrainSummarySignature->clear();
    context.terrainLayerLibrarySignature->clear();
    context.terrainLayerSourceKey->clear();
    if (context.resetTerrainLayerWindow) context.resetTerrainLayerWindow();
    *context.terrainLayerWindowDirty = true;
    *context.terrainLayerScrollResetPending = true;
    *context.terrainLayerLastScrollTop = -1.0f;
    // 重新 Attach 时 DOM 是新的：让大纲第一帧无条件重挂一次。
    *context.outlinerMounted = false;
    *context.sceneGizmoMarkupMounted = false;
    // 右键菜单的目标属于旧文档的 DOM，重新接上后必须先回到"没有菜单"。
    *context.contextMenuKind = ContextMenuKind::None;
    context.contextMenuTarget->clear();
}

} // namespace VL::EditorUi::Panel

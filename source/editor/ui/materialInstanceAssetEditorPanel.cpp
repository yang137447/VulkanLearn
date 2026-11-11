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

MaterialInstanceAssetEditorPanel::MaterialInstanceAssetEditorPanel(IEditorCommandSink* sink)
    : commandSink(sink)
{
}

MaterialInstanceAssetEditorPanel::~MaterialInstanceAssetEditorPanel()
{
    Detach();
}

bool MaterialInstanceAssetEditorPanel::ValidateDocument(Rml::ElementDocument& candidate, std::string& error)
{
    return Panel::ValidateEditorPanelDocument(candidate, error);
}

bool MaterialInstanceAssetEditorPanel::Attach(Rml::ElementDocument& candidate, std::string& error)
{
    Panel::EditorPanelBootstrapContext context = BootstrapContext();
    return Panel::AttachEditorPanel(context, candidate, error);
}

void MaterialInstanceAssetEditorPanel::Detach()
{
    Panel::EditorPanelBootstrapContext context = BootstrapContext();
    Panel::DetachEditorPanel(context);
}

Panel::OutlinerViewContext MaterialInstanceAssetEditorPanel::OutlinerView()
{
    Panel::OutlinerViewContext context;
    context.document = &document;
    context.widgetDocument = &widgetDocument;
    context.snapshot = &snapshot;
    context.expansion = &outlinerExpansion;
    context.query = &outlinerQuery;
    context.typeFilter = &outlinerTypeFilter;
    context.sortOrder = &outlinerSortOrder;
    context.rows = &outlinerRows;
    context.builtSortOrder = &outlinerBuiltSortOrder;
    context.builtExpansion = &outlinerBuiltExpansion;
    context.builtQuery = &outlinerBuiltQuery;
    context.builtTypeFilter = &outlinerBuiltTypeFilter;
    context.rowSignature = &outlinerRowSignature;
    context.rowsRevision = &outlinerRowsRevision;
    context.mountedWindow = &outlinerMountedWindow;
    context.mounted = &outlinerMounted;
    context.mountedRevision = &outlinerMountedRevision;
    context.mountedVisuals = &outlinerMountedVisuals;
    context.rowHeight = &outlinerRowHeight;
    context.treeWorldGeneration = &treeWorldGeneration;
    context.showActorVisualization = showActorVisualization;
    context.sceneEditing = SceneEditingActive();
    context.closeContextMenu = [this] { CloseContextMenu(); };
    return context;
}

Panel::DetailsViewContext MaterialInstanceAssetEditorPanel::DetailsView()
{
    Panel::DetailsViewContext context;
    context.widgetDocument = &widgetDocument;
    context.snapshot = &snapshot;
    context.runtimeView = &runtimeView;
    context.collapsedSections = &collapsedSections;
    context.detailsIdentity = &detailsIdentity;
    context.actorDetailsIdentity = &actorDetailsIdentity;
    context.textureOptionsDirty = &textureOptionsDirty;
    context.refreshingControls = &refreshingControls;
    context.viewingJson = viewingJson;
    context.resourceEditorVisible = resourceEditorVisible;
    const auto* activeResourceTab = resourceEditor.ActiveTab();
    context.particleResourceActive = resourceEditorVisible &&
        activeResourceTab != nullptr &&
        activeResourceTab->kind == EditorAssetKind::ParticleSystem;
    context.localStatus = &localStatus;
    context.sceneLogicComponentsMarkup =
        [this](const EditorSceneActorSnapshot& actor) { return SceneLogicComponentsMarkup(actor); };
    context.refreshActorValues =
        [this](const EditorSceneActorSnapshot* actor) { RefreshActorValues(actor); };
    context.refreshControls = [this] { RefreshControls(); };
    return context;
}

Panel::EditorPanelBootstrapContext MaterialInstanceAssetEditorPanel::BootstrapContext()
{
    Panel::EditorPanelBootstrapContext context;
    context.document = &document;
    context.listener = this;
    context.docking = &docking;
    context.widgetDocument = &widgetDocument;
    context.brushLibrary = &brushLibrary;
    context.retargetEditor = &retargetEditor;
    context.contentBrowser = &contentBrowser;
    context.jsonInspector = &jsonInspector;
    context.resourceJsonInspector = &resourceJsonInspector;
    context.terrainAssetEditor = &terrainAssetEditor;
    context.commandSink = commandSink;
    context.logPanel = &logPanel;
    context.performancePanel = &performancePanel;
    context.detailsIdentity = &detailsIdentity;
    context.actorDetailsIdentity = &actorDetailsIdentity;
    context.terrainSummarySignature = &terrainSummarySignature;
    context.blueprintAssetIdentity = &blueprintAssetIdentity;
    context.runtimeView = &runtimeView;
    context.refreshWorkspace = [this] { RefreshWorkspace(); };
    context.refreshLists = [this] { RefreshLists(); };
    context.refreshDetails = [this] { RefreshDetails(); };
    context.updateRuntimeView = [this](const UiViewModelSnapshot& value) { UpdateRuntimeView(value); };
    context.cancelBlueprintViewportDrag = [this] { CancelBlueprintViewportDrag(); };
    context.cancelSceneGizmoDrag = [this] { CancelSceneGizmoDrag(); };
    context.cancelTerrainBrushStroke = [this] { CancelTerrainBrushStroke(); };
    context.clearTerrainBrushHit = [this] { ClearTerrainBrushHit(); };
    context.resetTextureView = [this] { ResetTextureView(); };
    context.resetTerrainLayerWindow = [this] { terrainLayerWindow = TerrainLayerWindowState{}; };
    context.landscapeToolActive = &landscapeToolActive;
    context.actorDragPath = &actorDragPath;
    context.textureViewSource = &textureViewSource;
    context.previewDragButton = &previewDragButton;
    context.runtimeControlsCurrent = &runtimeControlsCurrent;
    context.motionMatchingRowsMounted = &motionMatchingRowsMounted;
    context.motionMatchingVisible = &motionMatchingVisible;
    context.motionMatchingQueryExpanded = &motionMatchingQueryExpanded;
    context.motionMatchingQueryChannels = &motionMatchingQueryChannels;
    context.motionMatchingRefreshTime = &motionMatchingRefreshTime;
    context.motionMatchingRefreshKey = &motionMatchingRefreshKey;
    context.textureOptionsDirty = &textureOptionsDirty;
    context.textureOptionSelections = &textureOptionSelections;
    context.brushLibraryCatalogDomain = &brushLibraryCatalogDomain;
    context.axisIndicatorSignature = &axisIndicatorSignature;
    context.terrainLayerLibrarySignature = &terrainLayerLibrarySignature;
    context.terrainLayerSourceKey = &terrainLayerSourceKey;
    context.terrainLayerWindowDirty = &terrainLayerWindowDirty;
    context.terrainLayerScrollResetPending = &terrainLayerScrollResetPending;
    context.terrainLayerLastScrollTop = &terrainLayerLastScrollTop;
    context.outlinerMounted = &outlinerMounted;
    context.sceneGizmoMarkupMounted = &sceneGizmoMarkupMounted;
    context.contextMenuKind = &contextMenuKind;
    context.contextMenuTarget = &contextMenuTarget;
    return context;
}

void MaterialInstanceAssetEditorPanel::SetSceneSelectionSink(IMaterialInstanceSceneSelectionSink* sink)
{
    sceneSelectionSink = sink;
}

void MaterialInstanceAssetEditorPanel::SetRuntimeActionSink(IEditorRuntimeActionSink* sink)
{
    runtimeActionSink = sink;
}

void MaterialInstanceAssetEditorPanel::Submit(EditorCommandEnvelope command)
{
    if (commandSink == nullptr)
    {
        localStatus = "Editor command service is unavailable.";
        return;
    }
    // 0 由 CommandBus 分配全局唯一 ID，避免与运行时导航/预览生产者的编号冲突。
    command.commandId = 0;
    command.source = EditorCommandSource::RmlUi;
    commandSink->Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::SubmitPathCommand(EditorCommandType type)
{
    if (!snapshot.activeDocument) return;
    EditorCommandEnvelope command;
    command.type = type;
    command.expectedDocumentRevision = snapshot.activeDocument->revision;
    command.payload = MaterialInstanceAssetPathPayload{snapshot.activeDocument->assetPath};
    if (type == EditorCommandType::ApplyMaterialInstancePreview)
    {
        command.payload = ApplyMaterialInstancePreviewPayload{
            snapshot.activeDocument->assetPath, snapshot.activeDocument->revision};
    }
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::QueueEdit(std::string key, EditorCommandBatchItem item)
{
    if (!snapshot.activeDocument) return;
    if (pendingEdits.empty())
    {
        pendingAssetPath = snapshot.activeDocument->assetPath;
        pendingRevision = snapshot.activeDocument->revision;
    }
    // 同一字段一帧内多次 change 合并成最终值；不同字段仍按用户操作顺序组成原子 batch。
    for (auto it = pendingEdits.begin(); it != pendingEdits.end(); ++it)
    {
        if (it->key == key)
        {
            pendingEdits.erase(it);
            break;
        }
    }
    pendingEdits.push_back({std::move(key), std::move(item)});
}

void MaterialInstanceAssetEditorPanel::FlushPendingEdits(bool save)
{
    if (pendingEdits.empty())
    {
        if (save) SubmitPathCommand(EditorCommandType::SaveMaterialInstanceDocument);
        return;
    }
    EditorCommandEnvelope command;
    command.type = EditorCommandType::ExecuteEditorCommandBatch;
    command.expectedDocumentRevision = pendingRevision;
    ExecuteEditorCommandBatchPayload payload;
    for (auto& edit : pendingEdits) payload.commands.push_back(std::move(edit.item));
    if (save)
    {
        payload.commands.push_back({EditorCommandType::SaveMaterialInstanceDocument,
            MaterialInstanceAssetPathPayload{pendingAssetPath}});
    }
    command.payload = std::move(payload);
    Submit(std::move(command));
    pendingEdits.clear();
    pendingValues.clear();
    pendingAssetPath.clear();
}

void MaterialInstanceAssetEditorPanel::RequestSave()
{
    if (const auto* tab = resourceEditor.ActiveTab(); tab && tab->kind == EditorAssetKind::Terrain &&
        IsResourceEditorVisible())
    {
        const auto* terrain = TerrainDocument(tab->path);
        if (terrain == nullptr) return;
        EditorCommandEnvelope command;
        command.type = EditorCommandType::SaveTerrainDocument;
        command.expectedDocumentRevision = terrain->documentRevision;
        command.payload = MaterialInstanceAssetPathPayload{tab->path};
        Submit(std::move(command));
        return;
    }
    if (const auto* tab = resourceEditor.ActiveTab(); tab && tab->kind == EditorAssetKind::Blueprint &&
        IsResourceEditorVisible())
    {
        OpenSaveDialog(tab->path);
        return;
    }
    if (const auto* tab = resourceEditor.ActiveTab(); tab &&
        tab->kind == EditorAssetKind::ParticleSystem &&
        IsResourceEditorVisible())
    {
        OpenSaveDialog(tab->path);
        return;
    }
    OpenSaveDialog(snapshot.activeDocument && IsMaterialEditorVisible()
        ? snapshot.activeDocument->assetPath : std::string{});
}

void MaterialInstanceAssetEditorPanel::RequestSaveAll()
{
    OpenSaveDialog({});
}

void MaterialInstanceAssetEditorPanel::OpenSaveDialog(const std::string& path)
{
    if (HasModal()) return;
    if (snapshot.modifiedDocuments.empty() && pendingEdits.empty() && pendingBlueprintPath.empty() &&
        pendingParticlePath.empty() &&
        (!snapshot.activeDocument || snapshot.activeDocument->status == EditorDocumentStatus::Clean))
    {
        localStatus = "No modified assets.";
        return;
    }
    // 先提交未完成 batch，再从下一轮权威快照取得新 revision；弹窗打开本身不保存。
    saveAwaitingSnapshot = !pendingEdits.empty() || !pendingBlueprintPath.empty() ||
        !pendingParticleChanges.empty();
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
    FlushParticleAssetEdits();
    FlushParticleAssetEdits();
    saveFilterPath = path;
    saveDialogVisible = true;
    saveChecked.clear();
    saveKnownPaths.clear();
    saveWaiting.clear();
    saveHadFailure = false;
    saveMessage.clear();
    CloseMenus();
    CloseContextMenu();
    RefreshSaveDialog();
}

void MaterialInstanceAssetEditorPanel::RefreshSaveDialog()
{
    if (!document) return;
    WidgetRefreshScope refresh(refreshingControls);
    if (saveDialogVisible && !saveWaiting.empty())
    {
        for (const auto& feedback : snapshot.saveFeedback)
        {
            if (feedback.sequence <= saveStartedSequence ||
                saveWaiting.erase(feedback.assetPath) == 0) continue;
            if (!feedback.succeeded)
            {
                saveHadFailure = true;
                saveMessage += feedback.assetPath + ": " + feedback.message + "\n";
            }
        }
        if (saveWaiting.empty() && !saveHadFailure)
        {
            saveDialogVisible = false;
            if (jsonAssetRequestSink) jsonAssetRequestSink->RequestRefreshAssetCatalog();
        }
    }
    widgetDocument.SetProperty("save-dialog", "display", saveDialogVisible ? "block" : "none");
    if (!saveDialogVisible) return;
    saveRows.clear();
    for (const auto& asset : snapshot.modifiedDocuments)
        if (saveFilterPath.empty() || saveFilterPath == asset.assetPath)
            saveRows.push_back(asset);
    // 兼容旧文档快照生产者；实际 Runtime 始终发布跨类型 modifiedDocuments。
    if (saveRows.empty() && snapshot.modifiedDocuments.empty() && snapshot.activeDocument &&
        snapshot.activeDocument->status != EditorDocumentStatus::Clean &&
        (saveFilterPath.empty() || saveFilterPath == snapshot.activeDocument->assetPath))
        saveRows.push_back({snapshot.activeDocument->assetPath, EditorAssetKind::MaterialInstance,
            snapshot.activeDocument->revision, snapshot.activeDocument->status,
            snapshot.activeDocument->validationMessage});
    std::string markup;
    for (size_t index = 0; index < saveRows.size(); ++index)
    {
        const auto& asset = saveRows[index];
        if (saveKnownPaths.insert(asset.assetPath).second) saveChecked.insert(asset.assetPath);
        markup += "<div class=\"save-row\"><input type=\"checkbox\" id=\"save-file-" +
            std::to_string(index) + "\" data-action=\"save-file\" data-index=\"" + std::to_string(index) +
            "\"/><label class=\"save-path\" for=\"save-file-" +
            std::to_string(index) + "\">" + Text(asset.assetPath) + "</label><span class=\"save-state\">" +
            Text(DocumentStatus(asset.status)) + "</span></div>";
    }
    if (markup.empty()) markup = "<p>No modified assets.</p>";
    SetMarkup("save-document-list", std::move(markup));
    // 勾选/等待态原地同步，点击 checkbox 不销毁焦点节点。
    for (size_t index = 0; index < saveRows.size(); ++index)
    {
        const auto id = "save-file-" + std::to_string(index);
        if (auto* input = document->GetElementById(id))
        {
            const bool checked = saveChecked.count(saveRows[index].assetPath) != 0;
            if (checked && !input->HasAttribute("checked")) input->SetAttribute("checked", "checked");
            else if (!checked && input->HasAttribute("checked")) input->RemoveAttribute("checked");
        }
        SetDisabled(id, !saveWaiting.empty());
    }
    SetMarkup("save-document-status", Text(saveMessage));
    bool any = false;
    for (const auto& row : saveRows) any = any || saveChecked.count(row.assetPath) != 0;
    SetDisabled("save-confirm", !any || !saveWaiting.empty() || saveAwaitingSnapshot);
    SetDisabled("save-cancel", !saveWaiting.empty());
    SetDisabled("save-select-all", saveRows.empty() || !saveWaiting.empty());
}

void MaterialInstanceAssetEditorPanel::ConfirmSave()
{
    if (!saveWaiting.empty() || saveAwaitingSnapshot) return;
    saveHadFailure = false;
    saveMessage.clear();
    saveStartedSequence = 0;
    for (const auto& feedback : snapshot.saveFeedback)
        saveStartedSequence = std::max(saveStartedSequence, feedback.sequence);
    for (const auto& row : saveRows)
    {
        if (saveChecked.count(row.assetPath) == 0) continue;
        EditorCommandEnvelope command;
        command.type = row.kind == EditorAssetKind::Scene
            ? EditorCommandType::SaveSceneDocument : row.kind == EditorAssetKind::Mesh
            ? EditorCommandType::SaveMeshDocument : row.kind == EditorAssetKind::Tressfx
            ? EditorCommandType::SaveTressfxDocument : row.kind == EditorAssetKind::Texture
            ? EditorCommandType::SaveTextureDocument : row.kind == EditorAssetKind::Blueprint
            ? EditorCommandType::SaveBlueprintDocument : row.kind == EditorAssetKind::ParticleSystem
            ? EditorCommandType::SaveParticleDocument : row.kind == EditorAssetKind::Terrain
            ? EditorCommandType::SaveTerrainDocument : EditorCommandType::SaveMaterialInstanceDocument;
        command.expectedDocumentRevision = row.revision;
        command.payload = MaterialInstanceAssetPathPayload{row.assetPath};
        saveWaiting.insert(row.assetPath);
        Submit(std::move(command));
    }
    RefreshSaveDialog();
}

} // namespace VL::EditorUi

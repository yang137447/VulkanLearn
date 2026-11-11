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
        (!snapshot.activeDocument || snapshot.activeDocument->status == EditorDocumentStatus::Clean))
    {
        localStatus = "No modified assets.";
        return;
    }
    // 先提交未完成 batch，再从下一轮权威快照取得新 revision；弹窗打开本身不保存。
    saveAwaitingSnapshot = !pendingEdits.empty() || !pendingBlueprintPath.empty();
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
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
            ? EditorCommandType::SaveBlueprintDocument : row.kind == EditorAssetKind::Terrain
            ? EditorCommandType::SaveTerrainDocument : EditorCommandType::SaveMaterialInstanceDocument;
        command.expectedDocumentRevision = row.revision;
        command.payload = MaterialInstanceAssetPathPayload{row.assetPath};
        saveWaiting.insert(row.assetPath);
        Submit(std::move(command));
    }
    RefreshSaveDialog();
}

void MaterialInstanceAssetEditorPanel::Build(const MaterialInstanceEditorSnapshot& value)
{
    // 不把旧 UI 修改静默重定位到新文档或新 revision；CommandBus 负责拒绝过期 batch。
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
    scriptEditRequests.clear();
    textureOptionsDirty = textureOptionsDirty || !SameTextureOptions(snapshot.textureAssets, value.textureAssets);
    const std::string previousDocument = snapshot.selectedDocumentPath;
    snapshot = value;
    SynchronizeSceneGizmoSelection();
    SynchronizeTerrainBrushSelection();
    if (blueprintCommandAwaitingSnapshot)
    {
        const auto* authored = BlueprintDocument(blueprintCommandPath);
        if (!authored || authored->documentRevision != blueprintCommandRevision)
            blueprintCommandAwaitingSnapshot = false;
        // GT 完整 drain 后发布终态；失败不推进 revision，同样必须解除本地在途门禁。
        for (const auto& feedback : snapshot.blueprintCommandFeedback)
            if (feedback.assetPath == blueprintCommandPath &&
                feedback.sequence > blueprintCommandFeedbackSequence)
                blueprintCommandAwaitingSnapshot = false;
    }
    saveAwaitingSnapshot = false;
    // 文档集合由服务拥有，停靠标签只是投影；关卡/贴图页面不会被每帧刷新抢走。
    const bool closingMaterialActive = !closingMaterialPath.empty() &&
        docking.IsPanelActive(MaterialPanelId(closingMaterialPath));
    RefreshDocumentTabs();
    bool materialClosed = !closingMaterialPath.empty();
    for (const auto& tab : snapshot.documentTabs)
        if (tab.assetPath == closingMaterialPath) materialClosed = false;
    if (materialClosed)
    {
        // MI 关闭由 GT 确认；确认后才切邻页，不能被服务内部的 MI 选择覆盖跨类型邻居。
        const std::string next = std::move(panelAfterMaterialClose);
        closingMaterialPath.clear();
        panelAfterMaterialClose.clear();
        activateMaterialDocument = false;
        if (closingMaterialActive) ActivateWorkspacePanel(next);
    }
    else if ((activateMaterialDocument || previousDocument != snapshot.selectedDocumentPath) &&
        !snapshot.selectedDocumentPath.empty())
    {
        docking.SetPanelVisible(MaterialPanelId(snapshot.selectedDocumentPath), true);
        docking.Activate(MaterialPanelId(snapshot.selectedDocumentPath));
        activateMaterialDocument = false;
        viewingJson = false;
    }
    RefreshWorkspace();
    resourceEditorVisible = IsResourceEditorVisible();
    logPanel.Build(snapshot.logs, docking.IsPanelActive("log"));
    // 面板数据变化时顺带喂一次性能页（5 Hz 节流在性能页内部）；平时由
    // UiSubsystem 在到期窗口单独调用 UpdatePerformance，不触发整份面板重建。
    UpdatePerformance(snapshot.performance);
    // UI 修改命令已入队但新草稿尚未发布时，关闭流程必须等到权威 revision。
    const bool closingTressfxStaging = pendingCloseKind == EditorAssetKind::Tressfx &&
        hairSimulationPath == pendingClosePath && hairSimulationPendingSave;
    if (discardBlueprintThenClose &&
        (pendingCloseKind == EditorAssetKind::Blueprint || pendingCloseKind == EditorAssetKind::Tressfx))
    {
        if (!discardBlueprintCloseSubmitted && !closingTressfxStaging)
        {
            // 等上一帧的修改成为权威版本再回退，不能把旧 revision 的丢弃请求排在修改后面。
            SubmitResourceDocumentCommand(pendingCloseKind == EditorAssetKind::Tressfx
                ? EditorCommandType::RevertTressfxDocument : EditorCommandType::RevertBlueprintDocument,
                pendingClosePath);
            discardBlueprintCloseSubmitted = true;
        }
        else if (discardBlueprintCloseSubmitted)
        {
            const auto* actor = ResourceDocument(pendingClosePath);
            if (actor && !actor->dirty)
            {
                CloseDocument(pendingClosePath, EditorDirtyDocumentPolicy::RequireClean);
                pendingClosePath.clear();
            }
            // 拒绝或尚未确认时保留标签和弹窗，允许用户按新版本重试。
            discardBlueprintThenClose = discardBlueprintCloseSubmitted = false;
        }
    }
    else if (saveThenClose &&
        (pendingCloseKind == EditorAssetKind::Blueprint || pendingCloseKind == EditorAssetKind::Tressfx))
    {
        if (!saveCloseSubmitted && !closingTressfxStaging)
        {
            SubmitResourceDocumentCommand(pendingCloseKind == EditorAssetKind::Tressfx
                ? EditorCommandType::SaveTressfxDocument : EditorCommandType::SaveBlueprintDocument,
                pendingClosePath);
            saveCloseSubmitted = true;
        }
        else if (saveCloseSubmitted)
        {
            for (const auto& feedback : snapshot.saveFeedback)
                if (feedback.assetPath == pendingClosePath && feedback.sequence > saveStartedSequence)
                {
                    const auto* actor = ResourceDocument(pendingClosePath);
                    if (feedback.succeeded && actor && !actor->dirty)
                    {
                        CloseDocument(pendingClosePath, EditorDirtyDocumentPolicy::RequireClean);
                        pendingClosePath.clear();
                    }
                    saveThenClose = false;
                    saveCloseSubmitted = false;
                    break;
                }
        }
    }
    else if (saveThenClose && !saveCloseSubmitted && snapshot.activeDocument &&
        snapshot.activeDocument->assetPath == pendingClosePath)
    {
        SubmitPathCommand(EditorCommandType::SaveMaterialInstanceDocument);
        saveCloseSubmitted = true;
    }
    else if (saveThenClose && saveCloseSubmitted)
    {
        if (snapshot.activeDocument && snapshot.activeDocument->assetPath == pendingClosePath &&
            (snapshot.activeDocument->status == EditorDocumentStatus::SaveFailed ||
             snapshot.activeDocument->status == EditorDocumentStatus::SourceChanged))
        {
            saveThenClose = false;
            saveCloseSubmitted = false;
        }
        for (const auto& tab : snapshot.documentTabs)
        {
            if (saveThenClose && tab.assetPath == pendingClosePath && !tab.dirty)
            {
                CloseDocument(pendingClosePath, EditorDirtyDocumentPolicy::RequireClean);
                pendingClosePath.clear();
                saveThenClose = false;
                saveCloseSubmitted = false;
                break;
            }
        }
    }
    RefreshLists();
    RefreshDetails();
    RefreshSaveDialog();
    // 全局保存不受当前文档/只读资源页控制；单文件保存保留文档自身的可用性。
    const bool modified = !snapshot.modifiedDocuments.empty() ||
        (snapshot.activeDocument && snapshot.activeDocument->status != EditorDocumentStatus::Clean);
    SetDisabled("menu-file-save", !modified);
    SetDisabled("workspace-save", !modified);
    if (document != nullptr)
    {
        widgetDocument.SetProperty("close-dialog", "display",
            pendingClosePath.empty() ? "none" : "block");
        widgetDocument.SetProperty("material-instance-details", "display", viewingJson ? "none" : "block");
        widgetDocument.SetProperty("json-inspector", "display", viewingJson ? "block" : "none");
        if (viewingJson)
            jsonInspector.Build(snapshot.jsonDocument, requestedJsonPath, snapshot.jsonDocumentLoading);
        if (const auto* active = resourceEditor.ActiveTab(); active != nullptr &&
            snapshot.jsonDocument != nullptr && snapshot.jsonDocument->assetPath == active->path)
            resourceEditor.SetDocument(snapshot.jsonDocument, snapshot.jsonDocumentLoading);
        RefreshResourceEditor();
        RefreshMaterialPreview();
        SetMarkup("close-document-name", Text(pendingClosePath));
        SetMarkup("close-document-status", Text(snapshot.statusMessage));
        SetDisabled("close-save", saveThenClose || discardBlueprintThenClose);
        SetDisabled("close-discard", discardBlueprintThenClose ||
            (saveThenClose && (pendingCloseKind == EditorAssetKind::Blueprint ||
                pendingCloseKind == EditorAssetKind::Tressfx)));
    }
}

void MaterialInstanceAssetEditorPanel::SetMarkup(const std::string& id, std::string markup)
{
    Panel::SetPanelMarkup(widgetDocument, id, std::move(markup));
}

void MaterialInstanceAssetEditorPanel::SetControlValue(const std::string& id, const std::string& value)
{
    Panel::SetPanelControlValue(widgetDocument, id, value);
}

void MaterialInstanceAssetEditorPanel::SetDisabled(const std::string& id, bool disabled)
{
    Panel::SetPanelDisabled(widgetDocument, id, disabled);
}

void MaterialInstanceAssetEditorPanel::RefreshWorkspace()
{
    if (document == nullptr) return;
    WidgetRefreshScope refresh(refreshingControls);
    docking.Sync();
    RefreshMenus();
    RefreshViewportToolbar();
    RefreshTerrainSummary();
}

void MaterialInstanceAssetEditorPanel::RefreshLists()
{
    WidgetRefreshScope refresh(refreshingControls);
    RefreshOutliner();
    std::string slots;
    if (snapshot.selectedModel)
    {
        for (size_t index = 0; index < snapshot.selectedModel->materials.size(); ++index)
        {
            const auto& slot = snapshot.selectedModel->materials[index];
            slots += UiWidgets::ListRow({"slot", index}, slot.materialSlotName.empty()
                ? std::filesystem::path(slot.materialInstancePath).filename().string()
                : slot.materialSlotName,
                snapshot.selectedMaterialSlotIndex == slot.materialSlotIndex);
        }
    }
    SetMarkup("material-slots", std::move(slots));
    widgetDocument.SetProperty("material-slots-section", "display",
        snapshot.selectedModel && !snapshot.selectedModel->materials.empty() ? "block" : "none");
    contentBrowser.Build(snapshot, runtimeView.scripts.classes);
}

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

void MaterialInstanceAssetEditorPanel::OpenResourceAsset(
    const std::string& path, EditorAssetKind kind)
{
    FlushBlueprintAssetEdits();
    if (!resourceEditor.Open(path, kind))
    {
        localStatus = "Too many resource editors are open.";
        return;
    }
    viewingJson = false;
    resourceEditorVisible = true;
    localStatus.clear();
    RequestActiveResourceMetadata();
    // 作者 owner 的生命周期属于资产标签，不能依赖 Simulation 段是否可见。
    if (kind == EditorAssetKind::Tressfx)
        RequestTressfxDocument(path);
    if (kind == EditorAssetKind::Blueprint)
    {
        EditorCommandEnvelope command;
        command.type = EditorCommandType::GetBlueprintDocument;
        command.payload = MaterialInstanceAssetPathPayload{path};
        Submit(std::move(command));
    }
    if (kind == EditorAssetKind::Mesh || kind == EditorAssetKind::Tressfx)
    {
        // 毛发资产：目录扫描已带顶层 type 与 simulation.skinning，打开时即可决定
        // 是否请求预览。带 skinning 的资产不进入视口（由蓝图 TressFX 组件承载），
        // 这里直接跳过请求；快照到达后还会以作者文档复核一次。
        const EditorJsonAssetEntry* entry = FindCatalogEntry(snapshot.assetCatalog, path);
        hairPreviewPath = kind == EditorAssetKind::Tressfx ? path : std::string();
        hairPreviewBlocked = entry != nullptr && entry->tressfxSkinning;
        hairRotation.Reset();
        if (hairPreviewBlocked)
            localStatus = std::string(TressfxHairSkinnedAssetMessage);
        else if (runtimeActionSink != nullptr)
        {
            // 回调只投递路径；GT 复用 previewmodel 的 LoadWorld 事务，不在 UI 内加载资产。
            // 即使标签已打开也重新请求，保证切过关卡后再次双击仍能回到该模型。
            RequestModelPreview(path);
        }
    }
    RefreshResourceEditor();
    docking.SetPanelVisible(ResourcePanelId(path), true);
    docking.Activate(ResourcePanelId(path));
}

void MaterialInstanceAssetEditorPanel::RequestCloseResource(const std::string& path)
{
    for (const auto& tab : resourceEditor.Tabs())
    {
        if (tab.path != path) continue;
        const auto* authored = ResourceDocument(path);
        if ((authored && authored->dirty) || pendingBlueprintPath == path ||
            (hairSimulationPath == path && hairSimulationPendingSave))
        {
            FlushBlueprintAssetEdits();
            pendingClosePath = path;
            pendingCloseKind = tab.kind;
            saveThenClose = saveCloseSubmitted = false;
            discardBlueprintThenClose = discardBlueprintCloseSubmitted = false;
            return;
        }
        CloseResource(path);
        return;
    }
}

void MaterialInstanceAssetEditorPanel::CloseResource(const std::string& path)
{
    const std::string next = docking.PanelAfterClose(ResourcePanelId(path));
    resourceEditor.Close(path);
    requestedTressfxDocuments.erase(path);
    // 后台关闭只移除标签，不切预览 World，也不覆盖当前页面的选择。
    RefreshResourceEditor();
    if (!next.empty()) ActivateWorkspacePanel(next);
}

void MaterialInstanceAssetEditorPanel::ActivateWorkspacePanel(const std::string& panel)
{
    if (panel == "viewport")
    {
        docking.SetPanelVisible("viewport", true);
        ReturnToLevel();
        return;
    }
    const std::string path = docking.ResourcePathForPanel(panel);
    if (path.empty()) return;
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
    viewingJson = false;
    docking.SetPanelVisible(panel, true);
    docking.Activate(panel);
    if (panel.rfind("resource:material:", 0) == 0)
    {
        resourceEditorVisible = false;
        EditorCommandEnvelope command;
        command.type = EditorCommandType::SelectMaterialInstanceDocument;
        command.payload = MaterialInstanceAssetPathPayload{path};
        Submit(std::move(command));
        return;
    }
    if (!resourceEditor.Activate(path)) return;
    resourceEditorVisible = true;
    RequestActiveResourceMetadata();
    const auto* active = resourceEditor.ActiveTab();
    if (active && (active->kind == EditorAssetKind::Mesh || active->kind == EditorAssetKind::Tressfx) &&
        !HasCurrentModelPreview())
        RequestModelPreview(path);
    RefreshResourceEditor();
}

void MaterialInstanceAssetEditorPanel::RequestActiveResourceMetadata()
{
    const auto* active = resourceEditor.ActiveTab();
    if (active != nullptr && active->kind == EditorAssetKind::Terrain)
    {
        EditorCommandEnvelope command;
        command.type = EditorCommandType::GetTerrainDocument;
        command.payload = MaterialInstanceAssetPathPayload{active->path};
        Submit(std::move(command));
    }
    // 读取器只保留最新请求；切回未完成的标签须补读，已缓存的标签不重复 IO。
    if (active != nullptr && (active->snapshot == nullptr || active->loading) &&
        jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(active->path);
}

void MaterialInstanceAssetEditorPanel::RequestResourceReload()
{
    const auto* tab = resourceEditor.ActiveTab();
    if (tab && tab->kind == EditorAssetKind::Retarget && tab->snapshot)
    {
        // GT 的 loading 快照可能晚一帧；请求发出就封住旧图像，直到新元数据 revision 到达。
        reloadingRetargetPath = tab->path;
        reloadingRetargetRevision = tab->snapshot->revision;
        previewDragButton = -1;
    }
    if (tab && tab->kind == EditorAssetKind::Blueprint)
    {
        if (!pendingBlueprintPath.empty()) { localStatus = "Save or revert pending edits before reloading."; return; }
        SubmitBlueprintAssetCommand(EditorCommandType::ReloadBlueprintDocument, tab->path);
        return;
    }
    if (tab && tab->kind == EditorAssetKind::Terrain)
    {
        const auto* terrain = TerrainDocument(tab->path);
        if (terrain == nullptr) return;
        EditorCommandEnvelope command;
        command.type = EditorCommandType::ReloadTerrainDocument;
        command.expectedDocumentRevision = terrain->documentRevision;
        command.payload = ReloadMaterialInstanceDocumentPayload{
            tab->path, EditorDirtyDocumentPolicy::RequireClean};
        Submit(std::move(command));
        return;
    }
    if (tab && tab->kind == EditorAssetKind::Tressfx)
    {
        const auto* authored = TressfxDocument(tab->path);
        if (!authored || authored->dirty || hairSimulationPendingSave)
        {
            localStatus = "Save or revert pending edits before reloading.";
            return;
        }
        SubmitResourceDocumentCommand(EditorCommandType::ReloadTressfxDocument, tab->path);
        if (jsonAssetRequestSink != nullptr)
            jsonAssetRequestSink->RequestOpenJsonAsset(tab->path);
        return;
    }
    resourceEditor.RequestReload();
    const std::string path = resourceEditor.ConsumeReloadRequest();
    if (path.empty()) return;
    if (jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(path);
    const auto* active = resourceEditor.ActiveTab();
    if (active != nullptr && (active->kind == EditorAssetKind::Mesh || active->kind == EditorAssetKind::Tressfx))
        RequestModelPreview(path);
}

void MaterialInstanceAssetEditorPanel::RequestModelPreview(const std::string& path)
{
    // 打开、切标签与 Reload 共用入口；蒙皮资产只能由角色组件提供预览姿态。
    const auto* active = resourceEditor.ActiveTab();
    if (active && active->path == path && active->kind == EditorAssetKind::Tressfx)
    {
        const auto* entry = FindCatalogEntry(snapshot.assetCatalog, path);
        const bool blocked = active->snapshot ? active->snapshot->tressfxSkinning :
            entry != nullptr && entry->tressfxSkinning;
        if (blocked) return;
    }
    RequestAssetPreview(path, false);
}

void MaterialInstanceAssetEditorPanel::RequestAssetPreview(const std::string& path, bool material,
    const std::string& retargetPath, uint64_t retargetRevision)
{
    previewDragButton = -1;
    if (runtimeActionSink == nullptr) return;
    if (!runtimeView.activeWorldPath.empty() &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() != "SC_model_preview.json")
        levelWorldPath = runtimeView.activeWorldPath;
    // 新的预览目标从零姿态开始；旋转只属于当前预览世界，不跨资产累积。
    if (requestedPreviewPath != path) hairRotation.Reset();
    requestedPreviewPath = path;
    requestedRetargetPath = retargetPath;
    requestedRetargetRevision = retargetRevision;
    initializedRetargetGeneration = 0;
    requestedPreviewMaterial = material;
    requestedPreviewBlueprint = false;
    requestedPreviewGeneration = 0;
    for (const auto& actor : OutlinerActors())
        requestedPreviewGeneration = std::max(requestedPreviewGeneration, actor.worldGeneration);
    UiAction request;
    request.type = material ? UiActionType::PreviewMaterial : retargetPath.empty() ?
        UiActionType::PreviewModel : UiActionType::PreviewRetarget;
    request.stringValue = retargetPath.empty() ? path : retargetPath;
    if (!retargetPath.empty()) request.retargetPreviewModel = path;
    runtimeActionSink->RequestRuntimeAction(std::move(request));
}

bool MaterialInstanceAssetEditorPanel::IsResourceEditorVisible() const
{
    for (const auto& tab : resourceEditor.Tabs())
        if (docking.IsPanelActive(ResourcePanelId(tab.path))) return true;
    return false;
}

bool MaterialInstanceAssetEditorPanel::PrepareImmersiveView(std::string& levelToLoad)
{
    levelToLoad.clear();
    if (HasModal()) return false;
    const bool previewWorld = !requestedPreviewPath.empty() ||
        std::filesystem::path(runtimeView.activeWorldPath).filename() == "SC_model_preview.json";
    if (previewWorld)
    {
        if (levelWorldPath.empty())
        {
            localStatus = "Open a level before entering immersive mode.";
            return false;
        }
        levelToLoad = levelWorldPath;
        requestedPreviewPath.clear();
    }
    // 这里只选择目标页面；宿主等关卡事务完成后隐藏 UI，失败时仍保留可操作的工作区。
    resourceEditorVisible = false;
    docking.SetPanelVisible("viewport", true);
    docking.Activate("viewport");
    return true;
}

void MaterialInstanceAssetEditorPanel::ReturnToLevel()
{
    previewDragButton = -1;
    resourceEditorVisible = false;
    docking.Activate("viewport");
    // 当前只有一个活动 World；返回关卡须重新走加载事务，不能把模型 World 冒充关卡。
    if (!requestedPreviewPath.empty() && !levelWorldPath.empty() && runtimeActionSink != nullptr)
    {
        UiAction request;
        request.type = UiActionType::LoadWorld;
        request.stringValue = levelWorldPath;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
        requestedPreviewPath.clear();
    }
}

bool MaterialInstanceAssetEditorPanel::HasCurrentModelPreview() const
{
    return CurrentModelPreviewGeneration() != 0;
}

bool MaterialInstanceAssetEditorPanel::IsMotionDebugViewport() const
{
    return docking.IsPanelActive("viewport") ||
        (IsResourceEditorVisible() && HasCurrentModelPreview());
}

bool MaterialInstanceAssetEditorPanel::IsSceneViewportElement(const Rml::Element* element) const
{
    // 候选表可滚动，命中属于编辑器，不能同时触发角色输入或关卡拾取。
    for (auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
        if (current->GetId() == "viewport-motion-matching") return false;
    return docking.IsSceneViewportElement(element);
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentModelPreviewGeneration() const
{
    if (requestedPreviewMaterial) return 0;
    const auto* active = resourceEditor.ActiveTab();
    if (!active) return 0;
    if (active->kind == EditorAssetKind::Retarget)
    {
        if (!retargetEditor.AnimationVisible() || active->loading || !active->snapshot ||
            !active->snapshot->retargetPreview || active->path != requestedRetargetPath ||
            active->snapshot->revision != requestedRetargetRevision) return 0;
    }
    else if ((active->kind != EditorAssetKind::Mesh && active->kind != EditorAssetKind::Tressfx) ||
        active->path != requestedPreviewPath) return 0;
    // JSON 读取完成不能证明 GPU 模型已加载；只在新预览 World 的冻结快照发布后展示。
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration > requestedPreviewGeneration &&
            actor.sceneObjectIdentity == "preview_model" &&
            std::filesystem::path(actor.scenePath).filename() == "SC_model_preview.json")
        {
            if (active->kind == EditorAssetKind::Retarget)
            {
                bool paired = false;
                for (const auto& source : OutlinerActors())
                    paired |= source.worldGeneration == actor.worldGeneration &&
                        source.sceneObjectIdentity == "preview_retarget_source|" + requestedRetargetPath;
                if (!paired) continue;
            }
            return actor.worldGeneration;
        }
    return 0;
}

bool MaterialInstanceAssetEditorPanel::IsMaterialEditorVisible() const
{
    return snapshot.activeDocument &&
        docking.IsPanelActive(MaterialPanelId(snapshot.activeDocument->assetPath));
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentMaterialPreviewGeneration() const
{
    if (!requestedPreviewMaterial || !snapshot.activeDocument ||
        snapshot.activeDocument->assetPath != requestedPreviewPath) return 0;
    // 只认已发布的新 World 和同路径材质槽，切标签时不能把上一颗球的画面当成新材质。
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration > requestedPreviewGeneration &&
            actor.sceneObjectIdentity == "preview_material" &&
            std::filesystem::path(actor.scenePath).filename() == "SC_model_preview.json")
            for (const auto& section : actor.sections)
                if (section.materialInstancePath == requestedPreviewPath) return actor.worldGeneration;
    return 0;
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentPreviewCameraGeneration() const
{
    return IsMaterialEditorVisible() ? CurrentMaterialPreviewGeneration() :
        IsResourceEditorVisible() ? (requestedPreviewBlueprint ? CurrentBlueprintPreviewGeneration() :
            CurrentModelPreviewGeneration()) : 0;
}

void MaterialInstanceAssetEditorPanel::RefreshMaterialPreview()
{
    if (IsMaterialEditorVisible() && (!requestedPreviewMaterial ||
        requestedPreviewPath != snapshot.activeDocument->assetPath))
        RequestAssetPreview(snapshot.activeDocument->assetPath, true);
    // 最后一份材质关闭后，恢复记住的关卡；不能留下预览 World 冒充关卡。
    if (requestedPreviewMaterial && snapshot.documentTabs.empty() && docking.IsPanelActive("viewport"))
        ReturnToLevel();
    const uint64_t generation = CurrentMaterialPreviewGeneration();
    widgetDocument.SetProperty("material-preview-image", "display", generation ? "block" : "none");
    widgetDocument.SetProperty("material-preview-status", "display", generation ? "none" : "block");
    SetDisabled("material-frame", !generation);
    SetMarkup("material-preview-status", Text(snapshot.activeDocument && !snapshot.activeDocument->previewMessage.empty()
        ? snapshot.activeDocument->previewMessage : "Loading material preview..."));
    // 新球从磁盘基线加载；重连成功后补一次未保存草稿，避免切回标签丢失预览效果。
    if (generation && IsMaterialEditorVisible() && appliedMaterialPreviewGeneration != generation &&
        snapshot.activeDocument->preview == EditorPreviewStatus::Connected)
    {
        appliedMaterialPreviewGeneration = generation;
        if (snapshot.activeDocument->status == EditorDocumentStatus::Dirty)
            SubmitPathCommand(EditorCommandType::ApplyMaterialInstancePreview);
    }
}

void MaterialInstanceAssetEditorPanel::RequestPreviewCamera(ModelPreviewNavigation mode, float x, float y)
{
    const uint64_t generation = CurrentPreviewCameraGeneration();
    if (!runtimeActionSink || !generation || HasModal()) return;
    UiAction action;
    action.type = UiActionType::NavigateModelPreview;
    action.previewCamera = {mode, generation, x, y};
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

bool MaterialInstanceAssetEditorPanel::ProcessModelPreviewInput(Rml::Event& event)
{
    const uint64_t generation = CurrentPreviewCameraGeneration();
    if (!generation || HasModal() || docking.IsDragging())
    {
        previewDragButton = -1;
        return false;
    }
    if (previewDragGeneration != generation) previewDragButton = -1;
    const bool imageHit = event.GetTargetElement() == document->GetElementById(
        IsMaterialEditorVisible() ? "material-preview-image" :
            requestedPreviewBlueprint ? "blueprint-asset-preview-image" :
                !requestedRetargetPath.empty() ? "retarget-preview-image" : "resource-preview-image");
    const auto& type = event.GetType();
    const float x = event.GetParameter<float>("mouse_x", 0);
    const float y = event.GetParameter<float>("mouse_y", 0);
    if (hairGroupPaintActive && IsActiveHairPreview() && HairBrushSection() != nullptr)
    {
        if (type == "mousedown" && imageHit &&
            event.GetParameter<int>("button", -1) == 0)
        {
            CloseMenus();
            CloseContextMenu();
            if (hairLassoActive)
                BeginHairLassoStroke(x, y,
                    event.GetParameter<int>("shift_key", 0) != 0,
                    event.GetParameter<int>("alt_key", 0) != 0);
            else
                BeginHairBrushStroke(x, y,
                    event.GetParameter<int>("shift_key", 0) != 0,
                    event.GetParameter<int>("alt_key", 0) != 0);
            if (hairBrushStrokeActive)
            {
                previewDragButton = 0;
                previewDragGeneration = generation;
                previewPointerX = x;
                previewPointerY = y;
            }
            return true;
        }
        if (type == "mousemove")
        {
            if (hairLassoDragging)
            {
                AppendHairLassoPoint(x, y);
                // 每 3 个采样更新一次 ghost：整屏套索不必按鼠标频率重跑 40k 选择。
                if (hairLassoScreenPoints.size() % 3 == 0)
                    UpdateHairLassoPreview();
                return true;
            }
            if (hairBrushStrokeActive)
            {
                UpdateHairBrushSample(x, y, true);
                return true;
            }
            // 右键/中键相机拖动仍走原路径；hover 采样只在没有其它鼠标手势时生效。
            if (previewDragButton < 0 && imageHit)
            {
                UpdateHairBrushSample(x, y, false);
                return true;
            }
        }
        if (type == "mouseup" && hairLassoDragging &&
            event.GetParameter<int>("button", -1) == 0)
        {
            CommitHairLassoStroke();
            previewDragButton = -1;
            return true;
        }
        if (type == "mouseup" && hairBrushStrokeActive &&
            event.GetParameter<int>("button", -1) == 0)
        {
            CommitHairBrushStroke();
            previewDragButton = -1;
            return true;
        }
        if (type == "mousescroll" && imageHit)
        {
            // 滚轮仍归相机缩放；只有 Alt+滚轮才调笔刷半径。未按 Alt 时不消费事件，
            // 继续走下面通用的 RequestPreviewCamera(Zoom)。
            if (event.GetParameter<int>("alt_key", 0) != 0)
            {
                const float delta = event.GetParameter<float>("wheel_delta_y", 0);
                hairBrushRadiusPx = std::clamp(
                    hairBrushRadiusPx + delta * 4.0f, 1.0f, 200.0f);
                SetControlValue("hair-brush-radius", SceneActorNumber(hairBrushRadiusPx));
                hairBrushStatus = "Brush radius: " +
                    SceneActorNumber(hairBrushRadiusPx) + " px.";
                UpdateHairBrushSample(x, y, hairBrushStrokeActive);
                return true;
            }
        }
    }
    if (type == "mousedown" && imageHit)
    {
        const int button = event.GetParameter<int>("button", -1);
        if (button < 0 || button > 2) return false;
        CloseMenus();
        CloseContextMenu();
        previewDragButton = button;
        previewDragGeneration = generation;
        previewPointerX = x;
        previewPointerY = y;
        return true;
    }
    if (type == "mousemove" && previewDragButton >= 0)
    {
        // 手势归开始命中的图像所有；拖到 Details 上仍继续，到抬起/失焦才取消。
        const float dx = x - previewPointerX;
        const float dy = y - previewPointerY;
        previewPointerX = x;
        previewPointerY = y;
        if (dx != 0 || dy != 0)
        {
            // 毛发视口：左键转模型（转根点才会触发毛发惯性），中/右键仍转相机。
            // 相机绕轨道对物理没有作用，因此两条手势必须分开。
            if (previewDragButton == 0 && IsActiveHairPreview())
                RequestHairPreviewRotation(dx, dy);
            else
                RequestPreviewCamera(previewDragButton == 2 ? ModelPreviewNavigation::Pan : ModelPreviewNavigation::Orbit, dx, dy);
        }
        return true;
    }
    if (type == "mouseup" && previewDragButton == event.GetParameter<int>("button", -2))
    {
        previewDragButton = -1;
        return true;
    }
    if (type == "mousescroll" && imageHit)
    {
        RequestPreviewCamera(ModelPreviewNavigation::Zoom, 0, event.GetParameter<float>("wheel_delta_y", 0));
        return true;
    }
    if (type == "dblclick" && imageHit)
    {
        RequestPreviewCamera(ModelPreviewNavigation::Frame);
        return true;
    }
    return false;
}

void MaterialInstanceAssetEditorPanel::CloseDocument(
    const std::string& path, EditorDirtyDocumentPolicy policy)
{
    if (TressfxDocument(path))
    {
        if (policy == EditorDirtyDocumentPolicy::DiscardChanges)
            SubmitResourceDocumentCommand(EditorCommandType::RevertTressfxDocument, path);
        CloseResource(path);
        return;
    }
    if (BlueprintDocument(path))
    {
        if (policy == EditorDirtyDocumentPolicy::DiscardChanges)
            SubmitBlueprintAssetCommand(EditorCommandType::RevertBlueprintDocument, path);
        CloseResource(path);
        return;
    }
    closingMaterialPath = path;
    panelAfterMaterialClose = docking.PanelAfterClose(MaterialPanelId(path));
    EditorCommandEnvelope command;
    command.type = EditorCommandType::CloseMaterialInstanceAsset;
    command.payload = CloseMaterialInstanceAssetPayload{path, policy};
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::SelectModelMaterial(
    const MaterialInstanceModelSnapshot& modelSnapshot, size_t index)
{
    ShowMaterialDetails();
    if (sceneSelectionSink == nullptr || index >= modelSnapshot.materials.size()) return;
    FlushPendingEdits();
    const auto model = SelectionModel(modelSnapshot);
    const auto& slot = model.materials[index];
    Editor::Selection::MaterialInstanceSelection selection;
    selection.worldGeneration = model.worldGeneration;
    selection.scenePath = model.scenePath;
    selection.objectId = slot.objectId;
    selection.objectIdentity = model.objectIdentity;
    selection.materialSlotIndex = slot.materialSlotIndex;
    selection.materialSlotName = slot.materialSlotName;
    selection.materialInstancePath = slot.materialInstancePath;
    selection.modelContext = model;
    sceneSelectionSink->RequestMaterialSelection(selection);
}

void MaterialInstanceAssetEditorPanel::HandleClick(Rml::Element& element, const std::string& action)
{
    Panel::HandlePanelClick(*this, element, action);
}

void MaterialInstanceAssetEditorPanel::HandleChange(Rml::Element& element, const std::string& action,
    const Rml::Event* event)
{
    Panel::HandlePanelChange(*this, element, action, event);
}

void MaterialInstanceAssetEditorPanel::ProcessEvent(Rml::Event& event)
{
    if (refreshingControls || document == nullptr) return;
    const auto& eventType = event.GetType();
    if (eventType == "mousescroll")
    {
        // 滚轮默认滚动发生在捕获之后；这里只标脏，下一次 Build 再按新的 scrollTop 重算窗口。
        auto* layerScroll = document->GetElementById("terrain-layer-scroll");
        for (auto* element = event.GetTargetElement(); element != nullptr;
            element = element->GetParentNode())
        {
            if (element == layerScroll)
            {
                terrainLayerWindowDirty = true;
                break;
            }
        }
    }
    if (ProcessViewportShortcut(event))
    {
        event.StopPropagation();
        return;
    }
    if (eventType == "mousemove")
        RefreshTerrainLayerHoverPreview(event.GetUnprojectedMouseScreenPos(), event.GetTargetElement());
    auto* sceneImage = document->GetElementById("editor-viewport-image");
    auto* sceneSurface = document->GetElementById("editor-viewport-surface");
    if (IsTerrainSculptActive() && sceneImage != nullptr &&
        (event.GetTargetElement() == sceneImage ||
         event.GetTargetElement() == sceneSurface) &&
        (eventType == "mousedown" || eventType == "mousemove" || eventType == "mouseup"))
    {
        const int button = eventType == "mousemove" ? -1 :
            event.GetParameter<int>("button", -1);
        if (ProcessTerrainSculptPointer(eventType, button,
            event.GetParameter<float>("mouse_x", 0.0f),
            event.GetParameter<float>("mouse_y", 0.0f)))
        {
            event.StopPropagation();
            return;
        }
    }
    if (event.GetType() == "keydown" && !HasModal())
    {
        const int key = event.GetParameter<int>("key_identifier", 0);
        const bool ctrl = event.GetParameter<int>("ctrl_key", 0) != 0;
        if (ctrl && key == Rml::Input::KI_Z && IsActiveHairPreview() &&
            !hairPaintUndo.empty())
        {
            UndoLastHairBrushCommit();
            event.StopPropagation();
            return;
        }
        if (key == Rml::Input::KI_ESCAPE &&
            (hairGroupPaintActive || hairBrushStrokeActive))
        {
            CancelHairBrushStroke(true);
            event.StopPropagation();
            return;
        }
        if (hairGroupPaintActive && IsActiveHairPreview())
        {
            if (key == Rml::Input::KI_RETURN && hairBrushStrokeActive)
            {
                if (hairLassoDragging) CommitHairLassoStroke();
                else CommitHairBrushStroke();
                event.StopPropagation();
                return;
            }
            if (key == Rml::Input::KI_OEM_4 || key == Rml::Input::KI_OEM_6)
            {
                const float delta = key == Rml::Input::KI_OEM_6 ? 2.0f : -2.0f;
                hairBrushRadiusPx = std::clamp(hairBrushRadiusPx + delta, 2.0f, 500.0f);
                SetControlValue("hair-brush-radius", SceneActorNumber(hairBrushRadiusPx));
                hairBrushStatus = "Brush radius: " +
                    SceneActorNumber(hairBrushRadiusPx) + " px.";
                if (hairBrushStamp.valid)
                {
                    if (hairLassoDragging) SampleHairBrushStamp(hairBrushCursorX, hairBrushCursorY);
                    else UpdateHairBrushSample(hairBrushCursorX, hairBrushCursorY,
                        hairBrushStrokeActive);
                }
                event.StopPropagation();
                return;
            }
        }
    }
    // Groups 的行内表单是面板本地状态：Esc 只取消当前表单，不触发文档/World 命令。
    if (event.GetType() == "keydown" && !HasModal() &&
        event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE)
    {
        if (hairGroupCreateActive || hairGroupAddRangeActive)
        {
            hairGroupCreateActive = false;
            hairGroupCreateName.clear();
            hairGroupCreateStart.clear();
            hairGroupCreateEnd.clear();
            hairGroupCreateError.clear();
            hairGroupAddRangeActive = false;
            hairGroupRangeStart.clear();
            hairGroupRangeEnd.clear();
            hairGroupRangeError.clear();
            hairGroupStatus = "Group form cancelled.";
            event.StopPropagation();
            return;
        }
    }
    if (ProcessSceneGizmoInput(event))
    {
        event.StopPropagation();
        return;
    }
    if (ProcessBlueprintAssetDrag(event)) return;
    if (ProcessBlueprintViewportInput(event) || ProcessTexturePreviewInput(event) || ProcessModelPreviewInput(event))
    {
        event.StopPropagation();
        return;
    }
    // 属性行的文本框较窄，用户点标签/行空白时也应该进入编辑，而不是只选中行。
    Rml::Element* eventTarget = event.GetTargetElement();
    if (event.GetType() == "click" && eventTarget != nullptr)
    {
        Rml::Element* row = eventTarget;
        while (row != nullptr && row != document)
        {
            const std::string id = row->GetId();
            if (id.size() > 4 && id.compare(id.size() - 4, 4, "-row") == 0)
            {
                if (Rml::Element* input = FirstActorTextInput(*row))
                    input->Focus();
                break;
            }
            row = row->GetParentNode();
        }
    }
    if (event.GetType() == "change") runtimeControlsCurrent = false;
    // 点击菜单栏以外的任何位置都收起已展开的菜单；菜单项自身的收起在下面按 action 处理。
    if (event.GetType() == "click" && !openMenu.empty() && !IsInsideMenuBar(event.GetTargetElement()))
        CloseMenus();
    // 上下文菜单同一规则：点弹层以外收起，条目自身的收起在动作派发之后。
    if (event.GetType() == "click" && IsContextMenuOpen() &&
        !IsInsideContextMenu(event.GetTargetElement()))
        CloseContextMenu();
    if (docking.ProcessEvent(event, HasModal()))
    {
        const std::string activatedPanel = docking.ConsumeActivatedPanel();
        ActivateWorkspacePanel(activatedPanel);
        event.StopPropagation();
        return;
    }
    // 右键抬起在 RmlUi 里只是带 button=1 的 mouseup（没有 contextmenu 事件），
    // 因此必须在这里拦下来：下面的分支只处理 click/change/dblclick，右键会直接掉出去。
    if (event.GetType() == "mouseup" && event.GetParameter<int>("button", 0) == 1)
    {
        // 有模态弹窗时不打开菜单：About 与脏关闭弹窗存在期间工作区命令一律不可用。
        if (!HasModal()) OpenContextMenu(event);
        event.StopPropagation();
        return;
    }
    // 捕获阶段只处理工作区拖动；普通控件的鼠标默认行为必须继续到目标节点。
    if (event.GetType() != "click" && event.GetType() != "change" && event.GetType() != "dblclick") return;
    Rml::Element* element = event.GetTargetElement();
    // select 内部输入会先发 change，再由 select 发自己的 change；仅处理外部控件事件。
    if (event.GetType() == "change" && element && !element->HasAttribute("data-action")) return;
    while (element != nullptr && element != document && !element->HasAttribute("data-action"))
        element = element->GetParentNode();
    if (element == nullptr || element == document || element->HasAttribute("disabled")) return;
    const std::string action = element->GetAttribute<std::string>("data-action", "");
    if (HasModal() && action != "close-save" && action != "close-discard" &&
        action != "close-cancel" && action != "about-close" &&
        action != "save-file" && action != "save-confirm" && action != "save-cancel" &&
        action != "save-select-all")
        return;
    // checkbox 在目标节点的默认 click 中切换 checked；捕获阶段不能截断它。
    // 随后的 change 才把选择同步到弹窗状态，程序刷新仍由 WidgetRefreshScope 抑制。
    if (event.GetType() == "click" && (action == "save-file" || action == "script-physics-trigger" ||
        action == "blueprint-asset-instance" || action == "blueprint-asset-physics-trigger" ||
        action == "blueprint-asset-component-instance" || action == "blueprint-asset-snap-enabled" ||
        action == "viewport-snap-enabled" ||
        action == "log-autoscroll" || action == "retarget-all-bones" || action == "retarget-zoom" ||
        action == "animation-clip" || action == "animation-time" ||
        action == "motion-debug" || action == "debug-draw-enabled" || action == "debug-draw-category" ||
        action == "debug-draw-owner")) return;
    // 选中菜单条目后收起菜单；标题自己决定展开还是收起，不能在这里先清空。
    if (action != "menu-open" && action.rfind("debug-draw-", 0) != 0 && IsInsideMenuBar(element)) CloseMenus();
    try
    {
        std::optional<std::string> openAssetPath;
        const auto* activeResource = resourceEditor.ActiveTab();
        if (IsResourceEditorVisible() && activeResource &&
            activeResource->kind == EditorAssetKind::Retarget && retargetEditor.ProcessEvent(event))
        {
            RefreshResourceEditor();
            event.StopPropagation();
            return;
        }
        if (logPanel.ProcessEvent(event))
        {
            event.StopPropagation();
            return;
        }
        if (resourceJsonInspector.ProcessEvent(event))
        {
            event.StopPropagation();
            return;
        }
        if (jsonInspector.ProcessEvent(event))
        {
            event.StopPropagation();
            return;
        }
        if (contentBrowser.ProcessEvent(event, openAssetPath))
        {
            if (openAssetPath) OpenBrowserAsset(*openAssetPath);
        }
        else if (event.GetType() == "click") HandleClick(*element, action);
        else if (event.GetType() == "change") HandleChange(*element, action, &event);
        else if (event.GetType() == "dblclick" &&
            (action == "outliner-actor" || action == "viewport-actor") && runtimeActionSink != nullptr)
        {
            const size_t index = element->GetAttribute<size_t>("data-index", 0);
            if (index < OutlinerActors().size())
            {
                const auto& actor = OutlinerActors()[index];
                if (!SceneActorEditableFields(actor).empty())
                {
                    UiAction request;
                    request.type = UiActionType::FrameEditorActor;
                    request.actorProperty.worldGeneration = actor.worldGeneration;
                    request.actorProperty.identity = actor.identity;
                    runtimeActionSink->RequestRuntimeAction(std::move(request));
                }
            }
        }
        // 条目派发完成后才收起菜单：动作本身不读取"菜单是否打开"这个状态。
        // 禁用条目在上面就已返回，因此菜单保持打开（与菜单栏的禁用条目一致）。
        if (event.GetType() == "click" && IsInsideContextMenu(element)) CloseContextMenu();
    }
    catch (const std::exception& error)
    {
        localStatus = error.what();
    }
    event.StopPropagation();
}

} // namespace VL::EditorUi

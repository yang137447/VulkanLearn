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

void MaterialInstanceAssetEditorPanel::Build(const MaterialInstanceEditorSnapshot& value)
{
    // 不把旧 UI 修改静默重定位到新文档或新 revision；CommandBus 负责拒绝过期 batch。
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
    FlushParticleAssetEdits();
    scriptEditRequests.clear();
    textureOptionsDirty = textureOptionsDirty || !SameTextureOptions(snapshot.textureAssets, value.textureAssets);
    const std::string previousDocument = snapshot.selectedDocumentPath;
    snapshot = value;
    bool particleSettled = false;
    bool particleSucceeded = false;
    std::string particleMessage;
    for (const auto& feedback : snapshot.particleCommandFeedback)
    {
        if (feedback.sequence <= particleCommandFeedbackSequence)
            continue;
        // 非在途命令的终态也要推进序号，避免旧 modify 结果误结算新批次。
        particleCommandFeedbackSequence = feedback.sequence;
        if (particleHistoryCommandAwaitingSnapshot &&
            feedback.assetPath == particleHistoryCommandPath &&
            (feedback.type == EditorCommandType::UndoParticleDocument ||
             feedback.type == EditorCommandType::RedoParticleDocument))
        {
            particleHistoryCommandAwaitingSnapshot = false;
            if (!feedback.succeeded)
                localStatus = feedback.message.empty()
                    ? "Particle history command rejected."
                    : feedback.message;
        }
        if (!particleCommandAwaitingSnapshot ||
            feedback.type !=
                EditorCommandType::ModifyParticleDocument ||
            feedback.assetPath != particleCommandPath)
            continue;
        particleSettled = true;
        particleSucceeded = feedback.succeeded;
        particleMessage = feedback.message;
    }
    if (particleCommandAwaitingSnapshot)
    {
        if (particleSettled)
        {
            particleCommandAwaitingSnapshot = false;
            if (particleSucceeded)
                inFlightParticleChanges.clear();
            else
            {
                MergeParticleInFlightChanges();
                localStatus = particleMessage.empty()
                    ? "Particle edit rejected; changes kept for retry."
                    : particleMessage;
            }
        }
        else
        {
            // 反馈丢失或文档已消失时不能永久堵住后续编辑。
            const auto* authored =
                ParticleDocument(particleCommandPath);
            if (authored == nullptr)
            {
                particleCommandAwaitingSnapshot = false;
                inFlightParticleChanges.clear();
            }
            else if (authored->documentRevision >
                particleCommandRevision)
            {
                // 服务已推进版本但没有可匹配的终态；重新排队等同值变更，
                // 让用户输入保留下来而不是静默丢失。
                particleCommandAwaitingSnapshot = false;
                MergeParticleInFlightChanges();
            }
        }
    }
    if (particleHistoryCommandAwaitingSnapshot)
    {
        const auto* authored =
            ParticleDocument(particleHistoryCommandPath);
        if (authored == nullptr ||
            authored->documentRevision > particleHistoryCommandRevision)
            particleHistoryCommandAwaitingSnapshot = false;
    }
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
    else if (discardParticleThenClose &&
        pendingCloseKind == EditorAssetKind::ParticleSystem)
    {
        // 先等在途 modify 和尚未 flush 的字段落地，再按权威 revision 回退。
        if (!particleCommandAwaitingSnapshot &&
            !particleHistoryCommandAwaitingSnapshot &&
            pendingParticleChanges.empty())
        {
            if (!discardParticleCloseSubmitted)
            {
                SubmitParticleAssetCommand(
                    EditorCommandType::RevertParticleDocument,
                    pendingClosePath);
                discardParticleCloseSubmitted = true;
            }
            else
            {
                const auto* authored =
                    ParticleDocument(pendingClosePath);
                if (authored != nullptr && !authored->dirty)
                {
                    CloseDocument(pendingClosePath,
                        EditorDirtyDocumentPolicy::RequireClean);
                    pendingClosePath.clear();
                }
                // 拒绝时保留弹窗和标签，允许用户按新版本重试。
                discardParticleThenClose =
                    discardParticleCloseSubmitted = false;
            }
        }
    }
    else if (saveThenClose &&
        pendingCloseKind == EditorAssetKind::ParticleSystem)
    {
        if (!saveCloseSubmitted)
        {
            if (!particleCommandAwaitingSnapshot &&
                !particleHistoryCommandAwaitingSnapshot &&
                pendingParticleChanges.empty())
            {
                saveStartedSequence = 0;
                for (const auto& feedback : snapshot.saveFeedback)
                    saveStartedSequence = std::max(
                        saveStartedSequence, feedback.sequence);
                SubmitParticleAssetCommand(
                    EditorCommandType::SaveParticleDocument,
                    pendingClosePath);
                saveCloseSubmitted = true;
            }
        }
        else
        {
            for (const auto& feedback : snapshot.saveFeedback)
                if (feedback.assetPath == pendingClosePath &&
                    feedback.sequence > saveStartedSequence)
                {
                    const auto* authored =
                        ParticleDocument(pendingClosePath);
                    if (feedback.succeeded && authored &&
                        !authored->dirty)
                    {
                        CloseDocument(pendingClosePath,
                            EditorDirtyDocumentPolicy::RequireClean);
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
        widgetDocument.SetProperty("particle-revert-dialog", "display",
            pendingParticleRevertPath.empty() ? "none" : "block");
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
        SetMarkup("particle-revert-name",
            Text(pendingParticleRevertPath));
        SetMarkup("close-document-status", Text(snapshot.statusMessage));
        SetDisabled("close-save", saveThenClose ||
            discardBlueprintThenClose || discardParticleThenClose);
        SetDisabled("close-discard", discardBlueprintThenClose ||
            discardParticleThenClose ||
            (saveThenClose &&
             (pendingCloseKind == EditorAssetKind::Blueprint ||
              pendingCloseKind == EditorAssetKind::ParticleSystem ||
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

} // namespace VL::EditorUi

#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include "editor/particles/particleCurveEdit.h"
#include "editor/particles/particleEmitterEdit.h"
#include "editor/particles/particleFlipbookEdit.h"
#include "editor/particles/particleModuleEdit.h"
#include "editor/ui/panel/editorPanelInternalHelpers.h"
#include "editor/ui/panel/particlePanelHelpers.h"
#include "ui/widgets/text/textWidgets.h"

namespace VL::EditorUi
{

using namespace Panel::Detail;
using namespace ParticlePanelDetail;

const EditorAssetDocumentResultPayload*
MaterialInstanceAssetEditorPanel::ParticleDocument(
    const std::string& path) const
{
    for (const auto& entry : snapshot.particleDocuments)
        if (entry.assetPath == path) return &entry;
    return nullptr;
}

void MaterialInstanceAssetEditorPanel::QueueParticleAssetDraft(
    EditorAssetFieldChange change)
{
    const auto* tab = resourceEditor.ActiveTab();
    const auto* authored = tab ? ParticleDocument(tab->path) : nullptr;
    if (!authored) return;
    if (!pendingParticlePath.empty() &&
        pendingParticlePath != tab->path)
    {
        if (particleCommandAwaitingSnapshot ||
            particleHistoryCommandAwaitingSnapshot)
        {
            localStatus =
                "The previous particle edit is still being applied.";
            return;
        }
        FlushParticleAssetEdits();
        if (!pendingParticlePath.empty()) return;
    }
    pendingParticlePath = tab->path;
    pendingParticleRevision = authored->documentRevision;
    for (auto& pending : pendingParticleChanges)
    {
        if (pending.field == change.field)
        {
            pending = std::move(change);
            return;
        }
    }
    pendingParticleChanges.push_back(std::move(change));
}

void MaterialInstanceAssetEditorPanel::FlushParticleAssetEdits()
{
    if (pendingParticlePath.empty() ||
        pendingParticleChanges.empty())
    {
        return;
    }
    // 在途期间只累积 pending；上一笔终态到达后再提交，避免用旧 revision
    // 排队和下一条命令互相覆盖。
    if (particleCommandAwaitingSnapshot ||
        particleHistoryCommandAwaitingSnapshot)
    {
        return;
    }
    ModifyAssetDocumentPayload payload;
    payload.assetPath = pendingParticlePath;
    std::vector<EditorAssetFieldChange> changes =
        std::move(pendingParticleChanges);
    inFlightParticleChanges = changes;
    payload.changes = std::move(changes);
    EditorCommandEnvelope command;
    command.type = EditorCommandType::ModifyParticleDocument;
    command.expectedDocumentRevision = pendingParticleRevision;
    command.payload = std::move(payload);
    Submit(std::move(command));
    particleCommandPath = pendingParticlePath;
    particleCommandRevision = pendingParticleRevision;
    pendingParticlePath.clear();
    pendingParticleChanges.clear();
    particleCommandAwaitingSnapshot = true;
}

void MaterialInstanceAssetEditorPanel::MergeParticleInFlightChanges()
{
    for (auto& change : inFlightParticleChanges)
    {
        const bool newerPending = std::any_of(
            pendingParticleChanges.begin(), pendingParticleChanges.end(),
            [&change](const EditorAssetFieldChange& pending)
            {
                return pending.field == change.field;
            });
        // pending 代表用户在旧命令之后的新输入；同字段以新值为准。
        if (!newerPending)
            pendingParticleChanges.push_back(std::move(change));
    }
    inFlightParticleChanges.clear();
    if (pendingParticlePath.empty() && !particleCommandPath.empty())
        pendingParticlePath = particleCommandPath;
    if (!pendingParticlePath.empty() &&
        pendingParticlePath == particleCommandPath)
    {
        if (const auto* authored = ParticleDocument(pendingParticlePath))
            pendingParticleRevision = authored->documentRevision;
    }
}

void MaterialInstanceAssetEditorPanel::SubmitParticleAssetCommand(
    EditorCommandType type,
    const std::string& path)
{
    const auto* authored = ParticleDocument(path);
    if (!authored) return;
    EditorCommandEnvelope command;
    command.type = type;
    command.expectedDocumentRevision = authored->documentRevision;
    if (type == EditorCommandType::ReloadParticleDocument)
        command.payload = ReloadMaterialInstanceDocumentPayload{
            path, EditorDirtyDocumentPolicy::RequireClean};
    else
        command.payload = MaterialInstanceAssetPathPayload{path};
    Submit(std::move(command));
}

bool MaterialInstanceAssetEditorPanel::RequestParticleHistory(bool redo)
{
    const auto* tab = resourceEditor.ActiveTab();
    if (!tab || tab->kind != EditorAssetKind::ParticleSystem ||
        !IsResourceEditorVisible())
        return false;
    const auto* authored = ParticleDocument(tab->path);
    if (!authored || HasModal() || particleCommandAwaitingSnapshot ||
        particleHistoryCommandAwaitingSnapshot ||
        !pendingParticlePath.empty() ||
        !pendingParticleChanges.empty() ||
        !(redo ? authored->canRedo : authored->canUndo))
        return true;
    SubmitParticleAssetCommand(
        redo ? EditorCommandType::RedoParticleDocument
             : EditorCommandType::UndoParticleDocument,
        tab->path);
    particleHistoryCommandAwaitingSnapshot = true;
    particleHistoryCommandPath = tab->path;
    particleHistoryCommandRevision = authored->documentRevision;
    for (const auto& feedback : snapshot.particleCommandFeedback)
        particleCommandFeedbackSequence = std::max(
            particleCommandFeedbackSequence, feedback.sequence);
    RefreshParticleHistoryControls();
    return true;
}

void MaterialInstanceAssetEditorPanel::RefreshParticleHistoryControls()
{
    if (document == nullptr) return;
    const auto* tab = resourceEditor.ActiveTab();
    if (!tab || tab->kind != EditorAssetKind::ParticleSystem ||
        !IsResourceEditorVisible())
        return;
    const auto* authored = ParticleDocument(tab->path);
    const bool enabled = authored != nullptr &&
        !particleCommandAwaitingSnapshot &&
        !particleHistoryCommandAwaitingSnapshot &&
        pendingParticlePath.empty() &&
        pendingParticleChanges.empty();
    SetDisabled("particle-undo", !enabled || !authored->canUndo);
    SetDisabled("particle-redo", !enabled || !authored->canRedo);
    SetDisabled("menu-edit-undo", !enabled || !authored->canUndo);
    SetDisabled("menu-edit-redo", !enabled || !authored->canRedo);
}

std::string
MaterialInstanceAssetEditorPanel::ParticleEmitterNameForField(
    const std::string& field) const
{
    if (field.rfind("emitters.", 0) != 0)
        return {};
    const size_t indexStart = std::string("emitters.").size();
    const size_t separator = field.find('.', indexStart);
    if (separator == std::string::npos || separator == indexStart)
        return {};
    size_t index = 0;
    for (size_t cursor = indexStart; cursor < separator; ++cursor)
    {
        const char character = field[cursor];
        if (character < '0' || character > '9') return {};
        index = index * 10 + static_cast<size_t>(character - '0');
    }
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active ? ParticleDocument(active->path) : nullptr;
    if (authored == nullptr || !authored->document.is_object())
        return {};
    const auto& document = authored->document;
    if (!document.contains("emitters") ||
        !document.at("emitters").is_array() ||
        index >= document.at("emitters").size())
        return {};
    return document.at("emitters").at(index).value(
        "name", "emitter" + std::to_string(index));
}

void MaterialInstanceAssetEditorPanel::RequestParticlePreview(
    const std::string& path)
{
    if (runtimeActionSink == nullptr) return;
    const bool updateInPlace = HasParticlePreview();
    if (!updateInPlace &&
        !runtimeView.activeWorldPath.empty() &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() !=
            "SC_particle_preview.json" &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() !=
            "SC_model_preview.json")
    {
        levelWorldPath = runtimeView.activeWorldPath;
    }
    requestedPreviewPath = path;
    requestedPreviewMaterial = false;
    requestedPreviewBlueprint = false;
    requestedPreviewParticle = true;
    const auto* authored = ParticleDocument(path);
    UiAction request;
    request.type = updateInPlace
        ? UiActionType::UpdateParticlePreview
        : UiActionType::PreviewParticle;
    request.stringValue = path;
    if (authored != nullptr && authored->document.is_object())
        request.particleDocument = authored->document;
    runtimeActionSink->RequestRuntimeAction(std::move(request));
}

bool MaterialInstanceAssetEditorPanel::HasParticlePreview() const
{
    const auto* active = resourceEditor.ActiveTab();
    return requestedPreviewParticle &&
        active != nullptr &&
        active->kind == EditorAssetKind::ParticleSystem &&
        active->path == requestedPreviewPath &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() ==
            "SC_particle_preview.json";
}
}

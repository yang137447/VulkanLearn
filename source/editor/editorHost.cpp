#include "editor/editorHost.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "SDL3/SDL.h"
#include "commonFunction.h"
#include "editor/command/editorCommandBus.h"
#include "editor/command/editorCommandJsonCodec.h"
#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/scene/editorSceneActorBuilder.h"
#include "editor/scene/scriptBlueprintAuthoring.h"
#include "editor/service/sceneDocumentService.h"
#include "editor/terrain/terrainEditApply.h"
#include "editor/terrain/terrainEditSession.h"
#include "editor/terrain/terrainPaintSession.h"
#include "editor/terrain/terrainCompositedSculptApply.h"
#include "editor/ui/editorUiShell.h"
#include "editor/ui/materialInstanceEditorView.h"
#include "engine/blueprintPreviewRequest.h"
#include "engine/blueprintPreviewTransform.h"
#include "engine/diagnosticsSubsystem.h"
#include "engine/editorThumbnailRenderSession.h"
#include "engine/modelPreviewRequest.h"
#include "engine/particlePreviewRequest.h"
#include "engine/runtimeCommand.h"
#include "engine/runtimeCommandExecutor.h"
#include "engine/runtimeConfig.h"
#include "mesh/loader/common/meshAssetLoader.h"
#include "pipeline/pipelineFactory.h"
#include "platform/platformWindow.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/editor/editorViewportGrid.h"
#include "render/frontend/scenePicking.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererTerrainLoader.h"
#include "renderSystem.h"
#include "script/scriptRuntime.h"
#include "script/scriptSourceLocation.h"
#include "terrain/terrainPaintFile.h"
#include "terrain/terrainPaintManifest.h"
#include "render/resource/terrainCompositedService.h"
#include "editor/terrain/terrainCompositedAuthorMonitor.h"
#include "terrain/terrainCompositedDependencies.h"
#include "terrain/terrainCompositedRefresh.h"
#include "terrain/terrainCompositedVerifiedInputs.h"
#include "terrain/terrainVertexUpdateQueue.h"
#include "ui/uiEditorViewModelSections.h"
#include "ui/uiSubsystem.h"
#include "world/world.h"
#include "world/worldManager.h"

namespace VL
{
namespace
{

TerrainBrushShape ToTerrainBrushShape(int shape)
{
    if (shape == 0)
        return TerrainBrushShape::Circle;
    if (shape == 1)
        return TerrainBrushShape::Square;
    throw std::invalid_argument("Unknown terrain brush shape.");
}

bool IsScriptAuthoringAction(UiActionType type)
{
    switch (type)
    {
    case UiActionType::AddScriptBlueprint:
    case UiActionType::SetScriptProperty:
    case UiActionType::AddScriptComponent:
    case UiActionType::RemoveScriptComponent:
    case UiActionType::SetScriptLogicComponent:
    case UiActionType::AddScriptLogicComponent:
    case UiActionType::RemoveScriptLogicComponent:
    case UiActionType::RenameScriptBlueprint:
    case UiActionType::DuplicateScriptBlueprint:
    case UiActionType::DeleteScriptBlueprint:
    case UiActionType::SetScriptPhysics:
    case UiActionType::UndoSceneEdit:
    case UiActionType::RedoSceneEdit:
    case UiActionType::CreateScriptBlueprintAsset:
    case UiActionType::SetScriptComponent:
        return true;
    default:
        return false;
    }
}

} // namespace

class EditorHost::ConsoleSinkImpl final : public IEditorConsoleSink
{
public:
    explicit ConsoleSinkImpl(EditorHost& owner) : owner(owner) {}

    bool IsAvailable() const override
    {
        return owner.EditorRuntime() != nullptr;
    }

    Submission SubmitJson(std::string_view request) override
    {
        auto* runtime = owner.EditorRuntime();
        if (runtime == nullptr)
            throw std::runtime_error("Editor document host unavailable");
        const EditorCommandSubmission submitted = runtime->SubmitJson(request);
        Submission result;
        result.text = EditorCommandJsonCodec::EncodeResultText(submitted.result);
        result.commandId = submitted.commandId;
        result.queued = submitted.admission == EditorCommandAdmission::Queued;
        return result;
    }

    std::optional<std::string> PollResult(
        uint64_t commandId,
        bool& isFinal) override
    {
        auto* runtime = owner.EditorRuntime();
        if (runtime == nullptr)
        {
            isFinal = true;
            return std::nullopt;
        }
        const auto result = runtime->GetCommandResult(commandId);
        if (!result)
        {
            isFinal = false;
            return std::nullopt;
        }
        isFinal = IsFinalEditorCommandStatus(result->status);
        if (!isFinal)
            return std::nullopt;
        return EditorCommandJsonCodec::EncodeResultText(*result);
    }

private:
    EditorHost& owner;
};

EditorHost::EditorHost() = default;
EditorHost::~EditorHost() = default;

RuntimeResult<void> EditorHost::Initialize(const EditorHostInit& hostInit)
{
    init = hostInit;
    if (init.renderSystem == nullptr)
        init.renderSystem = &RenderSystem::GetInstance();

    if (init.ui != nullptr)
    {
        init.ui->SetEditorUiShell(nullptr);
        uiShell.reset();
        uiShell = std::make_unique<EditorUiShell>();
        auto shellResult = init.ui->SetEditorUiShell(uiShell.get());
        if (shellResult.IsFailure())
        {
            uiShell.reset();
            return shellResult;
        }
    }

    consoleSink = std::make_unique<ConsoleSinkImpl>(*this);
    viewportGrid = std::make_unique<EditorViewportGrid>();
    if (init.renderSystem != nullptr)
        init.renderSystem->SetEditorViewportOverlay(viewportGrid.get());
    initialized = true;
    return RuntimeResult<void>::Success();
}

void EditorHost::Shutdown()
{
    if (init.ui != nullptr)
        init.ui->SetEditorUiShell(nullptr);
    uiShell.reset();
    ReleaseRendererResources();
    if (init.renderSystem != nullptr)
        init.renderSystem->SetEditorViewportOverlay(nullptr);
    if (viewportGrid != nullptr && init.rendererBackend != nullptr)
        viewportGrid->Shutdown(*init.rendererBackend);
    viewportGrid.reset();
    ResetAuthoringSessions();
    pendingScriptBlueprintSelection.clear();
    initialized = false;
}

void EditorHost::ReleaseRendererResources()
{
    thumbnailSession.reset();
    thumbnailRequest.reset();
    thumbnailSessionEpoch = 0;
}

Editor::SceneDocumentService* EditorHost::SceneDocuments() const
{
    return uiShell != nullptr ? uiShell->SceneDocuments() : nullptr;
}

Editor::MaterialInstanceEditorRuntime* EditorHost::EditorRuntime() const
{
    return uiShell != nullptr ? uiShell->EditorRuntime() : nullptr;
}

void EditorHost::WaitForRenderIdle()
{
    if (init.waitForRenderThreadIdle)
        init.waitForRenderThreadIdle();
}

void EditorHost::QueueTerrainCompositedRefresh(
    World& world, TerrainCompositedService& service,
    const TerrainBuildPlan& plan, const TerrainCompositedDirtyChange& change,
    uint64_t sourceRevision)
{
    const auto key = std::make_pair(change.surfaceId, change.kind);
    const uint64_t sessionEpoch = change.kind == TerrainCompositedDependencyChange::Paint ?
        terrainPaintSessionEpoch : terrainEditSessionEpoch;
    const auto pending = terrainPendingRefreshes.find(key);
    if (pending != terrainPendingRefreshes.end() &&
        pending->second->command.worldGeneration == world.GetGeneration() &&
        pending->second->command.sourceRevision == sourceRevision &&
        pending->second->sessionEpoch == sessionEpoch)
        return;
    if (terrainRefreshRevision == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("Terrain editor refresh revision exhausted");
    auto command = TerrainCompositedRefreshCommand::Freeze(
        plan, change, world.GetGeneration(), ++terrainRefreshRevision, sourceRevision,
        service.RefreshRevision(world.GetGeneration(), change.surfaceId));
    const auto ticket = service.QueueRefresh(command);
    if (ticket.id == 0 || ticket.worldGeneration != command.worldGeneration ||
        ticket.editSeq != command.editSeq || ticket.surfaceId != change.surfaceId)
        throw std::runtime_error("Terrain service returned an invalid refresh ticket");
    terrainPendingRefreshes.insert_or_assign(key,
        std::make_shared<Editor::TerrainCompositedRefreshReceipt>(
            Editor::TerrainCompositedRefreshReceipt{std::move(command), ticket, sessionEpoch}));
}

std::vector<EditorHost::TerrainRefreshObservation> EditorHost::ObserveTerrainRefreshes(
    uint64_t worldGeneration, const std::string& surfaceId) const
{
    std::vector<TerrainRefreshObservation> observations;
    const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming || !streaming->composited)
        return observations;
    for (const auto& entry : terrainPendingRefreshes)
    {
        const auto& pending = *entry.second;
        if (pending.ticket.worldGeneration != worldGeneration ||
            pending.ticket.surfaceId != surfaceId)
            continue;
        if (observations.size() == 64)
            throw std::runtime_error("Terrain observation exceeds bounded receipt count");
        CanonicalFieldHasher input("editor-s4-v2-immutable-input");
        if (pending.command.inputs)
        {
            const auto& plan = *pending.command.inputs;
            if (plan.layerPaint)
            {
                input.AddBytes("weights", plan.layerPaint->weights.data(),
                    plan.layerPaint->weights.size() * sizeof(float));
                input.AddBytes("opacity", plan.layerPaint->opacity.data(), plan.layerPaint->opacity.size());
            }
            if (plan.heightmap)
                input.AddBytes("height", plan.heightmap->data.data(), plan.heightmap->data.size());
        }
        observations.push_back({streaming->composited->PollRefresh(pending.ticket),
            pending.command.sourceRevision, pending.command.expectedRevision,
            pending.attempts, pending.terminalFailure, input.Finalize()});
    }
    return observations;
}

void EditorHost::PollTerrainCompositedRefresh(World& world)
{
    const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
    if (!streaming || !streaming->composited)
        return;
    for (auto iterator = terrainPendingRefreshes.begin(); iterator != terrainPendingRefreshes.end();)
    {
        auto& pending = *iterator->second;
        if (pending.command.worldGeneration != world.GetGeneration())
        {
            iterator = terrainPendingRefreshes.erase(iterator);
            continue;
        }
        if (pending.terminalFailure)
        {
            ++iterator;
            continue;
        }
        const auto status = streaming->composited->PollRefresh(pending.ticket);
        pending.state = status.state;
        if (!status.Terminal())
        {
            ++iterator;
            continue;
        }
        if (status.Acknowledges(pending.ticket))
        {
            if (iterator->first.second == TerrainCompositedDependencyChange::Paint &&
                terrainPaintSession && pending.Acknowledges(status, world.GetGeneration(),
                    terrainPaintSessionEpoch, terrainPaintSession->Revision()) &&
                terrainPaintSession->PlacementId() == pending.ticket.surfaceId)
                terrainPaintSession->AcknowledgeCompositedDirty(pending.command.sourceRevision);
            if (iterator->first.second == TerrainCompositedDependencyChange::Sculpt &&
                terrainEditSession && pending.Acknowledges(status, world.GetGeneration(),
                    terrainEditSessionEpoch, terrainEditSession->Revision()) &&
                terrainEditSession->PlacementId() == pending.ticket.surfaceId)
                terrainEditSession->AcknowledgeCompositedDirty(pending.command.sourceRevision);
        }
        else
        {
            if (!status.diagnostic.empty() && init.diagnostics)
                init.diagnostics->ReportWarning(status.diagnostic, "TerrainVT");
            if (pending.attempts < 3 &&
                terrainRefreshRevision != std::numeric_limits<uint64_t>::max())
            {
                try
                {
                    auto retry = pending.command;
                    const auto& placements = world.GetTerrainCollisionInstances();
                    const auto placement = std::find_if(placements.begin(), placements.end(),
                        [&pending](const TerrainCollisionInstance& candidate) {
                            return candidate.placementId == pending.ticket.surfaceId;
                        });
                    if (placement == placements.end())
                    {
                        pending.terminalFailure = true;
                        ++iterator;
                        continue;
                    }
                    retry.inputs = std::make_shared<const TerrainBuildPlan>(placement->plan);
                    retry.expectedRevision = streaming->composited->RefreshRevision(
                        retry.worldGeneration, pending.ticket.surfaceId);
                    retry.editSeq = ++terrainRefreshRevision;
                    const auto ticket = streaming->composited->QueueRefresh(retry);
                    if (!ticket.id || ticket.worldGeneration != retry.worldGeneration ||
                        ticket.editSeq != retry.editSeq || ticket.surfaceId != pending.ticket.surfaceId)
                        throw std::runtime_error("Terrain service returned an invalid retry ticket");
                    pending.command = std::move(retry);
                    pending.ticket = ticket;
                    pending.state = TerrainCompositedRefreshState::Queued;
                    ++pending.attempts;
                }
                catch (...)
                {
                    pending.terminalFailure = true;
                    throw;
                }
            }
            else
                pending.terminalFailure = true;
            ++iterator;
            continue;
        }
        iterator = terrainPendingRefreshes.erase(iterator);
    }
}

void EditorHost::MarkTerrainAuthorSaved(
    const std::string& placementId, TerrainCompositedDependencyChange kind)
{
    if (!terrainAuthorMonitor)
        return;
    Editor::TerrainCompositedAuthorMonitor::SurfaceStatus status;
    if (terrainAuthorMonitor->GetStatus(placementId, status))
        terrainAuthorMonitor->MarkSaved(placementId, kind, status.editRevision, status.generation);
}

void EditorHost::ResetAuthoringSessions()
{
    terrainEditSession.reset();
    terrainEditSessionGeneration = 0;
    terrainEditSessionPlacementId.clear();
    terrainPaintSession.reset();
    terrainPaintSessionGeneration = 0;
    terrainPaintSessionPlacementId.clear();
    terrainSculptApplies.clear();
    terrainSculptResidencyRevisions.clear();
    terrainPendingRefreshes.clear();
    terrainEditNeedsRebuild = false;
}

void EditorHost::BeginUiActionBatch()
{
    terrainEditNeedsRebuild = terrainEditNeedsRebuild ||
        (terrainEditSession && terrainEditSession->CompositedDirty().valid);
}

bool EditorHost::ApplyUiAction(
    const UiAction& action,
    World* world,
    RuntimeCommandExecutionResult&)
{
    switch (action.type)
    {
    case UiActionType::TerrainEdit:
        return ApplyTerrainEditAction(action, world);
    case UiActionType::TerrainPaint:
        return ApplyTerrainPaintAction(action, world);
    case UiActionType::OpenScriptSource:
        return ApplyOpenScriptSource(action);
    case UiActionType::CreateScriptBlueprintAsset:
        if (action.script.contains("class"))
        {
            auto& scripts = *init.scripts;
            std::string error;
            try
            {
                if (!scripts.IsEditing())
                    throw std::runtime_error("Stop gameplay before creating Blueprints");
                auto* documents = SceneDocuments();
                if (documents == nullptr)
                    throw std::runtime_error("Blueprint authoring is unavailable");
                if (documents->CreateScriptBlueprintFromClass(
                        action.stringValue,
                        action.script.at("class").get<std::string>(),
                        action.script.at("name").get<std::string>(),
                        scripts.Classes(),
                        error))
                {
                    scripts.SetFeedback("Blueprint asset created: " + action.stringValue);
                    if (init.ui != nullptr)
                        init.ui->RequestRefreshAssetCatalog();
                    return true;
                }
            }
            catch (const std::exception& exception)
            {
                error = exception.what();
            }
            scripts.SetFeedback(error);
            if (!error.empty() && init.diagnostics != nullptr)
                init.diagnostics->ReportWarning(error, "Script");
            return true;
        }
        return ApplyScriptAuthoringAction(action, world);
    case UiActionType::TransformBlueprintPreview:
        if (world != nullptr && init.ui != nullptr &&
            uiShell != nullptr && uiShell->EditorRuntime() != nullptr &&
            init.scripts != nullptr && init.scripts->IsEditing() &&
            init.renderSystem != nullptr &&
            !init.renderSystem->IsOfflineMode())
        {
            for (const auto& document :
                 uiShell->EditorRuntime()->GetSnapshot().blueprintDocuments)
            {
                if (document.assetPath == action.blueprintTransform.assetPath)
                {
                    ApplyBlueprintPreviewTransform(
                        *world,
                        action.blueprintTransform,
                        document.documentRevision,
                        document.document);
                    break;
                }
            }
        }
        return true;
    case UiActionType::FrameEditorActor:
        if (world != nullptr)
            FrameEditorActor(
                *world,
                action.actorProperty.worldGeneration,
                action.actorProperty.identity);
        return true;
    case UiActionType::PreviewBlueprint:
    case UiActionType::PreviewMaterial:
    case UiActionType::PreviewParticle:
    case UiActionType::UpdateParticlePreview:
    case UiActionType::PreviewModel:
    case UiActionType::PreviewRetarget:
        return ApplyPreviewAction(action, world);
    default:
        if (IsScriptAuthoringAction(action.type))
            return ApplyScriptAuthoringAction(action, world);
        return false;
    }
}

void EditorHost::EndUiActionBatch(World* world)
{
    if (!terrainEditNeedsRebuild || !terrainEditSession)
        return;
    if (world == nullptr || world->GetGeneration() != terrainEditSessionGeneration)
    {
        terrainEditSession.reset();
        terrainEditSessionGeneration = 0;
        terrainEditSessionPlacementId.clear();
        terrainEditNeedsRebuild = false;
        return;
    }
    if (init.rendererBackend == nullptr)
    {
        if (init.diagnostics != nullptr)
            init.diagnostics->ReportWarning(
                "Terrain sculpt CPU edit kept dirty, but no renderer backend is available.",
                "Terrain");
        return;
    }
    if (ApplyTerrainEditUpdate(*world))
        terrainEditNeedsRebuild = false;
}

bool EditorHost::ApplyTerrainEditAction(const UiAction& action, World* world)
{
    auto& scripts = *init.scripts;
    std::string error;
    if (world == nullptr)
        error = "Terrain sculpt requires an active World.";
    else if (action.terrainBrush.worldGeneration != world->GetGeneration())
        error = "The World changed since this terrain brush event.";
    else if (!scripts.IsEditing())
        error = "Stop gameplay before sculpting terrain.";
    else if (init.renderSystem->IsOfflineMode() ||
        std::filesystem::path(world->GetScenePath()).filename() ==
            "SC_model_preview.json")
        error = "Terrain sculpt requires a realtime authoring level.";

    const TerrainCollisionInstance* instance = nullptr;
    if (error.empty())
    {
        for (const TerrainCollisionInstance& candidate :
             world->GetTerrainCollisionInstances())
            if (candidate.placementId == action.terrainBrush.placementId)
            {
                instance = &candidate;
                break;
            }
        if (instance == nullptr)
            error = "Terrain placement was not found in the active World: " +
                action.terrainBrush.placementId;
    }

    if (error.empty() &&
        (!terrainEditSession ||
         terrainEditSessionGeneration != world->GetGeneration() ||
         terrainEditSessionPlacementId != action.terrainBrush.placementId))
    {
        // 同一 drain 内从 A 切到 B 或结束 A 后立刻 Begin B 时，先把
        // A 的 dirty 预览/取消结果提交到 GPU，不能只保留 B 的新 session。
        if (terrainEditSession && init.rendererBackend != nullptr &&
            terrainEditSessionGeneration == world->GetGeneration() &&
            terrainEditNeedsRebuild)
        {
            if (!ApplyTerrainEditUpdate(*world))
                error = "Previous terrain sculpt update failed; retry after the renderer recovers.";
            else
            {
                // 旧 session 的 CPU dirty 已经交给 GPU/刷新票据；切到新 session 后
                // BuildUpdate 不再包含这些 chunk，若不在此清除，新 session 会被
                // 误判为仍有未应用编辑（SaveHeightmap 随后对空 dirty 再 Apply 一次
                // 并静默失败）。BeginUiActionBatch 也会因未 ack 的 composited dirty
                // 重新置位该标记，因此成功路径必须在这里消费掉它。
                terrainEditNeedsRebuild = false;
            }
        }
        if (error.empty())
        {
            terrainEditSession.reset();
            auto candidate = std::make_unique<Editor::TerrainEditSession>();
            if (!Editor::TerrainEditSession::Create(*instance, *candidate, error))
            {
                candidate.reset();
            }
            else
            {
                terrainEditSession = std::move(candidate);
                terrainEditSessionEpoch = ++terrainSessionEpoch;
                terrainEditSessionGeneration = world->GetGeneration();
                terrainEditSessionPlacementId = action.terrainBrush.placementId;
            }
        }
    }

    if (error.empty() && terrainEditSession)
    {
        const auto operation = action.terrainBrush.operation;
        try
        {
            if (operation == TerrainBrushOperation::BeginStroke)
            {
                if (!terrainEditSession->BeginStroke())
                    error = "A terrain sculpt stroke is already active.";
            }
            else if (operation == TerrainBrushOperation::ApplyBrush)
            {
                if (!terrainEditSession->IsStrokeActive())
                    error = "Terrain brush received ApplyBrush without BeginStroke.";
                else
                {
                    TerrainSculptBrush brush;
                    if (action.terrainBrush.mode == 0)
                        brush.mode = TerrainSculptMode::Raise;
                    else if (action.terrainBrush.mode == 1)
                        brush.mode = TerrainSculptMode::Lower;
                    else if (action.terrainBrush.mode == 2)
                        brush.mode = TerrainSculptMode::Smooth;
                    else
                        throw std::invalid_argument("Unknown terrain sculpt mode.");
                    brush.shape =
                        ToTerrainBrushShape(action.terrainBrush.shape);
                    brush.centerUv = Eigen::Vector2f(
                        action.terrainBrush.u, action.terrainBrush.v);
                    brush.radiusUv = action.terrainBrush.radiusUv;
                    brush.strength = action.terrainBrush.strength;
                    brush.falloff = action.terrainBrush.falloff;
                    // 半径外/clamp 后无数值变化不是错误；一次 drain 内只做
                    // 一次 GPU 重建，拖动期间仍能看到实时地形预览。
                    if (terrainEditSession->ApplyBrush(brush))
                        terrainEditNeedsRebuild = true;
                }
            }
            else if (operation == TerrainBrushOperation::EndStroke)
            {
                if (!terrainEditSession->IsStrokeActive())
                    error = "Terrain brush received EndStroke without BeginStroke.";
                else if (terrainEditSession->EndStroke())
                    terrainEditNeedsRebuild = true;
            }
            else if (operation == TerrainBrushOperation::CancelStroke)
            {
                if (!terrainEditSession->IsStrokeActive())
                    error = "Terrain brush received CancelStroke without BeginStroke.";
                else
                {
                    const uint64_t revision = terrainEditSession->Revision();
                    terrainEditSession->CancelStroke();
                    // 未改过高度的空笔画 Cancel 不应支付一次全地形上传。
                    terrainEditNeedsRebuild = terrainEditNeedsRebuild ||
                        terrainEditSession->Revision() != revision;
                }
            }
            else if (operation == TerrainBrushOperation::Undo)
            {
                if (!terrainEditSession->Undo())
                    error = "No terrain stroke is available to undo.";
                else
                    terrainEditNeedsRebuild = true;
            }
            else if (operation == TerrainBrushOperation::Redo)
            {
                if (!terrainEditSession->Redo())
                    error = "No terrain stroke is available to redo.";
                else
                    terrainEditNeedsRebuild = true;
            }
            else if (operation == TerrainBrushOperation::SaveHeightmap)
            {
                if (terrainEditSession->IsStrokeActive())
                    throw std::runtime_error("End the terrain sculpt stroke before saving");
                if (terrainEditNeedsRebuild)
                {
                    if (!ApplyTerrainEditUpdate(*world))
                        throw std::runtime_error("Apply pending terrain sculpt edits before saving");
                    terrainEditNeedsRebuild = false;
                }
                const std::filesystem::path path =
                    std::filesystem::path(CommonFunction::GetResourcePath()) /
                    terrainEditSession->Desc().heightmapPath;
                if (terrainEditSession->SaveHeightmap(path, error))
                {
                    terrainUnsavedHeight.erase(terrainEditSessionPlacementId);
                    MarkTerrainAuthorSaved(terrainEditSessionPlacementId,
                        TerrainCompositedDependencyChange::Sculpt);
                    scripts.SetFeedback("Heightmap saved: " + path.string());
                }
            }
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
        }
    }
    if (!error.empty())
    {
        scripts.SetFeedback(error);
        if (init.diagnostics != nullptr)
            init.diagnostics->ReportWarning(error, "Terrain");
    }
    return true;
}

bool EditorHost::ApplyTerrainPaintAction(const UiAction& action, World* world)
{
    auto& scripts = *init.scripts;
    std::string error;
    if (world == nullptr)
        error = "Terrain paint requires an active World.";
    else if (action.terrainPaint.worldGeneration != world->GetGeneration())
        error = "The World changed since this terrain paint event.";
    else if (!scripts.IsEditing())
        error = "Stop gameplay before painting terrain layers.";
    else if (init.renderSystem->IsOfflineMode() ||
        std::filesystem::path(world->GetScenePath()).filename() ==
            "SC_model_preview.json")
        error = "Terrain paint requires a realtime authoring level.";

    const TerrainCollisionInstance* instance = nullptr;
    if (error.empty())
    {
        for (const TerrainCollisionInstance& candidate :
             world->GetTerrainCollisionInstances())
            if (candidate.placementId == action.terrainPaint.placementId)
            {
                instance = &candidate;
                break;
            }
        if (instance == nullptr)
            error = "Terrain placement was not found in the active World: " +
                action.terrainPaint.placementId;
    }
    if (error.empty() &&
        (!terrainPaintSession ||
         terrainPaintSessionGeneration != world->GetGeneration() ||
         terrainPaintSessionPlacementId != action.terrainPaint.placementId))
    {
        terrainPaintSession.reset();
        auto candidate = std::make_unique<Editor::TerrainPaintSession>();
        if (!Editor::TerrainPaintSession::Create(*instance, *candidate, error))
            candidate.reset();
        else
        {
            terrainPaintSession = std::move(candidate);
            terrainPaintSessionEpoch = ++terrainSessionEpoch;
            terrainPaintSessionGeneration = world->GetGeneration();
            terrainPaintSessionPlacementId = action.terrainPaint.placementId;
        }
    }

    bool invalidate = false;
    if (error.empty() && terrainPaintSession)
    {
        const auto operation = action.terrainPaint.operation;
        try
        {
            if (operation == TerrainBrushOperation::BeginStroke)
            {
                if (!terrainPaintSession->BeginStroke())
                    error = "A terrain paint stroke is already active.";
            }
            else if (operation == TerrainBrushOperation::ApplyBrush)
            {
                if (!terrainPaintSession->IsStrokeActive())
                    error = "Terrain paint received ApplyBrush without BeginStroke.";
                else
                {
                    Editor::TerrainPaintBrush brush;
                    brush.layerIndex = action.terrainPaint.layerIndex;
                    brush.shape =
                        ToTerrainBrushShape(action.terrainPaint.shape);
                    brush.centerUv = Eigen::Vector2f(
                        action.terrainPaint.u, action.terrainPaint.v);
                    brush.radiusUv = action.terrainPaint.radiusUv;
                    brush.strength = action.terrainPaint.strength;
                    brush.falloff = action.terrainPaint.falloff;
                    brush.opacity = action.terrainPaint.opacity;
                    brush.flow = action.terrainPaint.flow;
                    brush.noise = action.terrainPaint.noise;
                    brush.noiseSeed = action.terrainPaint.noiseSeed;
                    brush.erase = action.terrainPaint.erase;
                    if (terrainPaintSession->ApplyBrush(brush))
                        invalidate = true;
                }
            }
            else if (operation == TerrainBrushOperation::EndStroke)
            {
                if (!terrainPaintSession->IsStrokeActive())
                    error = "Terrain paint received EndStroke without BeginStroke.";
                else invalidate = terrainPaintSession->EndStroke();
            }
            else if (operation == TerrainBrushOperation::CancelStroke)
            {
                if (!terrainPaintSession->IsStrokeActive())
                    error = "Terrain paint received CancelStroke without BeginStroke.";
                else
                {
                    const uint64_t revision = terrainPaintSession->Revision();
                    terrainPaintSession->CancelStroke();
                    invalidate = terrainPaintSession->Revision() != revision;
                }
            }
            else if (operation == TerrainBrushOperation::Undo)
            {
                if (!terrainPaintSession->Undo())
                    error = "No terrain paint stroke is available to undo.";
                else invalidate = true;
            }
            else if (operation == TerrainBrushOperation::Redo)
            {
                if (!terrainPaintSession->Redo())
                    error = "No terrain paint stroke is available to redo.";
                else invalidate = true;
            }
            else if (operation == TerrainBrushOperation::SavePaint)
            {
                if (terrainPaintSession->IsStrokeActive())
                    error = "Finish or cancel the paint stroke before saving.";
                else
                {
                    const auto paint = terrainPaintSession->PaintData();
                    if (!paint || paint->Empty())
                        error = "Terrain paint has no edited data to save.";
                    else if (terrainPaintSession->PaintPath().empty())
                        error =
                            "TR has no paintPath; add one to persist terrain paint.";
                    else
                    {
                        std::filesystem::path path;
                        std::string pathError;
                        if (!ResolveTerrainPaintPath(
                                CommonFunction::GetResourcePath(),
                                terrainPaintSession->PaintPath(),
                                path,
                                pathError))
                            error = pathError;
                        else
                        {
                            // 已存在的 sidecar 保留 tile 身份与 feather；编辑器只
                            // 更新 paint 形状、层 key 和本次实际字节的哈希。
                            TerrainPaintManifest manifest;
                            manifest.version = 2;
                            manifest.tileX = 0;
                            manifest.tileY = 0;
                            manifest.tileGrid = 1;
                            manifest.featherTexels = 0.0f;
                            std::string manifestError;
                            if (TerrainPaintManifestExists(
                                    path, manifestError))
                            {
                                if (!LoadTerrainPaintManifest(
                                        path, manifest, manifestError))
                                    error = manifestError;
                            }
                            else if (!manifestError.empty())
                                error = manifestError;
                            if (error.empty())
                            {
                                manifest.width = paint->width;
                                manifest.height = paint->height;
                                manifest.layerCount = paint->layerCount;
                                manifest.layerKeys =
                                    terrainPaintSession->LayerKeys();
                                if (!SaveTerrainLayerPaintBundle(
                                        path, *paint, manifest, manifestError))
                                    error = manifestError;
                                else
                                {
                                    terrainUnsavedPaint.erase(terrainPaintSessionPlacementId);
                                    MarkTerrainAuthorSaved(terrainPaintSessionPlacementId,
                                        TerrainCompositedDependencyChange::Paint);
                                    scripts.SetFeedback(
                                        "Terrain paint saved: " + path.string());
                                }
                            }
                        }
                    }
                }
            }
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
        }
    }
    if (error.empty() && (invalidate ||
        (terrainPaintSession && terrainPaintSession->CompositedDirty().valid)))
    {
        if (invalidate)
        {
            terrainUnsavedPaint.insert(terrainPaintSessionPlacementId);
            if (terrainAuthorMonitor)
                terrainAuthorMonitor->MarkEdited(terrainPaintSessionPlacementId, TerrainCompositedDependencyChange::Paint);
        }
        // 先更新 World 的 placement 快照：Play/Stop 或切换 Actor 会销毁
        // 会话，重建时必须从最新覆盖层继续，而不是回到加载期旧快照。
        if (!world->UpdateTerrainPlacementLayerPaint(
                terrainPaintSessionPlacementId,
                terrainPaintSession->PaintData()))
            error = "Terrain paint placement disappeared from the active World.";
        else
        {
            try
            {
                const auto streaming =
                    RendererResourceCache::GetInstance().GetTerrainStreaming();
                if (!streaming)
                    throw std::runtime_error(
                        "Terrain paint requires layered terrain pages.");
                if (instance->plan.desc.compositedVT.enabled &&
                    streaming->composited)
                {
                    TerrainBuildPlan updatedPlan = instance->plan;
                    updatedPlan.layerPaint = terrainPaintSession->PaintData();
                    const auto dirty = terrainPaintSession->CompositedDirty();
                    if (dirty.valid)
                    {
                        QueueTerrainCompositedRefresh(*world, *streaming->composited,
                            updatedPlan, {terrainPaintSessionPlacementId,
                                TerrainCompositedDependencyChange::Paint, dirty.rect}, dirty.revision);
                    }
                }
                else
                {
                    throw std::runtime_error(
                        "Terrain paint requires the composited refresh transaction service");
                }
            }
            catch (const std::exception& exception)
            {
                error = std::string("Terrain paint page update failed: ") +
                    exception.what();
            }
        }
    }
    if (!error.empty())
    {
        scripts.SetFeedback(error);
        if (init.diagnostics != nullptr)
            init.diagnostics->ReportWarning(error, "Terrain");
    }
    return true;
}

bool EditorHost::ApplyTerrainEditUpdate(World& world)
{
    if (!terrainEditSession || init.rendererBackend == nullptr ||
        terrainEditSessionGeneration != world.GetGeneration())
        return false;
    Editor::TerrainMeshUpdate update = terrainEditSession->BuildUpdate();
    // 加载期驻留窗口会跳过部分 (chunk, LOD)；CPU 编辑仍要对整张高度图
    // 生成 dirty，但 GPU 只能替换 World 里真实存在的对象。
    const auto& meshObjects = world.GetMeshObjects();
    update.chunks.erase(std::remove_if(update.chunks.begin(), update.chunks.end(),
        [&meshObjects, &update](const Editor::TerrainChunkMeshUpdate& chunk)
        {
            const auto found = meshObjects.find(chunk.objectName);
            return found == meshObjects.end() || !found->second.isTerrain ||
                found->second.terrainPlacementId != update.placementId;
        }), update.chunks.end());
    if (update.chunks.empty() && !update.compositedDirty.valid) return false;
    if (update.chunks.empty() && update.compositedDirty.valid &&
        update.desc.compositedVT.enabled)
    {
        try
        {
            const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
            if (!streaming || !streaming->composited)
                return false;
            for (const auto& placement : world.GetTerrainCollisionInstances())
                if (placement.placementId == update.placementId)
                {
                    auto plan = placement.plan;
                    plan.heightmap = update.heightmap;
                    QueueTerrainCompositedRefresh(world, *streaming->composited, plan,
                        {update.placementId, TerrainCompositedDependencyChange::Sculpt,
                            update.compositedDirty.rect}, update.sourceRevision);
                    return true;
                }
        }
        catch (const std::exception& error)
        {
            if (init.diagnostics)
                init.diagnostics->ReportWarning(error.what(), "TerrainVT");
        }
        return false;
    }
    if (update.desc.compositedVT.enabled && TerrainCompositedService::IsAuthoringMode())
    {
        const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
        if (!streaming || !streaming->composited || !streaming->terrainVertices)
            return false;
        std::string error;
        auto& sculpt = terrainSculptApplies[update.placementId];
        const auto residency = streaming->terrainVertices->ResidencyRevision();
        if (terrainSculptResidencyRevisions[update.placementId] != residency)
            sculpt.reset();
        if (!sculpt)
        {
            auto baseline = terrainEditSession->BuildUpdate(true);
            baseline.chunks.erase(std::remove_if(baseline.chunks.begin(), baseline.chunks.end(),
                [&meshObjects, &baseline](const Editor::TerrainChunkMeshUpdate& chunk) {
                    const auto found = meshObjects.find(chunk.objectName);
                    return found == meshObjects.end() || !found->second.isTerrain ||
                        found->second.terrainPlacementId != baseline.placementId;
                }), baseline.chunks.end());
            const bool dynamic = std::all_of(baseline.chunks.begin(), baseline.chunks.end(),
                [&streaming](const Editor::TerrainChunkMeshUpdate& chunk) {
                    return streaming->terrainVertices->HasDynamicChunk(chunk.resourceKey);
                });
            auto candidate = std::make_shared<Editor::TerrainCompositedSculptApply>();
            if (!candidate->Register(world.GetGeneration(), baseline, dynamic, error))
            {
                if (init.diagnostics)
                    init.diagnostics->ReportWarning(error, "Terrain");
                return false;
            }
            sculpt = std::move(candidate);
            terrainSculptResidencyRevisions[update.placementId] = residency;
        }
        TerrainVertexUpdateQueue::Prepared prepared;
        try
        {
            std::vector<TerrainVertexUpdateQueue::Update> vertices;
            vertices.reserve(update.chunks.size());
            for (const auto& chunk : update.chunks)
                vertices.push_back({chunk.resourceKey,
                    std::make_shared<const std::vector<Vertex>>(chunk.vertices)});
            prepared = streaming->terrainVertices->Prepare(world.GetGeneration(), vertices);
        }
        catch (const std::exception& failure)
        {
            if (init.diagnostics)
                init.diagnostics->ReportWarning(failure.what(), "Terrain");
            return false;
        }
        TerrainEditApplyStats stats;
        if (!ApplyCompositedTerrainSculptUpdate(world, *sculpt, update,
                [this, &world, &streaming, &prepared](const TerrainBuildPlan& plan,
                    const TerrainCompositedDirtyChange& change, uint64_t revision) {
                    if (change.rect.minU < change.rect.maxU && change.rect.minV < change.rect.maxV)
                        QueueTerrainCompositedRefresh(world, *streaming->composited,
                            plan, change, revision);
                    streaming->terrainVertices->Commit(std::move(prepared));
                }, stats, error))
        {
            if (init.diagnostics)
                init.diagnostics->ReportWarning(error, "Terrain");
            return false;
        }
        terrainUnsavedHeight.insert(update.placementId);
        if (terrainAuthorMonitor)
            terrainAuthorMonitor->MarkEdited(update.placementId, TerrainCompositedDependencyChange::Sculpt);
        terrainEditSession->ResetDirty();
        return true;
    }
    WaitForRenderIdle();

    const bool layered = !update.desc.layers.empty();
    if (layered)
    {
        const std::string message =
            "Layered terrain sculpt requires the composited authoring transaction service";
        if (init.diagnostics)
            init.diagnostics->ReportWarning(message, "Terrain");
        if (init.scripts)
            init.scripts->SetFeedback(message);
        return false;
    }

    TerrainEditApplyStats stats;
    std::string error;
    if (!ApplyTerrainMeshUpdate(
            world, *init.rendererBackend, update, stats, error))
    {
        const std::string message = "Terrain sculpt GPU update failed: " + error;
        if (init.diagnostics != nullptr)
            init.diagnostics->ReportWarning(message, "Terrain");
        if (init.scripts != nullptr)
            init.scripts->SetFeedback(message);
        return false;
    }
    terrainEditSession->ResetDirty();
    if ((stats.renderableObjects != 0 || stats.worldChunks != 0) &&
        init.diagnostics != nullptr)
        init.diagnostics->ReportInfo(
            "Terrain sculpt updated " + std::to_string(stats.renderableObjects) +
            " renderable objects and " + std::to_string(stats.worldChunks) +
            " World chunks.", "Terrain");
    return true;
}

bool EditorHost::ApplyOpenScriptSource(const UiAction& action)
{
    auto& scripts = *init.scripts;
    std::string error;
    try
    {
        const auto url = ScriptSourceEditorUrl(
            CommonFunction::GetResourcePath(),
            scripts.Classes(),
            action.stringValue);
        if (!SDL_OpenURL(url.c_str()))
            throw std::runtime_error(SDL_GetError());
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    scripts.SetFeedback(error);
    if (!error.empty() && init.diagnostics != nullptr)
        init.diagnostics->ReportWarning(error, "Script");
    return true;
}

bool EditorHost::ApplyScriptAuthoringAction(
    const UiAction& action,
    World* world)
{
    auto& scripts = *init.scripts;
    std::string error;
    if (world == nullptr || world->GetGeneration() != action.actorProperty.worldGeneration)
        error = "The World changed since this script request";
    else if (init.renderSystem->IsOfflineMode() ||
        std::filesystem::path(world->GetScenePath()).filename() ==
            "SC_model_preview.json")
        error = "C# Actors require a realtime level";
    else if (auto* documents = SceneDocuments())
    {
        std::string id;
        if (StageScriptBlueprintAction(
                *world, scripts, *documents, action, id, error))
        {
            if (action.type == UiActionType::CreateScriptBlueprintAsset)
            {
                scripts.SetFeedback("Blueprint asset created: " + action.stringValue);
                if (init.ui != nullptr)
                    init.ui->RequestRefreshAssetCatalog();
                return true;
            }
            pendingScriptBlueprintSelection = id.empty() ? "" : "blueprintInstance|" + id;
            RuntimeCommand command;
            command.type = RuntimeCommandType::LoadWorld;
            command.stringValue = world->GetScenePath();
            if (init.commandBus != nullptr)
                init.commandBus->Queue(std::move(command));
        }
    }
    scripts.SetFeedback(error);
    if (!error.empty() && init.diagnostics != nullptr)
        init.diagnostics->ReportWarning(error);
    return true;
}

bool EditorHost::ApplyPreviewAction(const UiAction& action, World* world)
{
    (void)world;
    RuntimeCommand command;
    command.sourceText = "ui";
    switch (action.type)
    {
    case UiActionType::PreviewBlueprint:
    {
        // UI 只传地址和 revision；候选几何必须来自 GT 文档 owner 已接受的草稿。
        if (init.ui == nullptr || uiShell == nullptr ||
            uiShell->EditorRuntime() == nullptr ||
            init.scripts == nullptr || !init.scripts->IsEditing() ||
            init.renderSystem->IsOfflineMode())
            return true;
        const auto& documents =
            uiShell->EditorRuntime()->GetSnapshot().blueprintDocuments;
        bool found = false;
        for (const auto& document : documents)
            if (document.assetPath == action.stringValue &&
                document.documentRevision == action.blueprintPreviewRevision)
            {
                command.extraSceneObjects = BuildBlueprintPreviewObjects(
                    action.stringValue,
                    action.blueprintPreviewRevision,
                    document.documentRevision,
                    document.document);
                found = true;
                break;
            }
        if (!found) return true;
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = ModelPreviewScenePath;
        break;
    }
    case UiActionType::PreviewMaterial:
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = ModelPreviewScenePath;
        command.extraSceneObjects = BuildMaterialPreviewObjects(action.stringValue);
        break;
    case UiActionType::PreviewParticle:
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = ParticlePreviewScenePath;
        command.extraSceneObjects =
            BuildParticlePreviewObjects(action.stringValue);
        command.particleDocumentPath = action.stringValue;
        command.particleDocument = action.particleDocument;
        break;
    case UiActionType::UpdateParticlePreview:
        command.type = RuntimeCommandType::UpdateParticlePreview;
        command.stringValue = action.stringValue;
        command.particleDocumentPath = action.stringValue;
        command.particleDocument = action.particleDocument;
        break;
    case UiActionType::PreviewModel:
        command.type = RuntimeCommandType::LoadWorld;
        command.stringValue = ModelPreviewScenePath;
        command.extraSceneObjects = BuildModelPreviewObjects(action.stringValue);
        // 厘米骨骼/TFX 资产按原尺度预览，不继承旧静态模型注入的 43 倍尺度。
        try
        {
            const auto buildPlan =
                MeshAssetLoader::Prepare(action.stringValue).buildPlan;
            if (buildPlan.assetType == "tressfx")
            {
                // 带 skinning 的毛发需要源角色姿态，视口不承担；由蓝图 TressFX
                // 组件承载。这里硬拒绝，不能静默剥掉 skinning 后假装可预览。
                if (buildPlan.tressfxSimulation.skinning)
                {
                    if (init.diagnostics != nullptr)
                        init.diagnostics->ReportWarning(
                            "TressFX hair preview skipped: asset declares simulation.skinning "
                            "and is authored by the Blueprint TressFX component: " +
                            action.stringValue);
                    return true;
                }
                command.extraSceneObjects[0]["scale"] = {1.0, 1.0, 1.0};
            }
            else if (buildPlan.IsSkeletal())
                command.extraSceneObjects[0]["scale"] = {1.0, 1.0, 1.0};
        }
        catch (const std::exception& error)
        {
            if (init.diagnostics != nullptr)
                init.diagnostics->ReportWarning(error.what());
            return true;
        }
        break;
    case UiActionType::PreviewRetarget:
        try
        {
            const auto target =
                MeshAssetLoader::Prepare(action.retargetPreviewModel).buildPlan;
            if (!target.IsSkeletal() || target.animationSourceModelPath.empty() ||
                target.retargetProfilePath.empty() ||
                std::filesystem::weakly_canonical(CommonFunction::Path(
                    target.retargetProfilePath)) !=
                std::filesystem::weakly_canonical(CommonFunction::Path(
                    action.stringValue)))
                throw std::invalid_argument(
                    "Retarget preview model must reference the opened RT profile");
            const auto source =
                MeshAssetLoader::Prepare(target.animationSourceModelPath).buildPlan;
            if (!source.IsSkeletal() || !source.animationSourceModelPath.empty())
                throw std::invalid_argument(
                    "Retarget preview source must be a non-retargeted skeletal mesh");
            command.type = RuntimeCommandType::LoadWorld;
            command.stringValue = ModelPreviewScenePath;
            command.extraSceneObjects = BuildRetargetPreviewObjects(
                action.retargetPreviewModel,
                target.animationSourceModelPath,
                action.stringValue);
        }
        catch (const std::exception& error)
        {
            if (init.diagnostics != nullptr)
                init.diagnostics->ReportWarning(error.what());
            return true;
        }
        break;
    default:
        return false;
    }
    if (init.commandBus != nullptr)
        init.commandBus->Queue(std::move(command));
    return true;
}

void EditorHost::Tick(float, World* world)
{
    if (world && world->GetGeneration() != terrainAuthorMonitorGeneration)
    {
        terrainAuthorMonitor = std::make_unique<Editor::TerrainCompositedAuthorMonitor>();
        terrainAuthorMonitorGeneration = world->GetGeneration();
        terrainUnsavedHeight.clear();
        terrainUnsavedPaint.clear();
        terrainPendingRefreshes.clear();
    }
    if (world && terrainAuthorMonitor)
    {
        const auto streaming = RendererResourceCache::GetInstance().GetTerrainStreaming();
        const auto baselineBatchStarted = std::chrono::steady_clock::now();
        std::shared_ptr<Editor::TerrainCompositedAuthorMonitor::BaselineLoadSession> baselineSession;
        std::vector<std::shared_ptr<TerrainCompositedVerifiedInputs>> baselineOwners;
        uint64_t baselineAttempted = 0;
        uint64_t baselineLoaded = 0;
        for (const auto& placement : world->GetTerrainCollisionInstances())
        {
            Editor::TerrainCompositedAuthorMonitor::SurfaceStatus existing;
            if (!placement.plan.desc.compositedVT.enabled ||
                terrainAuthorMonitor->GetStatus(placement.placementId, existing) ||
                !streaming || !streaming->composited)
                continue;
            if (!baselineSession)
                baselineSession =
                    std::make_shared<Editor::TerrainCompositedAuthorMonitor::BaselineLoadSession>();
            ++baselineAttempted;
            const auto placementStarted = std::chrono::steady_clock::now();
            double serviceElapsedMs = 0;
            bool serviceCompleted = false;
            bool loaded = false;
            size_t capturedDependencies = 0;
            std::string baselineError;
            try
            {
                const auto baseline = streaming->composited->LoadAuthorBaseline(
                    world->GetGeneration(), placement.placementId);
                serviceElapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - placementStarted).count();
                serviceCompleted = true;
                capturedDependencies = baseline.dependencies.size();
                if (!baseline.verificationOwner)
                    throw std::runtime_error("Terrain author baseline has no terrain verification owner");
                if (std::find(baselineOwners.begin(), baselineOwners.end(), baseline.verificationOwner) ==
                    baselineOwners.end())
                    baselineOwners.push_back(baseline.verificationOwner);
                if (baseline.worldGeneration != world->GetGeneration() ||
                    baseline.surfaceId != placement.placementId)
                    throw std::runtime_error("Terrain author baseline belongs to another World or placement");
                std::string error;
                if (!terrainAuthorMonitor->LoadBaseline(baseline,
                        {CommonFunction::GetResourcePath(), CommonFunction::GetProjectPath()},
                        baselineSession, error))
                    throw std::runtime_error(error);
                loaded = true;
                ++baselineLoaded;
            }
            catch (const std::exception& error)
            {
                baselineError = error.what();
                if (init.diagnostics)
                    init.diagnostics->ReportWarning(error.what(), "TerrainVT");
            }
            const auto placementEnded = std::chrono::steady_clock::now();
            std::cout << "[RVT_EDITOR_BASELINE_PLACEMENT] " << nlohmann::json{
                {"worldGeneration", world->GetGeneration()}, {"surface", placement.placementId},
                {"beginMonotonicMs", std::chrono::duration<double, std::milli>(
                    placementStarted.time_since_epoch()).count()},
                {"endMonotonicMs", std::chrono::duration<double, std::milli>(
                    placementEnded.time_since_epoch()).count()},
                {"elapsedMs", std::chrono::duration<double, std::milli>(
                    placementEnded - placementStarted).count()},
                {"serviceCompleted", serviceCompleted},
                {"serviceElapsedMs", serviceCompleted ? nlohmann::json(serviceElapsedMs) : nlohmann::json(nullptr)},
                {"capturedDependencyCount", capturedDependencies}, {"loaded", loaded},
                {"error", baselineError}}.dump() << std::endl;
        }
        if (baselineSession)
        {
            // 全部 Service/Monitor 初始化调用结束才关闭；Poll 另有防漏关闭，不跨作者编辑复用。
            baselineSession->Close();
            const auto metrics = baselineSession->Stats();
            nlohmann::json sharedMetrics = nlohmann::json::array();
            for (const auto& owner : baselineOwners)
            {
                owner->Close();
                const auto statistics = owner->Statistics();
                sharedMetrics.push_back({
                    {"ownerHandle", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owner.get()))},
                    {"beginNanoseconds", statistics.beginNanoseconds},
                    {"endNanoseconds", statistics.endNanoseconds},
                    {"requests", statistics.requests}, {"requestedBytes", statistics.requestedBytes},
                    {"readCalls", statistics.readCalls}, {"actualReadBytes", statistics.actualReadBytes},
                    {"validatedBytes", statistics.validatedBytes}, {"hits", statistics.hits},
                    {"failures", statistics.failures}, {"retainedBytes", statistics.retainedBytes},
                    {"closed", statistics.closed}});
            }
            const auto ended = std::chrono::steady_clock::now();
            std::cout << "[RVT_EDITOR_BASELINE_INITIALIZATION] " << nlohmann::json{
                {"worldGeneration", world->GetGeneration()}, {"attemptedSurfaces", baselineAttempted},
                {"loadedSurfaces", baselineLoaded},
                {"beginMonotonicMs", std::chrono::duration<double, std::milli>(
                    baselineBatchStarted.time_since_epoch()).count()},
                {"endMonotonicMs", std::chrono::duration<double, std::milli>(
                    ended.time_since_epoch()).count()},
                {"elapsedMs", std::chrono::duration<double, std::milli>(
                    ended - baselineBatchStarted).count()},
                {"monitorObjectRequests", metrics.objectRequests},
                {"monitorObjectReads", metrics.objectReads}, {"monitorReadBytes", metrics.readBytes},
                {"monitorCacheHits", metrics.cacheHits}, {"sharedOwnerCount", baselineOwners.size()},
                {"sharedOwnerStatistics", std::move(sharedMetrics)}}.dump() << std::endl;
        }
        try
        {
            PollTerrainCompositedRefresh(*world);
            if (terrainPaintSession && terrainPaintSessionGeneration == world->GetGeneration() &&
                terrainPaintSession->CompositedDirty().valid && streaming && streaming->composited)
            {
                for (const auto& placement : world->GetTerrainCollisionInstances())
                    if (placement.placementId == terrainPaintSessionPlacementId)
                    {
                        auto plan = placement.plan;
                        plan.layerPaint = terrainPaintSession->PaintData();
                        const auto dirty = terrainPaintSession->CompositedDirty();
                        QueueTerrainCompositedRefresh(*world, *streaming->composited, plan,
                            {placement.placementId, TerrainCompositedDependencyChange::Paint, dirty.rect},
                            dirty.revision);
                        break;
                    }
            }
        }
        catch (const std::exception& error)
        {
            if (init.diagnostics)
                init.diagnostics->ReportWarning(error.what(), "TerrainVT");
        }
    }
    if (terrainAuthorMonitor)
    {
        for (const auto& status : terrainAuthorMonitor->Poll(
                 Editor::TerrainCompositedAuthorMonitor::Clock::now()))
        {
            if (status.externalChange && init.scripts)
                init.scripts->SetFeedback(
                    "Terrain source changed: " + status.surfaceId +
                    (status.conflict ? "; unsaved edits conflict. Save or discard before reload/Cook." :
                        "; reload and Cook to refresh the derived cache."));
        }
    }
    if (init.ui == nullptr)
        return;
    // 先在 GT 稳定帧边界处理 editor/preview，再生成 UI 与 World snapshot。
    init.ui->TickMaterialInstanceEditor();
    // 脚本驱动的选中请求在判定脏标记之前处理，选中高亮才能在同一个 GT 帧重建。
    if (!pendingScriptBlueprintSelection.empty() &&
        init.ui->RequestActorSelection(pendingScriptBlueprintSelection))
    {
        pendingScriptBlueprintSelection.clear();
    }
}

void EditorHost::TickThumbnails(
    const std::vector<PlatformEvent>& platformEvents)
{
    if (init.ui == nullptr || init.rendererBackend == nullptr ||
        init.pipelineFactory == nullptr || init.runtimeConfig == nullptr)
        return;
    auto* library = uiShell != nullptr ? uiShell->ThumbnailLibrary() : nullptr;
    if (library == nullptr)
        return;
    if (thumbnailRequest && !library->IsRenderRequestCurrent(*thumbnailRequest))
        thumbnailRequest.reset();
    if (!thumbnailScheduler.CanAdvance(
            std::chrono::steady_clock::now(),
            platformEvents,
            init.ui->CanRenderThumbnails()))
        return;
    // 上一帧 RT 已归还所有权；这里不调用活动 World 的加载事务，也不重绑活动图。
    if (!thumbnailRequest)
    {
        thumbnailRequest = library->TakeRenderRequest();
        if (!thumbnailRequest) return;
        // Refresh 丢弃会话缓存，但不在 GT 等待旧的 CPU 导入/解码；Begin 会拒绝迟到结果。
        if (thumbnailSession && thumbnailSessionEpoch != thumbnailRequest->epoch)
            thumbnailSession->Reset();
        thumbnailSessionEpoch = thumbnailRequest->epoch;
        if (!thumbnailSession)
            thumbnailSession = std::make_unique<EditorThumbnailRenderSession>(
                *init.rendererBackend, *init.pipelineFactory);
        thumbnailSession->Begin(
            thumbnailRequest->path,
            thumbnailRequest->kind,
            init.runtimeConfig->GetRenderGraphJson());
        return;
    }
    if (!thumbnailSession)
    {
        thumbnailSession = std::make_unique<EditorThumbnailRenderSession>(
            *init.rendererBackend, *init.pipelineFactory);
        thumbnailSession->Begin(
            thumbnailRequest->path,
            thumbnailRequest->kind,
            init.runtimeConfig->GetRenderGraphJson());
        return;
    }
    if (thumbnailSession->Step())
    {
        library->CompleteRenderRequest(
            *thumbnailRequest,
            thumbnailSession->Image(),
            thumbnailSession->Error());
        thumbnailRequest.reset();
    }
}

bool EditorHost::CanUndo(const std::string& scenePath, bool redo) const
{
    auto* documents = SceneDocuments();
    return documents != nullptr && !scenePath.empty() &&
        documents->CanUndo(scenePath, redo);
}

nlohmann::json EditorHost::DraftSceneDocument(
    const std::string& scenePath,
    std::string& error)
{
    auto* documents = SceneDocuments();
    if (documents == nullptr)
        return nullptr;
    return documents->DraftForWorldLoad(scenePath, error);
}

bool EditorHost::PrepareActorEdit(
    const std::string& scenePath,
    const std::string& identity,
    std::string& error)
{
    auto* documents = SceneDocuments();
    return documents != nullptr &&
        documents->PrepareActorEdit(scenePath, identity, error);
}

void EditorHost::RecordActorEdit(
    World& world,
    const ActorPropertyEdit& edit)
{
    auto* documents = SceneDocuments();
    if (documents == nullptr) return;
    // actor 快照的构建仍完全留在编辑器侧；运行时执行器只调用本转发函数。
    for (const auto& actor : BuildEditorSceneActors(world))
        if (actor.identity == edit.identity)
            documents->RecordActorEdit(actor, edit);
}

void EditorHost::ReplaySceneEditsAfterWorldChange(
    const std::string& scenePath,
    uint64_t generation,
    World& world)
{
    ResetAuthoringSessions();
    auto* documents = SceneDocuments();
    if (documents == nullptr)
        return;
    std::string error;
    const auto edits = documents->ReplayEdits(scenePath, generation, error);
    // 草稿跨 World 保留；只在换代后重放，不重复入文档，不修改观察相机。
    for (const auto& edit : edits)
    {
        if (!ApplyEditorActorProperty(world, edit, error)) break;
        if (edit.property == EditorActorProperty::CastShadows &&
            init.renderSystem != nullptr)
            init.renderSystem->SetCsmSettings(world.GetCsmSettings());
    }
    if (!error.empty() && init.diagnostics != nullptr)
        init.diagnostics->ReportWarning(error);
}

std::shared_ptr<const void> EditorHost::BuildSceneActorSnapshots(
    const World& world) const
{
    return std::make_shared<const std::vector<EditorUi::EditorSceneActorSnapshot>>(
        BuildEditorSceneActors(world));
}

std::shared_ptr<const void> EditorHost::BuildSceneModelSnapshots() const
{
    auto models =
        std::make_shared<std::vector<EditorUi::MaterialInstanceModelSnapshot>>();
    const auto contexts = init.renderSystem != nullptr
        ? init.renderSystem->GetMaterialInstanceModelContexts()
        : std::vector<VL::MaterialInstanceModelContext>{};
    models->reserve(contexts.size());
    for (const VL::MaterialInstanceModelContext& model : contexts)
    {
        EditorUi::MaterialInstanceModelSnapshot modelSnapshot;
        modelSnapshot.worldGeneration = model.worldGeneration;
        modelSnapshot.scenePath = model.scenePath;
        modelSnapshot.objectId = model.objectId;
        modelSnapshot.displayName = model.displayName;
        modelSnapshot.objectIdentity = model.objectIdentity;
        modelSnapshot.actorIdentity = model.actorIdentity;
        modelSnapshot.materials.reserve(model.materials.size());
        for (const VL::MaterialInstanceModelMaterial& material :
             model.materials)
        {
            EditorUi::MaterialInstanceModelMaterialSnapshot materialSnapshot;
            materialSnapshot.objectId = material.objectId;
            materialSnapshot.materialSlotIndex = material.materialSlotIndex;
            materialSnapshot.materialSlotName = material.materialSlotName;
            materialSnapshot.materialInstancePath = material.materialInstancePath;
            modelSnapshot.materials.push_back(std::move(materialSnapshot));
        }
        models->push_back(std::move(modelSnapshot));
    }
    return models;
}

void EditorHost::BuildUiSections(const World* world)
{
    if (init.ui == nullptr)
        return;
    if (world != nullptr)
        init.ui->SetSceneActors(BuildSceneActorSnapshots(*world));
    else
        init.ui->SetSceneActors(
            std::make_shared<const std::vector<EditorUi::EditorSceneActorSnapshot>>());
    init.ui->SetMaterialInstanceSceneModels(BuildSceneModelSnapshots());
}

bool EditorHost::SelectSceneObjectFromPick(
    const MaterialInstanceSelection& selection)
{
    return init.ui != nullptr && init.ui->SelectSceneObjectFromPick(selection);
}

void EditorHost::ClearMaterialInstanceSceneSelection()
{
    if (init.ui != nullptr)
        init.ui->ClearMaterialInstanceSceneSelection();
}

IEditorConsoleSink* EditorHost::ConsoleSink()
{
    if (!consoleSink)
    consoleSink = std::make_unique<ConsoleSinkImpl>(*this);
    return consoleSink.get();
}

IEditorViewportOverlay* EditorHost::ViewportOverlay()
{
    return viewportGrid.get();
}

bool EditorHost::IsEnabled() const
{
    return init.ui != nullptr && init.ui->IsDeveloperUiEnabled();
}

bool EditorHost::IsEditorFlavor() const
{
    return true;
}

bool EditorHost::IsVisible() const
{
    return init.ui != nullptr && init.ui->IsDeveloperUiVisible();
}

} // namespace VL

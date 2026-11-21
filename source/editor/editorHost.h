#pragma once

#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "engine/editorHost.h"
#include "engine/editorThumbnailScheduler.h"
#include "editor/assets/editorThumbnailLibrary.h"
#include "terrain/terrainCompositedRefresh.h"

namespace VL
{

class EditorThumbnailRenderSession;
class EditorViewportGrid;
class EditorUiShell;
class World;
class TerrainCompositedService;
struct TerrainBuildPlan;
struct TerrainCompositedDirtyChange;
enum class TerrainCompositedDependencyChange;

namespace Editor
{
class MaterialInstanceEditorRuntime;
class SceneDocumentService;
class TerrainEditSession;
class TerrainPaintSession;
class TerrainCompositedSculptApply;
class TerrainCompositedAuthorMonitor;
struct TerrainCompositedRefreshReceipt;
}

// Editor-side implementation of the runtime host seam. It owns every object
// that used to live in EngineLoop but is meaningless to a game build: the
// viewport grid, terrain authoring sessions, thumbnail renderer, scene
// documents and editor UI snapshot construction.
class EditorHost final : public IEditorHost
{
public:
    // 只读测试观察：复制 GT receipt 和互斥 Poll 状态；不提交刷新，也不暴露 RT 可变容器。
    struct TerrainRefreshObservation
    {
        TerrainCompositedRefreshStatus status;
        uint64_t sourceRevision = 0;
        uint64_t expectedRevision = 0;
        uint32_t attempts = 0;
        bool terminalFailure = false;
        ContentDigest inputIdentity;
    };
    std::vector<TerrainRefreshObservation> ObserveTerrainRefreshes(
        uint64_t worldGeneration, const std::string& surfaceId) const;

    EditorHost();
    ~EditorHost() override;

    RuntimeResult<void> Initialize(const EditorHostInit& init) override;
    void Shutdown() override;
    void ReleaseRendererResources() override;

    void BeginUiActionBatch() override;
    bool ApplyUiAction(
        const UiAction& action,
        World* world,
        RuntimeCommandExecutionResult& result) override;
    void EndUiActionBatch(World* world) override;

    bool ApplyTerrainEditUpdate(World& world) override;
    void Tick(float deltaSeconds, World* world) override;
    void TickThumbnails(
        const std::vector<PlatformEvent>& platformEvents) override;

    bool CanUndo(const std::string& scenePath, bool redo) const override;
    nlohmann::json DraftSceneDocument(
        const std::string& scenePath, std::string& error) override;
    bool PrepareActorEdit(
        const std::string& scenePath,
        const std::string& identity,
        std::string& error) override;
    void RecordActorEdit(World& world, const ActorPropertyEdit& edit) override;
    void ReplaySceneEditsAfterWorldChange(
        const std::string& scenePath, uint64_t generation, World& world) override;
    void ResetAuthoringSessions() override;

    std::shared_ptr<const void> BuildSceneActorSnapshots(
        const World& world) const override;
    std::shared_ptr<const void> BuildSceneModelSnapshots() const override;
    void BuildUiSections(const World* world) override;

    bool SelectSceneObjectFromPick(
        const MaterialInstanceSelection& selection) override;
    void ClearMaterialInstanceSceneSelection() override;

    IEditorConsoleSink* ConsoleSink() override;
    IEditorViewportOverlay* ViewportOverlay() override;

    bool IsEditorFlavor() const override;
    bool IsEnabled() const override;
    bool IsVisible() const override;

private:
    class ConsoleSinkImpl;

    Editor::SceneDocumentService* SceneDocuments() const;
    Editor::MaterialInstanceEditorRuntime* EditorRuntime() const;

    void WaitForRenderIdle();
    void QueueTerrainCompositedRefresh(
        World& world, TerrainCompositedService& service,
        const TerrainBuildPlan& plan, const TerrainCompositedDirtyChange& change,
        uint64_t sourceRevision);
    void MarkTerrainAuthorSaved(
        const std::string& placementId, TerrainCompositedDependencyChange kind);
    void PollTerrainCompositedRefresh(World& world);
    bool ApplyTerrainEditAction(const UiAction& action, World* world);
    bool ApplyTerrainPaintAction(const UiAction& action, World* world);
    bool ApplyOpenScriptSource(const UiAction& action);
    bool ApplyScriptAuthoringAction(const UiAction& action, World* world);
    bool ApplyPreviewAction(const UiAction& action, World* world);

    EditorHostInit init;
    bool initialized = false;

    std::unique_ptr<EditorUiShell> uiShell;
    std::unique_ptr<EditorViewportGrid> viewportGrid;

    std::unique_ptr<Editor::TerrainEditSession> terrainEditSession;
    uint64_t terrainEditSessionGeneration = 0;
    std::string terrainEditSessionPlacementId;
    std::unique_ptr<Editor::TerrainPaintSession> terrainPaintSession;
    std::map<std::string, std::shared_ptr<Editor::TerrainCompositedSculptApply>> terrainSculptApplies;
    std::map<std::string, uint64_t> terrainSculptResidencyRevisions;
    std::unique_ptr<Editor::TerrainCompositedAuthorMonitor> terrainAuthorMonitor;
    uint64_t terrainAuthorMonitorGeneration = 0;
    uint64_t terrainRefreshRevision = 0;
    uint64_t terrainSessionEpoch = 0;
    uint64_t terrainPaintSessionEpoch = 0;
    uint64_t terrainEditSessionEpoch = 0;
    std::map<std::pair<std::string, TerrainCompositedDependencyChange>,
        std::shared_ptr<Editor::TerrainCompositedRefreshReceipt>> terrainPendingRefreshes;
    std::set<std::string> terrainUnsavedHeight;
    std::set<std::string> terrainUnsavedPaint;
    uint64_t terrainPaintSessionGeneration = 0;
    std::string terrainPaintSessionPlacementId;
    // Terrain sculpt CPU edits are accumulated across one UiAction drain and
    // the GPU mesh/weight replacement runs once in EndUiActionBatch.
    bool terrainEditNeedsRebuild = false;

    std::unique_ptr<EditorThumbnailRenderSession> thumbnailSession;
    std::optional<Editor::ThumbnailRenderRequest> thumbnailRequest;
    uint64_t thumbnailSessionEpoch = 0;
    EditorThumbnailScheduler thumbnailScheduler;

    std::string pendingScriptBlueprintSelection;
    std::unique_ptr<ConsoleSinkImpl> consoleSink;
};

} // namespace VL

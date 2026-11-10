#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "engine/editorHost.h"
#include "engine/editorThumbnailScheduler.h"
#include "editor/assets/editorThumbnailLibrary.h"

namespace VL
{

class EditorThumbnailRenderSession;
class EditorViewportGrid;
class EditorUiShell;
class World;

namespace Editor
{
class MaterialInstanceEditorRuntime;
class SceneDocumentService;
class TerrainEditSession;
class TerrainPaintSession;
}

// Editor-side implementation of the runtime host seam. It owns every object
// that used to live in EngineLoop but is meaningless to a game build: the
// viewport grid, terrain authoring sessions, thumbnail renderer, scene
// documents and editor UI snapshot construction.
class EditorHost final : public IEditorHost
{
public:
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

    bool IsEnabled() const override;
    bool IsVisible() const override;

private:
    class ConsoleSinkImpl;

    Editor::SceneDocumentService* SceneDocuments() const;
    Editor::MaterialInstanceEditorRuntime* EditorRuntime() const;

    void WaitForRenderIdle();
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

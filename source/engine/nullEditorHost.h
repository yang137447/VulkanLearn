#pragma once

#include "engine/editorHost.h"

namespace VL
{

// Game-side editor host: every editor operation is a no-op. Runtime code can
// keep one unconditional host call instead of scattering feature checks.
class NullEditorHost final : public IEditorHost
{
public:
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
};

} // namespace VL

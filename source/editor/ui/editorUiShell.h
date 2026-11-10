#pragma once

#include <memory>

#include "ui/iEditorUiShell.h"

namespace VL
{

namespace Editor
{
class EditorJsonAssetLibrary;
class EditorThumbnailLibrary;
class MaterialInstanceEditorRuntime;
class SceneDocumentService;
}

// Editor-module owner of every editor-only Rml document, panel, library and
// view-model section. It shares the runtime's single Rml::Context: it never
// calls Rml::Initialise/Shutdown and never owns Context::Update.
class EditorUiShell final : public IEditorUiShell
{
public:
    EditorUiShell();
    ~EditorUiShell() override;

    EditorUiShell(const EditorUiShell&) = delete;
    EditorUiShell& operator=(const EditorUiShell&) = delete;

    RuntimeResult<void> Initialize(const UiEditorShellInit& init) override;
    void Shutdown() override;
    RuntimeResult<void> ReloadDocument() override;

    void SetVisible(bool visible) override;
    bool IsVisible() const override;

    void Resize(uint32_t width, uint32_t height) override;
    void Tick(
        float deltaSeconds,
        const UiViewModelSnapshot& view,
        uint64_t scriptsSectionVersion) override;
    void UpdateSections(
        const UiViewModelSnapshot& view,
        const UiPerformanceSection& performance,
        bool snapshotDue,
        UiSectionMask runtimeChanged) override;
    void PublishEditorTextures() override;
    UiSectionVersions Versions() const override;

    bool HandlePlatformEvent(
        const PlatformEvent& event,
        bool& consumed) override;
    bool HandleGlobalShortcut(
        const PlatformEvent& event,
        bool& consumed) override;
    bool ApplyAction(
        const UiAction& action,
        World* world,
        RuntimeCommandExecutionResult& result,
        bool& consumed) override;
    bool PrepareImmersiveView(std::string& levelPath) override;

    void NotifyWorldChanged(
        const std::string& worldPath,
        uint64_t generation) override;

    void SetMaterialInstanceSceneModels(
        UiEditorSnapshotPayload models) override;
    void SetSceneActors(UiEditorSnapshotPayload actors) override;
    std::optional<MaterialInstanceModelContext>
        ConsumeMaterialInstanceModelSelectionRequest() override;
    std::optional<MaterialInstanceSelection>
        ConsumeMaterialInstanceSelectionRequest() override;
    bool ConsumeSceneSelectionClearRequest() override;

    bool RequestModelSelection(
        const MaterialInstanceModelContext& model) override;
    bool RequestMaterialSelection(
        const MaterialInstanceSelection& selection) override;
    bool RequestActorSelection(const std::string& actorIdentity) override;
    bool SelectSceneObjectFromPick(
        const MaterialInstanceSelection& selection) override;
    void ClearMaterialInstanceSceneSelection() override;
    bool CancelBlueprintAssetDrag() override;
    void RequestOpenJsonAsset(const std::string& assetPath) override;
    void RequestRefreshAssetCatalog() override;

    bool IsScenePickTarget(float mouseX, float mouseY) const override;
    UiViewportRect GetSceneViewportRect() const override;
    bool IsMotionDebugViewport() const override;
    bool IsAssetEditorVisible() const override;
    bool HasModal() const override;
    bool IsWorkspaceDragging() const override;
    bool IsEditorDocumentElement(const Rml::Element* element) const override;
    bool IsSceneViewportElement(const Rml::Element* element) const override;
    bool IsSceneViewportInputActive() const override;
    void SetSceneViewportInputActive(bool active) override;
    bool IsPointerCaptured() const override;
    void CancelTransientInteraction() override;

    // Concrete editor-module access for EditorHost only.
    Editor::SceneDocumentService* SceneDocuments() const;
    Editor::MaterialInstanceEditorRuntime* EditorRuntime() const;
    Editor::EditorThumbnailLibrary* ThumbnailLibrary() const;
    bool CanRenderThumbnails() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VL

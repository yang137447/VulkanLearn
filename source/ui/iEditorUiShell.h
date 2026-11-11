#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/runtimeResult.h"
#include "platform/platformEvent.h"
#include "render/frontend/scenePicking.h"
#include "ui/rmlUiInterfaces.h"
#include "ui/uiAction.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/viewModel/runtimeUiSections.h"
#include "ui/viewport/editorViewportGeometry.h"

namespace Rml
{
class Context;
class Element;
}

namespace VL
{

class DiagnosticsSubsystem;
class IEditorUiShell;
class World;
struct RuntimeCommandExecutionResult;

// Opaque editor snapshot payloads. The runtime UI carries them through without
// naming editor vector element types.
using UiEditorSnapshotPayload = std::shared_ptr<const void>;

// Low-level services the editor shell still needs from the single runtime UI
// owner: Rml input dispatch, the one RmlUi context, command queueing and the
// runtime refresh scheduler. The shell never owns Rml::Initialise/Shutdown or
// Context::Update.
class IEditorUiRuntimeBridge
{
public:
    virtual ~IEditorUiRuntimeBridge() = default;

    virtual bool ProcessRmlEvent(const PlatformEvent& event) = 0;
    virtual void ResetRmlInputState() = 0;
    virtual void TrackPointer(float x, float y) = 0;
    virtual bool IsMouseButtonPressed(PlatformMouseButton button) const = 0;
    virtual bool HasPressedMouseButtons() const = 0;
    virtual bool IsKeyboardKeyPressed(PlatformKey key) const = 0;

    // A panel-local click/wheel changes editor DOM state: request one rebuild
    // and force the next RmlUi update.
    virtual void RequestPanelBuild() = 0;
    virtual void MarkPanelBuildMutation() = 0;
    virtual void RequestRuntimeAction(UiAction action) = 0;
    virtual void RequestViewModelRefresh(UiSectionMask sections) = 0;

    // Editor document hot reload is validated through the runtime system
    // interface so parse errors still roll back the whole UI commit.
    virtual void BeginDocumentValidation() = 0;
    virtual bool EndDocumentValidation(std::string& outError) = 0;

    virtual void RecordSectionPublish(UiSectionId id) = 0;
    virtual void RecordPanelBuild() = 0;
    virtual void RecordRuntimeViewUpdate() = 0;
    // Called from EditorUiShell's destructor if its owner disappeared without
    // an explicit UiSubsystem::SetEditorUiShell(nullptr).
    virtual void ClearEditorUiShell(IEditorUiShell* shell) = 0;
};

struct UiEditorShellInit
{
    Rml::Context* context = nullptr;
    RmlUiRenderInterface* renderInterface = nullptr;
    IEditorUiRuntimeBridge* bridge = nullptr;
    const DiagnosticsSubsystem* diagnostics = nullptr;
    std::filesystem::path assetRoot;
    std::filesystem::path sceneRoot;
    void* previewAdapter = nullptr;
    bool enabled = false;
    bool visible = false;
};

// Runtime-owned seam for editor UI ownership. Implemented by EditorUiShell in
// the editor module. The runtime UI holds a nullable pointer and forwards only
// through this interface, so runtime translation units never include
// editor/**.
class IEditorUiShell
{
public:
    virtual ~IEditorUiShell() = default;

    virtual RuntimeResult<void> Initialize(const UiEditorShellInit& init) = 0;
    // Detaches/unloads editor documents before the runtime destroys the shared
    // RmlUi context. Must be idempotent.
    virtual void Shutdown() = 0;
    virtual RuntimeResult<void> ReloadDocument() = 0;

    virtual void SetVisible(bool visible) = 0;
    virtual bool IsVisible() const = 0;

    virtual void Resize(uint32_t width, uint32_t height) = 0;
    virtual void Tick(
        float deltaSeconds,
        const UiViewModelSnapshot& view,
        uint64_t scriptsSectionVersion) = 0;
    // Publishes editor assets/outliner sections, rebuilds panels and refreshes
    // the panel runtime projection. Returns nothing; versions are queried via
    // Versions() so the runtime can merge the changed mask.
    virtual void UpdateSections(
        const UiViewModelSnapshot& view,
        const UiPerformanceSection& performance,
        bool snapshotDue,
        UiSectionMask runtimeChanged) = 0;
    virtual void PublishEditorTextures() = 0;
    virtual UiSectionVersions Versions() const = 0;

    virtual bool HandlePlatformEvent(
        const PlatformEvent& event,
        bool& consumed) = 0;
    // Editor-only global shortcuts (currently Ctrl+S). Runtime shortcuts stay
    // in RuntimeUi.
    virtual bool HandleGlobalShortcut(
        const PlatformEvent& event,
        bool& consumed) = 0;
    virtual bool ApplyAction(
        const UiAction& action,
        World* world,
        RuntimeCommandExecutionResult& result,
        bool& consumed) = 0;
    virtual bool PrepareImmersiveView(std::string& levelPath) = 0;

    virtual void NotifyWorldChanged(
        const std::string& worldPath,
        uint64_t generation) = 0;

    virtual void SetMaterialInstanceSceneModels(
        UiEditorSnapshotPayload models) = 0;
    virtual void SetSceneActors(UiEditorSnapshotPayload actors) = 0;
    virtual std::optional<MaterialInstanceModelContext>
        ConsumeMaterialInstanceModelSelectionRequest() = 0;
    virtual std::optional<MaterialInstanceSelection>
        ConsumeMaterialInstanceSelectionRequest() = 0;
    virtual bool ConsumeSceneSelectionClearRequest() = 0;

    virtual bool RequestModelSelection(
        const MaterialInstanceModelContext& model) = 0;
    virtual bool RequestMaterialSelection(
        const MaterialInstanceSelection& selection) = 0;
    virtual bool RequestActorSelection(const std::string& actorIdentity) = 0;
    virtual bool SelectSceneObjectFromPick(
        const MaterialInstanceSelection& selection) = 0;
    virtual void ClearMaterialInstanceSceneSelection() = 0;
    virtual bool CancelBlueprintAssetDrag() = 0;
    virtual void RequestOpenJsonAsset(const std::string& assetPath) = 0;
    virtual void RequestRefreshAssetCatalog() = 0;

    virtual bool IsScenePickTarget(float mouseX, float mouseY) const = 0;
    virtual UiViewportRect GetSceneViewportRect() const = 0;
    virtual bool IsMotionDebugViewport() const = 0;
    virtual bool IsAssetEditorVisible() const = 0;
    virtual bool HasModal() const = 0;
    virtual bool IsWorkspaceDragging() const = 0;
    virtual bool IsEditorDocumentElement(const Rml::Element* element) const = 0;
    virtual bool IsSceneViewportElement(const Rml::Element* element) const = 0;
    virtual bool IsSceneViewportInputActive() const = 0;
    virtual void SetSceneViewportInputActive(bool active) = 0;
    virtual bool IsPointerCaptured() const = 0;
    virtual void CancelTransientInteraction() = 0;
};

} // namespace VL

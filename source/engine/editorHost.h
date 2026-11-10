#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"
#include "platform/platformEvent.h"
#include "ui/uiAction.h"
#include "world/actorProperty.h"

class RenderSystem;
class PipelineFactory;

namespace VL
{

class CommandBus;
class DiagnosticsSubsystem;
class IEditorViewportOverlay;
class PlatformWindow;
class RendererBackendVulkan;
class RuntimeConfig;
class ScriptRuntimeSubsystem;
class UiSubsystem;
class World;
class WorldManager;
struct MaterialInstanceSelection;
struct RuntimeCommandExecutionResult;

// Runtime-owned composition input for the editor module. Every pointer is a
// borrowed owner that outlives the host; the host never owns engine systems.
struct EditorHostInit
{
    PlatformWindow* window = nullptr;
    const RuntimeConfig* runtimeConfig = nullptr;
    CommandBus* commandBus = nullptr;
    const DiagnosticsSubsystem* diagnostics = nullptr;
    WorldManager* worldManager = nullptr;
    ScriptRuntimeSubsystem* scripts = nullptr;
    RenderSystem* renderSystem = nullptr;
    RendererBackendVulkan* rendererBackend = nullptr;
    PipelineFactory* pipelineFactory = nullptr;
    UiSubsystem* ui = nullptr;
    // GT-only safe-point hook: terrain sculpt and thumbnail work must never
    // mutate GPU objects while the render thread still records a frame.
    std::function<void()> waitForRenderThreadIdle;
};

// Narrow console seam. The editor host owns JSON command codec/result state;
// the runtime console only prints text and tracks queued command ids.
class IEditorConsoleSink
{
public:
    virtual ~IEditorConsoleSink() = default;

    virtual bool IsAvailable() const = 0;

    struct Submission
    {
        std::string text;
        uint64_t commandId = 0;
        bool queued = false;
    };

    // Throws std::exception on malformed JSON, exactly like the editor runtime.
    virtual Submission SubmitJson(std::string_view request) = 0;
    // Returns the encoded result only when the command reached a final status.
    virtual std::optional<std::string> PollResult(
        uint64_t commandId, bool& isFinal) = 0;
};

// Runtime-side seam for editor ownership. Implemented by the editor module
// (source/editor/editorHost.*) and by NullEditorHost in game builds. The
// interface deliberately names only runtime value types so engine headers
// never depend on editor headers.
class IEditorHost
{
public:
    virtual ~IEditorHost() = default;

    virtual RuntimeResult<void> Initialize(const EditorHostInit& init) = 0;
    virtual void Shutdown() = 0;
    // Renderer resize/shutdown path: drop GPU sessions that reference the old
    // swapchain, without tearing down editor documents.
    virtual void ReleaseRendererResources() = 0;

    // Drain helpers: terrain GPU rebuild is accumulated across one UiAction
    // batch and applied once, preserving the pre-split behaviour.
    virtual void BeginUiActionBatch() = 0;
    // Consumes editor-only actions (terrain, script authoring, previews,
    // selection-driven document edits). Returns false for runtime actions.
    virtual bool ApplyUiAction(
        const UiAction& action,
        World* world,
        RuntimeCommandExecutionResult& result) = 0;
    virtual void EndUiActionBatch(World* world) = 0;

    // Applies pending CPU terrain edit to the GPU at the GT safe point.
    // Returns true when an update was applied.
    virtual bool ApplyTerrainEditUpdate(World& world) = 0;

    // GT stable-point tick: editor document runtime and pending actor
    // selection. Thumbnails keep their separate scheduler entry point.
    virtual void Tick(float deltaSeconds, World* world) = 0;
    virtual void TickThumbnails(
        const std::vector<PlatformEvent>& platformEvents) = 0;

    // Editor scene-document hooks used by EngineLoop / RuntimeCommandExecutor.
    virtual bool CanUndo(const std::string& scenePath, bool redo) const = 0;
    virtual nlohmann::json DraftSceneDocument(
        const std::string& scenePath, std::string& error) = 0;
    virtual bool PrepareActorEdit(
        const std::string& scenePath,
        const std::string& identity,
        std::string& error) = 0;
    virtual void RecordActorEdit(
        World& world, const ActorPropertyEdit& edit) = 0;
    virtual void ReplaySceneEditsAfterWorldChange(
        const std::string& scenePath, uint64_t generation, World& world) = 0;
    virtual void ResetAuthoringSessions() = 0;

    // Editor-only UI sections. The payloads are intentionally type-erased:
    // UiSubsystem publishes them as opaque shared_ptr<const void>.
    virtual std::shared_ptr<const void> BuildSceneActorSnapshots(
        const World& world) const = 0;
    virtual std::shared_ptr<const void> BuildSceneModelSnapshots() const = 0;
    virtual void BuildUiSections(const World* world) = 0;

    // Pick/selection hooks.
    virtual bool SelectSceneObjectFromPick(
        const MaterialInstanceSelection& selection) = 0;
    virtual void ClearMaterialInstanceSceneSelection() = 0;

    virtual IEditorConsoleSink* ConsoleSink() = 0;
    virtual IEditorViewportOverlay* ViewportOverlay() = 0;

    virtual bool IsEnabled() const = 0;
    virtual bool IsVisible() const = 0;
};

} // namespace VL

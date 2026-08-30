# Game UI Stack

## Status

This document is the current implementation contract for the VulkanLearn UI
stack. The two-layer UI plan is implemented for the Windows-first Vulkan
runtime:

- RmlUi owns player-facing runtime content.
- Dear ImGui owns optional developer tools.
- SDL3 remains the platform event and text-input boundary.
- VulkanLearn records the final overlay on the render thread.

The original adoption plan is retained at
`documents/plan/ui/game-ui-stack-plan.md` as the decision history and checklist.
This document is the source of truth for the implemented behavior.

## Responsibilities

### Runtime UI

RmlUi documents and styles under `ui/` are game-content assets. The current
runtime page is `ui/runtime-control.rml` with `ui/runtime-control.rcss` and
`ui/localization.json`. It provides:

- a compact HUD that shows only the active scene name
- a right-side runtime debug page split into Overview, Views, Post FX, World,
  and System tabs
- frame, FPS, scene, input ownership, wind-profile, and hot-reload status
- all 12 renderer debug-view modes
- tone mapping plus bloom strength, threshold, knee, and clamp controls
- environment intensity plus SpeedTree wind strength and gust controls
- locale switching, developer-UI visibility, close, and quit actions
- development-time hot reload with last-valid-document rollback

RmlUi does not receive direct access to `RenderGraph`, `MaterialInstance`, the
Vulkan device, or swapchain objects. Destructive validation commands such as
scene reload stress, resize stress, render-graph reload stress, and rollback
failure tests remain console or launch-option workflows and are intentionally
not exposed as clickable runtime controls.

### Developer UI

Dear ImGui is compiled only when `VULKANLEARN_ENABLE_DEVELOPER_UI=ON` and is
runtime-toggleable with `F1`. It is a material-instance tuning surface: the
selected model's material-instance navigation occupies the left 18% and the
material inspector occupies the right 30%. The center area is only a dock
layout and input region; the scene remains displayed directly by the SDL3/Vulkan
render path. Dear ImGui must not display the scene through `ImGui::Image`, bind
a RenderGraph-owned scene texture, or introduce a scene texture ID into the UI
snapshot. The editor is mutable through the typed editor-command boundary;
Dear ImGui remains code-defined and is not part of RML/RCSS asset reload.

### Platform and renderer

`PlatformApplication` and `PlatformWindow` translate SDL3 events into the
engine-owned `PlatformEvent` type. `UiSubsystem` arbitrates those events and
updates the active UI layer. `RenderSystem` records the immutable UI snapshot
through `UiOverlayRendererVulkan` after all configured render-graph passes and
before frame submission/presentation.

## Typed Runtime Boundary

The UI boundary has three explicit data types:

- `UiAction` is the only UI-to-engine mutation request. It carries an action
  type plus typed integer, floating-point, and string payloads.
- `UiViewModelSnapshot` is the game-thread read-only state published to the UI
  update step. It contains frame timing, active world, debug view, tone mapping,
  bloom, environment, SpeedTree wind, locale, and visibility state.
- `UiRenderSnapshot` is immutable draw data consumed by the renderer. It owns
  vertices, indices, clip rectangles, blend modes, generation-tagged RGBA8
  UI texture snapshots, and the pixel-space `sceneViewportRect` used as layout
  and picking metadata for the SDL3/Vulkan scene view. It does not own or
  reference a scene image.

RmlUi callbacks queue `UiAction` objects into `CommandBus`. ImGui material-
instance edits use the typed editor-command boundary and never mutate live
renderer state from a widget callback. `EngineLoop::ApplyQueuedUiActions()`
drains the queue at the stable beginning of a frame. UI visibility and locale
are applied to `UiSubsystem`, `UiActionType::Quit` is handled directly by
`EngineLoop`, and render, post-process, environment, SpeedTree, and material-
instance editor actions use their respective engine-owned command paths.

## Threading And Ownership

The game thread owns `UiSubsystem`, the live RmlUi context, the ImGui context,
input arbitration, asset reload, and view-model updates. The render thread
never traverses live RmlUi documents or a live ImGui context.

At the end of `UiSubsystem::Update()`, the subsystem builds one immutable
`UiRenderSnapshot` and publishes it through `UiRenderSnapshotQueue`. The queue
keeps only the newest snapshot. `RenderSystem` consumes the latest snapshot on
the render thread and retains the last consumed snapshot when no newer one is
available. The current V1 render loop waits for the render thread before the
next engine-owned renderer mutation, which keeps the GT/RT lifetime contract
deterministic while preserving the immutable boundary.

The queue is cleared before UI shutdown. UI shutdown occurs after the render
thread is stopped and the renderer has waited idle; renderer UI resources are
then destroyed with the rest of the render-system resources.

## Input Arbitration

`UiInputMode` and `UiInputOwner` are explicit in `UiInputOwnershipSnapshot`.
The implemented priority is:

1. global UI shortcuts
2. the visible RmlUi runtime page
3. the visible Dear ImGui developer UI
4. remaining game input

The runtime page uses `UiOnly` ownership for keyboard, pointer, controller,
and text input. With only the developer UI visible, the mode is `GameAndUi`:
relative mouse mode is disabled so the pointer remains visible, ImGui claims
only events covered by its `WantCapture*` or navigation state, and
`InputSubsystem` still drains SDL relative deltas every frame but publishes them
to the game only while the pointer owner is `Game` and relative mouse mode is
actually enabled. Releasing capture therefore exposes a free cursor without
continuing to rotate the game camera. The pass-through center dock region keeps
the directly rendered SDL3/Vulkan scene eligible for game camera input. Scene
picking converts window coordinates into that layout rectangle and uses the
same aspect ratio as rendering, so clicking a model continues to select the
correct material instance after docking. With no UI modal or developer panel
visible, the game owns input and the player's requested relative mouse mode is
restored.

`PlatformApplication` initializes SDL's gamepad subsystem, opens and closes
connected gamepads, and publishes connection lifecycle events. The lifecycle
updates `ImGuiBackendFlags_HasGamepad`; RmlUi D-pad and analog navigation use
reference-counted key state so every direction press has a matching release,
including axis recenter, focus loss, page close, and device removal. `F10` and
gamepad `Start` toggle the runtime page; `Esc` toggles the player's requested
mouse capture state; `F1` toggles developer UI;
gamepad `East` closes an open runtime page.

SDL focus events are forwarded to ImGui with `AddFocusEvent()` and clear RmlUi
pressed-key, pointer, controller, and composition state on focus loss. SDL
`TEXT_EDITING` updates the active RmlUi `TextInputContext` composition range;
the following UTF-8 `TEXT_INPUT` commits that composition without duplicating
text. Resize events update the UI context and continue to the engine resize path.

Touch, accessibility semantics, and safe-area policy remain explicit follow-up
capabilities rather than hidden assumptions.

## RmlUi Integration

`RmlUiSystemInterface` owns elapsed time, diagnostics routing, clipboard access,
and platform text-input activation. `RmlUiTextInputHandler` retains only the
currently active `TextInputContext` and translates SDL composition updates into
RmlUi selection, composition, commit, cancel, and focus-loss operations.
Candidate document validation captures RmlUi errors and warnings while parsing
and converts them into a runtime error without replacing the active document.

`RmlUiRenderInterface` converts RmlUi geometry into the CPU-side render
snapshot. It preserves scissor rectangles and transforms, packs premultiplied
vertex colors, captures generated or loaded textures, and assigns a generation
to every texture record. The renderer can therefore synchronize texture
content without exposing RmlUi handles to Vulkan code.

The content contract follows the RmlUi property set shipped in `extern/RmlUi`.
For example, font-family is a single registered family name and borders use
RmlUi-supported width/color declarations; authored CSS must not assume browser
CSS shorthand coverage.

## Dear ImGui Integration

When compiled in, `UiSubsystem` creates a docking-enabled ImGui context with
keyboard and gamepad navigation enabled. The subsystem owns `NewFrame()`, panel
construction, draw-data conversion, and shutdown. ImGui draw lists contain
developer panels only and are appended to the same `UiRenderSnapshot` as RmlUi
geometry, but the two widget models do not share state or callbacks. The
center dock is not an ImGui scene-image surface: it does not emit an
`ImGui::Image` command or consume a RenderGraph external texture.

The material editor runtime snapshot is built once per frame and shared by two
docked content surfaces. The left navigation lists only material slots from the
currently selected model; the right inspector owns document actions, tabs,
parameters, texture bindings, and status. Parameter or texture edits create
sparse overrides directly, while highlighted `Reset` actions restore the base
material value without a separate override checkbox.

The ImGui font atlas is copied into the RmlUi texture bridge and is rendered
through the same Vulkan texture-generation path. The UI overlay supports both
straight-alpha and premultiplied-alpha draw commands so the RmlUi and ImGui
outputs retain their intended blend behavior.

## Vulkan Overlay Contract

`UiOverlayRendererVulkan` is initialized after the renderer backend exists and
uses the generated `uiOverlay_vert.spv` and `uiOverlay_frag.spv` shaders. It
owns:

- a descriptor set layout and pool for UI textures
- a load-preserve swapchain render pass and per-image framebuffers
- straight-alpha and premultiplied-alpha graphics pipelines
- host-visible, per-swapchain-image dynamic vertex/index buffers
- white-texture fallback for untextured geometry
- texture generations and retired GPU textures

`RenderSystem::RecordAndSubmitCurrentRenderScene()` records the configured
SDL3/Vulkan render path first, then records the UI overlay into the same
swapchain image. The scene is therefore displayed by Vulkan directly; the
overlay only draws RmlUi and ImGui panels over it. `UiOverlayRendererVulkan`
does not own, sample, or import the scene image, and leaves queue submission
and present synchronization to `RendererBackendVulkan`.

Swapchain-dependent framebuffers, pipelines, and dynamic buffers are released
and rebuilt through the renderer resize lifecycle. Replaced or removed GPU
textures record `ResourceRetireQueue::GetLastSubmittedEpoch()` and are destroyed
only after `RendererBackendVulkan::BeginFrame()` advances the completed epoch
past their last use. Shutdown and resize wait for renderer idle before forcing
any remaining overlay retirement records to release.

## Asset And Hot-Reload Contract

`config/config.json` contains the `ui` object. Its fields are:

- `enabled`
- `hotReload`
- `developerUiEnabled`
- `developerUiVisible`
- `assetRoot`
- `document`
- `localization`
- `defaultLocale`
- `fontFaces`

Relative UI paths are resolved through `RuntimeConfig`, while the UI assets
remain repository-local under `ui/`. Configured font paths are tried first;
Windows platform fallback candidates are then attempted, including Segoe UI,
Noto Sans SC, Microsoft YaHei, and Arial when available.

Hot reload fingerprints the document, linked stylesheets, and localization
file. A changed candidate is parsed and validated before commit. If parsing,
required-element validation, or localization validation fails, the previous
valid document and localization table remain active and the failure is exposed
in diagnostics and the developer panel. The active page, visibility, locale,
and scroll position live outside the replaceable document and are restored on
successful commit. RmlUi texture records are erased once the current CPU
snapshot has captured them; published snapshots retain pixel data through
`shared_ptr`, while the render thread independently retires the matching GPU
resources by completed frame epoch.

## Configuration And Launch Controls

The build option `VULKANLEARN_ENABLE_DEVELOPER_UI` removes the Dear ImGui
compile-time dependency and code path when disabled. Runtime UI can be disabled
with `ui.enabled=false`; developer UI can be overridden at launch with:

```text
build/bin/main.exe --dev-ui
build/bin/main.exe --no-dev-ui
```

The normal frame smoke path exercises UI initialization when it is enabled:

```text
build/bin/main.exe --framesmoke 2 --exit-after-tests
```

`workerThreadCount=1` uses the synchronous render path. `workerThreadCount=2`
keeps UI updates on the game thread and consumes the immutable snapshot on the
render thread.

## Source Of Truth

- `source/ui/uiAction.h`: typed UI commands
- `source/ui/uiRenderSnapshot.h`: view-model, ownership, and draw-data types
- `source/ui/uiRenderSnapshotQueue.*`: game-thread/render-thread handoff
- `source/ui/uiSubsystem.*`: RmlUi, ImGui, input, localization, and reload
- `source/ui/rmlUiInterfaces.*`: RmlUi platform/render adapters
- `source/ui/uiOverlayRendererVulkan.*`: Vulkan overlay resources and recording
- `source/engine/engineLoop.*`: lifecycle, input priority, snapshots, actions
- `source/renderSystem.*`: final-pass overlay recording and renderer view model
- `source/platform/platformEvent.h`: platform-neutral event vocabulary
- `config/config.json`: runtime UI switches and asset paths
- `ui/`: versioned runtime UI content
- `shader/glsl/uiOverlay.*`: UI shader source

Generated SPIR-V under `shader/spv/` is produced by the normal startup shader
compile path and is not the shader authoring source.

## Verification Matrix

The implemented stack is considered healthy when all of the following hold:

1. CMake configures and the target links with the developer UI enabled.
2. A short frame smoke initializes RmlUi, loads a platform font, initializes
   the UI subsystem, starts the configured render-thread mode, and exits with
   code zero.
3. The smoke log contains no `RmlUi error`, `RmlUi warning`,
   `DocumentParseFailed`, or render thread failure entries.
4. A build configured with `VULKANLEARN_ENABLE_DEVELOPER_UI=OFF` omits Dear
   ImGui, while `--no-dev-ui` on an ImGui-enabled build skips its runtime
   initialization for that launch.
5. A resize rebuilds the UI swapchain resources without destroying the active
   document; direct SDL3/Vulkan scene display continues after the rebuild.
6. Repeated valid and malformed hot reloads keep the last valid document,
   release obsolete CPU texture records, and drain retired GPU textures after
   their completed frame epochs.
7. Focus loss, keyboard navigation, D-pad navigation, analog recenter, gamepad
   removal, UTF-8 input, and IME composition leave no stuck input state.
8. `Esc` releases mouse capture without publishing free-cursor motion to the
   game camera; UI ownership may suspend capture without overwriting the
   player's requested capture state.

The first three checks are automated by the repository's existing configure,
build, and frame-smoke commands. Interactive resize, hot reload, and visual
focus checks remain manual acceptance checks when a desktop session is
available.

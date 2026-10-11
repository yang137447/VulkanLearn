# Game UI Stack

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/ue-lite-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

## Status

Current contract, 2026-10-10: RmlUi serves both runtime controls and the
runtime-toggleable editor workspace. ImGui and its CMake option are removed.
SDL3 remains the event/text-input boundary; VulkanLearn records the final
overlay on the render thread.

`documents/plan/ui/game-ui-stack-plan.md` is historical adoption context.
`documents/plan/ui/rmlui-editor-plan.md` separates the current migration baseline
from future scene authoring and richer asset workflows.

## Responsibilities

`ui/runtime-control.rml`, `.rcss` and `localization.json` define the runtime HUD
and control page: scene selection, frame status, debug views, tone mapping,
bloom, CSM, camera speed, environment, vegetation, locale and visibility.
The runtime page temporarily hides the editor and owns all input.
Destructive validation workflows remain console/launch operations, not buttons.

`ui/editor-workspace.rml` and `.rcss` define the default workspace:

- Top: command toolbar.
- Right 31%, upper: Scene Outliner and expandable model/material tree.
- Right 31%, lower: Details, material slots, MI tabs, parameters, render states,
  texture bindings and status, or a generic read-only JSON inspector.
- Bottom left/center: Content Browser (MI/Mesh/Scene/Texture/Terrain JSON sources
  tree, breadcrumbs, search, type filter, tiles/list, sorting, size and explicit
  refresh controls) / Renderer
  (offline controls) tabs; switching only changes presentation.
- Left/center: a scene Viewport image and game input.
- Bottom edge: status bar.
- Dirty-close modal: explicit Save/Discard/Cancel.

This remains a material workspace, not a full scene-authoring tool.
Scene display uses a graph-owned offscreen image. GT owns only a logical
texture token and a framebuffer-pixel layout rectangle, never image handles.

The reusable `vl_ui_widgets` module and `ui/widgets/editor-widgets.rcss`
own common controls, row/tab composition and DOM refresh behavior.
`documents/contracts/ui-component-system.md` defines categories and ownership.

`MaterialInstanceAssetEditorPanel` borrows a document and consumes immutable
editor views. The model tree expands into material-slot children through the
same selection sink as the material-slot list. Its callbacks emit typed editor commands, scene selection
requests, or runtime actions; they do not invoke the document service,
mutate World, write assets, or touch Vulkan. `materialInstanceEditorView.h`
keeps the view types and command sink independent of any UI library.

Same-frame edits coalesce into an ordered revision-gated atomic batch.
Save and Validate include pending edits. Dirty close waits for a clean
authoritative snapshot; failed saves keep the document and dialog open.
Focused partial input is not overwritten by snapshots. Labels are RML-escaped.
Color is currently edited as Linear RGBA numbers, without an sRGB palette.
Texture edits can be saved; texture live-preview descriptor replacement remains
unavailable. Numeric/render-state preview retains the existing renderer adapter.
Content Browser selection is separate from the open document. A single click
selects an item; a double click on MI emits the existing editable-document open
command. Scene double-click reuses the existing `UiActionType::LoadWorld` transaction.
Mesh/Texture/Terrain double-click opens a separate resource-editor tab with a
type placeholder and read-only JSON Details. It does not load models, pixels,
or referenced `data`; tabs are deduplicated and can return to World.
Browser paths/filter/view/history are local presentation state.
Type icons and material spheres are placeholders, not real thumbnails or model
previews. The inspector does not replace MI editing or the scene Viewport.

### JSON Metadata Browsing

`EditorJsonAssetLibrary` owns a dedicated catalog/document read worker. It scans
the resource root once at initialization and again only on explicit Refresh;
open requests read the selected JSON. Each-frame GT `Poll()` only tries the
completion mailbox, with no per-frame catalog scan or asset-JSON I/O.
Pending requests coalesce, opens take priority (also between scan files), and
both worker delivery and GT publication reject superseded generations.
This is separate from UI-document hot-reload fingerprinting.

SAX parsing keeps top-level classification/name/description and bounded,
preorder field rows, not a full JSON DOM. Every `data` field and its subtree at
any depth (ASCII case-insensitive) is omitted from presentation and snapshots.
The containing JSON bytes are still read and syntax-checked, including omitted
subtrees; referenced payload files are never followed. Scans exclude `.git`,
`Generated` and `data` directories, do not recurse through directory symlinks,
and validate relative/canonical paths within the resource root.

Supported top-level types take precedence over filename prefixes; `speedtree`
is classified as Mesh. Recognizable malformed candidates retain diagnostics,
not partial field trees; unsupported explicit types are excluded.
`EditorJsonAssetLimits` in `source/editor/assets/editorJsonAssetLibrary.h` pins
4 MiB/file, 100000 source lines, depth/path-depth constraints of 64,
4096 displayed fields, 1024-byte display-text truncation budgets (plus `...`),
4096-byte JSON Pointers, 20000 catalog assets and 100000 visited scan entries.
Field truncation still validates remaining syntax. These are metadata input
limits, not runtime asset-loader contracts.

Catalog and document views are shared immutable snapshots. Browser indices
rebuild only on catalog identity/revision changes; filtering/sorting and markup
are dirty-driven, and hidden browsers defer that work. Grid/list windows mount
at most 192 item root nodes, including two overscan rows on each side within
that cap, plus at most two height spacers. This does not bound item descendants
or the Sources tree, and does not truncate the searchable/scrollable catalog.
Selection/dirty markers update in place; double clicks validate path, DOM
generation and scroll position. `EditorJsonInspectorController` provides
expand/search and at most 64 field rows per page, refreshing only when its
snapshot or presentation state changes. Constants and detailed contracts are
recorded in `ui-component-system.md`.

## Typed Boundaries

- `UiAction`: typed runtime-control mutation requests into `CommandBus`.
- `EditorCommandEnvelope`: asset-document mutation requests into `EditorCommandBus`.
- `UiViewModelSnapshot` and `MaterialInstanceEditorSnapshot`: GT read-only presentation state.
- `IEditorJsonAssetRequestSink`: read-only open/refresh intents to the GT facade.
  `MaterialInstanceEditorSnapshot.assetCatalog` and `.jsonDocument` retain
  `shared_ptr<const ...>` results without per-frame deep copies of assets/fields.
- `UiRenderSnapshot`: immutable vertices, indices, clip rectangles, blend modes,
  generation-tagged RGBA8 textures and `sceneViewportRect` layout/picking metadata.
  `sceneViewportEnabled` selects offscreen presentation. A reserved
  `SceneViewportTextureId` references the current renderer-owned output
  logically; it carries no pixels, Vulkan handles or GPU ownership.

`EngineLoop::ApplyQueuedUiActions()` drains runtime requests at the stable
frame boundary. The MI runtime drains editor commands separately, validates
document revisions and performs renderer-owned preview preparation.
There is no second UI-to-renderer mutation route.

## Threading And Ownership

GT owns `UiSubsystem`, the RmlUi context, both documents, input arbitration,
reload and view updates. At the end of Update it publishes one immutable
snapshot through `UiRenderSnapshotQueue`, which retains only the newest.
RT consumes that snapshot and may retain the last one when no new one exists.
RT never traverses live documents or asset sessions.
The JSON worker performs only bounded filesystem/parse work, never RmlUi,
World mutation or Vulkan access. GT alone publishes its completed snapshots;
shutdown joins the worker before releasing the library's snapshots.

The V1 loop waits for RT before the next engine-owned renderer mutation.
Shutdown clears the queue after stopping RT and waiting for renderer idle,
then destroys UI and renderer resources through their existing owners.

## Input

Priority is global shortcuts, runtime page, editor workspace, then game.
The runtime page uses `UiOnly`. The editor uses `GameAndUi` with a visible
cursor; RmlUi hit testing, keyboard focus, drag capture and modal state claim
panel input. The Viewport image passes through to the game, and clicking it blurs
editor focus. A dirty-close modal claims the window. No visible UI restores
the player's requested relative mouse mode.

`InputSubsystem` drains SDL relative deltas each frame but publishes them only
when pointer ownership is Game and relative mouse mode is actually enabled.
Releasing capture cannot keep rotating the camera.

F10/gamepad Start toggle runtime controls, F1 toggles the editor, Esc toggles
requested mouse capture, and gamepad East closes the runtime page.
Ctrl+S saves the active MI when no close modal is open.
RmlUi keyboard/D-pad/analog navigation uses reference-counted press state.
Focus loss clears keys, pointer capture, controller and composition state;
device removal clears controller state. Resize updates RmlUi dimensions and
continues through the engine's swapchain lifecycle.
SDL TEXT_EDITING updates composition; UTF-8 TEXT_INPUT commits once.
Touch, accessibility semantics and safe-area policy remain future work.

## RmlUi And Reload

`RmlUiSystemInterface` handles time, diagnostics, clipboard and platform text
activation. `RmlUiTextInputHandler` borrows the active TextInputContext and
translates composition, selection, commit/cancel and focus loss.
`RmlUiRenderInterface` captures scissor/transform data, premultiplied colors
and generated/loaded textures into CPU snapshots with texture generations.
There is no separate font atlas, input bridge or draw-data converter.

RmlUi colors are authored and premultiplied in sRGB. When the native surface
uses an sRGB attachment, compiled geometry unassociates, linearizes and
repremultiplies vertex RGB once; floating-point vertex colors avoid dark-color
byte quantization. Loaded straight RGBA and generated premultiplied RGBA are
converted once at their registration boundaries into linear premultiplied
UNORM pixels. Alpha is not gamma-corrected. Ordinary UNORM surfaces retain the
previous encoded-color path; the surface format is fixed for the UI context.
This intentionally uses Vulkan's linear-space compositing rather than the
nonlinear compositing in RmlUi's GL3 reference backend. The reserved scene-image
token bypasses CPU texture conversion, so scene display is not linearized twice.
Shader locations and reload descriptor ABI stay unchanged.

RML/RCSS must use the property set shipped in `extern/RmlUi`, not assume
browser CSS shorthand support. Font configuration tries explicit faces first,
then Windows fallback faces including Segoe UI and Microsoft YaHei.
The editor stylesheet currently selects Segoe UI.
Document source URLs use forward slashes (`path.generic_string()`), including
on Windows, so relative RCSS/image URLs resolve from the document directory.
Shared editor scrollbars declare explicit dimensions; an auto-width RmlUi
vertical scrollbar can otherwise consume the content width and collapse a
wrapped asset grid into a single column.

`WidgetDocument` caches markup and `(element id, property, value)` writes,
avoiding repeated RmlUi DirtyProperty invalidation on unchanged layout/display
values. Attach/Detach clear caches; subtree replacement invalidates only
destroyed descendants, preserving unrelated layout caches during text refresh.
Dock placement and panel visibility use the same property cache.

UI asset fingerprints drive candidate reload. Localization, runtime document
and enabled editor document must all parse and pass required-control validation
before replacement; errors or warnings reject the candidate. Failure retains
the last valid documents. Runtime page, visibility, locale and runtime scroll
state live outside replaceable documents. MI state remains in its runtime,
not the RML document. Editor focus/scroll may reset on successful reload.

Published CPU texture data is retained by shared ownership. RT independently
retires replaced GPU texture generations by completed frame epoch.

## Vulkan Overlay

`UiOverlayRendererVulkan` initializes after the renderer backend and owns its
texture descriptor layout/pool, load-preserve and clear-present render passes,
framebuffers, blend variants, per-image dynamic buffers, white fallback,
GPU texture generations and retirements. Existing straight/premultiplied
pipeline pairs remain transactional shader-reload participants; removing a UI
producer does not change their ABI or retirement policy.

RenderSystem records configured scene passes first. Ordinary game frames overlay
RmlUi onto the same swapchain image; editor frames sample the graph-owned scene
output while composing the workspace into swapchain. The overlay borrows the
scene view/sampler but never owns the image. RendererBackendVulkan retains
submission/present ownership. Resize rebuilds framebuffers, pipelines and dynamic buffers. Texture retirement
uses last-submitted/completed epochs; shutdown and resize wait for idle before
forced release.

Per-image vertex/index buffers reuse sufficient capacity and grow geometrically
only when needed: 256 KiB vertex / 128 KiB index bases, doubled until sufficient
(`NextBufferCapacity` in `source/ui/uiOverlayRendererVulkan.cpp`).
CPU snapshot vectors reserve previous-frame vertex/index/draw counts.
Draw snapshots are still produced each frame; these are allocation/layout
optimizations, not a zero-work or measured-FPS guarantee.

### Independent Scene Viewport

RenderGraph owns one sampled single-sample color image per acquired swapchain
image, plus pipeline-compatible alternate render passes/framebuffers for
present-output passes. Editor frames render the final scene into that image,
not into swapchain and not by screenshot/cropping. RT transitions color writes
to shader-readable layout before the UI image samples it. UiOverlay owns
per-image descriptors and updates only the slot whose fence BeginFrame has
waited for. A compatible clear-present pass composes the workspace into
swapchain; ordinary game frames keep the original present path.

Graph candidates, no-throw state swaps and immediate/epoch retirement include
the added targets and alternate passes. The UI does not cache a graph pointer
or import the target into its CPU texture map. Offline display borrows the
current output and retires replaced framebuffers through the same epoch queue.
Straight/premultiplied UI pipeline reload remains unchanged.

The GPU backing resolution follows native-window/swapchain extent. Splitter
movement changes logical viewport size and camera projection, but does not
reallocate the graph on every pointer move or add a GPU idle wait.
The UI samples the full image UV range. EngineLoop updates camera projection
before freezing the World snapshot; CSM derives aspect from that snapshot.
Picking subtracts the viewport origin and uses its logical dimensions.
Offline film/preview uses logical viewport size; display validation uses that
size and resamples to the backing image before UI presentation.

Present-output passes must be single-sample; the first viewport writer cannot
LOAD uninitialized contents and the final writer must STORE its result.
These are graph-source validation boundaries, not per-frame shader fixes.
Native multi-window rendering and adaptive GPU backing resolution are not
part of this implementation.

## Configuration

`config/config.json -> ui` retains `enabled`, `hotReload`, `developerUiEnabled`,
`developerUiVisible`, `assetRoot`, `document`, `localization`, `defaultLocale`
and `fontFaces`. The developer names now select the RmlUi editor, not a library.
`--dev-ui` enables it; `--no-dev-ui` skips editor initialization while retaining
runtime controls. `ui.enabled=false` disables the complete UI subsystem.
There is no developer-UI CMake switch.

`workerThreadCount=1` renders synchronously; `2` consumes snapshots on RT.
The normal shader build path produces overlay SPIR-V from `shader/glsl/`.

## Verification

Root-registered GoogleTest target `editor_ui_tests` loads real RML/RCSS with
CPU fake sinks and covers parsing, refresh, focused input, edit batching,
revision preservation, save/dirty close, escaping and typed offline actions.
Existing MI suites retain document/codec/service and fake-preview coverage.
Neither suite proves Vulkan preview replacement or interactive IME behavior.
JSON-library suites cover nested data omission, classification, input/path
limits, stale worker results, immutable publication and idle polling without
rescans. Inspector tests cover read-only routing, escaping and 64-row pages.
Browser performance tests use synthetic 10000/50000-item snapshots for bounded
DOM, warm-frame caching, whole-row spacers and scroll/double-click identity;
the worker's real catalog cap remains 20000. Widget/dock tests cover property
cache invalidation and idle layout writes. CPU fake-GPU checks are not actual
GPU/FPS acceptance.

Application acceptance requires normal startup with editor enabled/disabled,
one-/two-thread smoke without UI/Vulkan errors, actual window output, scene
picking, resize, focus/IME/gamepad behavior and valid/invalid reload checks.
Use existing launch/console capabilities; do not add runtime-test flags.
Unperformed interactive checks must remain explicit verification gaps.

# Game UI Stack

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/architecture-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

## Status

Current contract, 2026-10-12: RmlUi serves the minimal runtime HUD and the
runtime-toggleable editor workspace. ImGui and its CMake option are removed.
SDL3 remains the event/text-input boundary; VulkanLearn records the final
overlay on the render thread.

`documents/plan/ui/game-ui-stack-plan.md` is historical adoption context.
`documents/plan/ui/rmlui-editor-plan.md` separates the current migration baseline
from future scene authoring and richer asset workflows.

## Responsibilities

`ui/runtime-hud.rml` and `.rcss` define a non-interactive scene-name and gameplay-text HUD.
Optional C# gameplay publishes bounded text through `Blueprint.Hud.SetText/Remove`.
`ScriptRuntimeView.hudLines` carries frozen values; GT binds their text to
`game_ui.gameplay_text`, and RmlUi text interpolation escapes markup.
Pause preserves the last text; Stop, World replacement and publisher cleanup clear it.
Scripts have no DOM or layout access. The existing HUD visibility policy applies.

World-space debug geometry uses the shared [DrawDebug](debug-draw.md) collector.
GT freezes geometry and camera values; the UI adapter projects lines/points and
records escaped labels in a separate non-interactive RmlUi context. These commands
sit after the scene image and before workspace tools/menus or the immersive HUD.
The adapter shares the ordinary immutable UI snapshot and texture ownership path.
Optional DepthTest vertices carry projected device Z. RT borrows the resolved scene
depth for fragment comparisons, using the logical scene viewport for UV mapping;
ordinary UI vertices retain Overlay behavior. The depth descriptor follows the
acquired-image fence and swapchain resource lifecycle. See the DrawDebug contract
for missing-depth and MSAA behavior.
The viewport DrawDebug menu only produces typed controls with World generation.
The former Runtime Debug page, Settings button, F10/gamepad Start entry points
and runtime-page binding callbacks are removed. Scene selection, actor settings,
viewport modes and offline controls belong to the editor workspace; runtime
commands remain available independently of the deleted page.
Destructive validation workflows remain console/launch operations, not buttons.

`ui/editor-workspace.rml` and `.rcss` define the default workspace:

- Top: command toolbar.
- Right 31%, upper: Scene Outliner and expandable model/material tree.
- Right 31%, lower: Details, actor properties and material slots, or a generic
  read-only JSON inspector.
- Central MI tabs: preview sphere with parameters, render states, texture bindings,
  save/preview controls and document status.
- Central Blueprint tabs: Components tree, real 3D viewport and Details for class/defaults,
  Instance Editable permissions, component transforms, mesh/camera configuration and root physics.
- Central Retarget tabs: read-only source/target bind and calibrated skeleton overlays,
  orthographic views, mapped joint selection, local axes and anchored position/chain measurements.
  An optional target SM reference also enables side-by-side source/target animated meshes with
  shared clip, play/pause and seek controls through the existing preview World.
  Reference values come from the asset worker; animation uses the production retarget and skinning path.
- Bottom left/center: Content Browser (MI/Mesh/TressFX/Scene/Texture/Terrain/Blueprint/Retarget JSON sources
  tree, breadcrumbs, search, type filter, tiles/list, sorting, size and explicit
  refresh controls) / Renderer
  (offline controls) / Log (Output and Scripts) / Performance (FPS, CPU frame ms,
  GPU timed total and per-pass table) tabs; switching only changes presentation.
- Left/center: a scene Viewport image and game input.
- Bottom edge: status bar.
- Dirty-close modal: explicit Save/Discard/Cancel.

The workspace supports actor property editing with GT-owned scene drafts and
explicit conflict-aware scene saves and bounded Scene Undo/Redo through formal World reload.
Optional [C# Blueprints](csharp-blueprint-runtime.md) add placement, scalar properties and
Play/Pause/Stop through value-only UiActions. GT validates candidates, stages the
shared Scene document and queues formal LoadWorld; callbacks never invoke the host.
Blueprint assets also use native RmlUi drag/drop into the level viewport; instance
overrides/reset preserve sparse Scene data. Creating an asset emits a GT request
with the target path and actor identity; callbacks never read or write files.
Esc cancels a Blueprint drag before the normal mouse-capture toggle.
The Outliner creation form also offers Create Blueprint Asset, emitting a registered class/name/path
request. GT creates a sparse BP JSON configuration and refreshes the asset catalog.
It neither creates source files nor requests compilation; C# catalog changes use the assembly reload transaction.
Blueprint Details also emits rename/duplicate/delete, inline component tree and root
physics edits. Asset-owned component fields are editable on instances only when the BP
explicitly exposes them; inherited root physics remains read-only.
Scene scalar overrides require explicit Instance Editable permission from the BP
asset, or the class attribute for inline Blueprints. Both GT requests and formal
loading enforce this permission. Blueprint tabs produce revision-gated `blueprint.*`
document commands; they do not mutate scene instances or run managed callbacks.
Scene history is unavailable in asset pages, offline mode or Play/Pause.
Blueprint pages instead route Edit Undo/Redo to their own revision-gated document history.
Frozen per-path command completion feedback releases UI waiting state even when rejection leaves
the document revision unchanged; history never writes files automatically.
Level editing navigates an independent World-owned editor camera. The authored
scene Camera remains editable without following viewport movement; immersion
returns to the scene camera. Model/material previews keep their existing camera.
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

The Content Browser merges registered C# Blueprint classes as UI-only virtual entries.
Class actions emit a class identity for GT source opening or a class/path/name request for
JSON Blueprint creation. Source editing opens VS Code through a validated resource file URI;
classes have no engine document tab and do not enter the JSON asset library.
One class can supply multiple configuration assets with sparse property overrides.

Same-frame edits coalesce into an ordered revision-gated atomic batch.
Validate includes pending edits. Save opens confirmation after flushing pending
edits and waits for an authoritative revision before enabling confirmation.
The shared Save Content list includes dirty Scene, Mesh, Texture, Blueprint and MI files, supports
individual checkbox selection, and never writes merely by opening.
Content Browser marks these types in place and offers a path-targeted Save action.
Mesh/Texture drafts enter through the [standard asset API](asset-modify-api.md).
Dirty close waits for a clean
authoritative snapshot; failed saves keep the document and dialog open.
Focused partial input is not overwritten by snapshots. Labels are RML-escaped.
Color is currently edited as Linear RGBA numbers, without an sRGB palette.
Texture edits can be saved; texture live-preview descriptor replacement remains
unavailable. Numeric/render-state preview retains the existing renderer adapter.

The dockable Log panel consumes the `logs` field of the `assets` view-model
section, an immutable process-wide `LogStore` snapshot published once per
revision (no copy per panel build). Diagnostics, RmlUi messages, managed logs, C#
compiler output and Shader Compiler cache/build diagnostics carry sequence, timestamp,
severity, source and optional file/line/column. Shader cache hits are Debug, misses
and successful build summaries are Info, failed summaries are Error, and successful
shaderc compilations report Warning only when shaderc returns warnings. Existing
console output is preserved; shader compilation failures continue through their
existing upper-level exception diagnostics rather than adding duplicate errors.
The thread-safe store retains at most 4096 entries and 4 MiB of text; snapshots are
shared until the revision changes. Output supports severity/source/text filters and
auto-scroll. Clear advances a panel-local sequence watermark and leaves diagnostic
history and existing console output intact. Rows are virtualized and hidden panels
defer filtering/DOM work. Scripts reads frozen timing, per-object callback, physics,
assembly and GC metrics at most four times per second, with 64 objects per page.
Neither Log view invokes managed code, reads World, or modifies diagnostic producers.
The Performance tab consumes the `performance` view-model section, a value copy of
`PerformanceSnapshot` frozen by EngineLoop at the stable frame boundary from the renderer's
thread-safe timing snapshot. It is fed through `UpdatePerformance()` on the 100 ms
window (the Log "Scripts" page through `UpdateScriptPerformance()`), so a new
performance sample never triggers a workspace panel `Build`. It shows FPS, CPU frame ms, the sum of valid GPU pass samples,
a CPU stage table, and GPU rows in graph pass order with name, ms, percent and a proportional
bar; rows are not sorted by cost. The CPU table sits under the summary with its own heading and
header, uses the same row/bar markup, and normalizes each stage against `cpuMs`; its derived
`other` row closes the gap to CPU frame and is shown with a distinct colour. The panel is a pure
presentation consumer: it does not read World, assets, `RenderSystem`, query pools or Vulkan
objects, and it has no command callbacks. It rebuilds its DOM at 5 Hz (0.2 s) rather than every
frame, and refreshes immediately when the tab becomes visible. Unsupported timestamps, a
not-yet-read-back window and a window with no valid sample show explicit waiting/unsupported
text and `n/a`/`--`, never fabricated zeros.
The same snapshot feeds `perf json` and `--perf-jsonl`; see
[gpu-pass-timing.md](gpu-pass-timing.md).
The seven permanent dock panels include Log and Performance; version-1 saved layouts migrate by
adding Log to the Content stack, and version-1/2 layouts also gain Performance there while
retaining active tabs, ratios and hidden panels. Version-3 layouts require both panels.
Content Browser selection is separate from the open document. A single click
selects an item; a double click on MI emits the existing editable-document open
command. Scene double-click reuses the existing `UiActionType::LoadWorld` transaction.
Blueprint double-click opens its independent configuration tab with a `blueprint.get_document`
request. Its central 3D image uses the shared renderer-owned scene viewport.
`PreviewBlueprint` carries only path/revision; GT checks the authoritative BP draft and
prepares its visual components in `SC_model_preview`. Component/root transform edits
debounce for 350 ms; scalar defaults/permissions do not reload geometry. The preview
does not create a managed context or simulate physics. Matching Blueprint identity and a
new World generation gate image publication and camera input. Returning reloads the
remembered level. BP drafts reach formal scene instances only after explicit save.
The scene selector includes `blueprintInstance` levels and excludes editor-only `terrain`;
type filtering does not replace formal scene or C# host validation.
The Blueprint viewport supports mesh-component bounding-box picking and three-axis authoring gizmos.
Translation follows parent axes, rotation edits local YXZ Euler components, and scale follows local axes.
Dragging sends generation/revision-gated values to GT and continuously updates the real mesh/light
transforms without reloading the World; release submits one document edit and cancellation restores
the preview baseline. Self supports root rotation/scale, and the toolbar supplies translation,
angle and scale snapping. Camera, light and Scene components have pickable helper graphics;
mesh hits take priority over Self collision wires. Right drag orbits and middle drag pans.
Stale preview geometry or a changed viewport cancels editing. No node graph, world-space gizmo
or level gizmo is provided.
MI tabs request `PreviewMaterial`, using the formal preview ball with an instance-only
material binding in `SC_model_preview`. The material image supplies the viewport rectangle
and uses UI-owned, generation-guarded orbit/pan/zoom input, just like the model image.
The image waits for the matching material slot in the new World; reconnecting reapplies
dirty numeric/render-state drafts without saving. Texture live preview remains unsupported.
Mesh double-click opens a resource-editor tab beside the level tab in the same stack, requests the typed
`UiActionType::PreviewModel` route, and loads the model into the neutral
`SC_model_preview` World through `LoadWorld.extraSceneObjects`; its JSON Details
remain read-only. Texture double-click decodes its source image on the asset worker
for a bounded RGB preview; Terrain only opens metadata. Neither reads embedded `data`.
Resource tabs are deduplicated and
can return to World.
Mesh tabs display the renderer-owned `vl://scene-viewport` output in
`resource-preview-image`. The visible model image supplies the viewport rectangle;
resource-editor visibility no longer disables offscreen output. Publication waits
for a newer World snapshot containing `preview_model` in `SC_model_preview`, so
metadata completion alone does not expose the previous scene as a model preview.
Refreshing snapshots preserves the selected page; only explicit open/switch/close
actions activate another document. Returning to the level requests its remembered
scene path through LoadWorld; accepted unsaved actor edits are replayed into the
new generation only if the scene source digest still matches the draft baseline.
Observer movement and temporary visibility are not scene-document edits.
Changing model tabs and reloading request the selected model again. This still
uses the single active World and fixed injection scale, not an independent
World per tab. The preview camera frames the transformed model bounds on publication.
Model-image input remains UI-owned: left/right drag orbits, middle drag pans,
wheel zooms, and double-click or the frame button resets framing. Typed navigation
actions are applied on GT only for the matching World generation; focus loss cancels
dragging. Switching/reloading tabs resets the camera, without modifying model assets.
Browser paths/filter/view/history are local presentation state.
Resource tabs retain their completed JSON snapshots. Activating, reopening, or
closing onto an unfinished tab reissues its metadata request because the shared
reader keeps only the latest request; frame refresh never retries it repeatedly.
Texture grid tiles use asynchronous, bounded, disk-cached RGB thumbnails from a
separate CPU worker. Only visible tiles (plus the existing two-row overscan) request
images; completion updates image attributes without rebuilding the list. Scene input
pauses new work/publication, and hidden tiles release UI textures. The cache uses
BLAKE3 content keys and is revalidated on explicit Refresh. Mesh and realtime MI
tiles use a separate offscreen render session with their real material shaders,
the formal preview ball and neutral HDRI. GT advances preparation after 750 ms of
input inactivity; graph, descriptors and IBL never replace the active World.
Pointer hover motion does not restart this delay; scrolling, clicking, keyboard/text
input, resizing and captured viewport/editor interaction still do. Pending Mesh/MI
requests do not block the CPU texture queue. Each lane has one completion slot;
render requests remain single-claim and wait for polling before the next claim.
Rendered thumbnails are session-only; CPU import/shader preparation/texture decode
use a single asynchronous job, while GPU upload and pipeline creation remain on GT.
Rendered Mesh/MI results stay on GPU: a copy into a distinct renderer-owned sampled
image replaces readback and UI pixel upload. Completion publishes a logical image
token only after the existing dedicated fence completes. The 256-entry thumbnail
LRU, frozen UI snapshots and retiring overlay descriptors pin tokens; backend image
ownership survives preview-session reset and window resize, and expired tokens
retire their images through frame epochs. Overlay descriptors borrow the views and
sampler without owning their destruction. Compatible sRGB/UNORM views preserve the
linear/encoded UI sampling policy. CPU texture thumbnails keep their pixel path.
Scene icons and unsupported offline-only materials remain placeholders.
The inspector does not replace MI editing or
the scene Viewport. See the UI component contract for format and cache limits.

### JSON Metadata Browsing

`EditorJsonAssetLibrary` owns a dedicated catalog/document read worker. It scans
the resource root once at initialization and again only on explicit Refresh;
open requests read the selected JSON. Each-frame GT `Poll()` only tries the
completion mailbox, with no per-frame catalog scan or asset-JSON I/O.
Pending requests coalesce, opens take priority (also between scan files), and
both worker delivery and GT publication reject superseded generations.
Explicit Texture opens additionally decode PNG/JPEG/TGA/BMP source bytes through
TextureIO, within the canonical resource root (64 MiB input, 32 MP decoded limit).
The worker publishes an immutable sRGB display preview with a 1024-pixel longest
edge; linear source RGB is display-encoded, alpha is ignored intentionally for
normal/packed textures. The UI bridge weakly registers revision-keyed pixel sources;
Rml textures use the existing immutable snapshot and GPU retirement path. Tabs retain
their CPU previews; closing them releases ownership. Reload publishes a new revision.
Unsupported formats and missing/corrupt sources show diagnostics, never a previous image.
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

## Change-Driven Refresh

The HUD has only a world-name binding, dirtied only when its displayed name
changes. The deleted debug page has no per-frame binding or formatting work.
Editor FPS text refreshes at 250 ms intervals using a monotonic clock,
independently of simulation time.

The editor offline controls skip formatting and DOM lookup when their input
snapshot is unchanged. User edits invalidate that cache; focused partial input
remains untouched, and the first unfocused update reconciles the authoritative
value. Document reattachment invalidates both controls and metrics caches.
RmlUi input processing stays immediate, but the context update and draw-snapshot
publication are gated by `UiUpdateScheduler` (see "Update Skip And Snapshot
Retention"); an input event always forces both in the same frame, so hover,
pressed, focus and drag feedback are not left stale.
Actor helper icons retain fixed layout boxes and update screen position through
draw transforms. Selected helper lines retain slots for their unclipped topology;
clipping changes transforms/visibility rather than replacing DOM. Camera motion
therefore does not invalidate workspace layout through helper markup updates.

This is binding/control invalidation, not dirty-rectangle rendering. When the UI
is unchanged GT does not build or publish a new draw snapshot, so RT keeps
compositing the last immutable one; unchanged panel data does not imply reused
GPU draw buffers, only that an unchanged snapshot is re-uploaded from the same
CPU vertices.

### View Model Sections, Versions And Cadence

`source/ui/uiViewModelSections.h` defines the immutable building block
`UiSnapshotSection<T> { std::shared_ptr<const T> data; uint64_t version; }`.
The monolithic `UiViewModelSnapshot` remains only as the **projection** that
`UiSubsystem` assembles from sections for the existing panel API; it is no
longer the producer-side snapshot. The visibility/cadence decision stays in
`UiViewModelRefreshSchedule` / `UiViewModelBuildMode`
(`source/ui/uiViewModelRefresh.h`), derived only from visibility:

- **None** — `UiSubsystem` is not initialized, or both the developer workspace
  and the runtime HUD are hidden (immersive mode). No section is published, no
  RmlUi document update runs. GT still freezes the camera and DrawDebug overlay
  and publishes an immutable draw snapshot with no UI documents and
  `sceneViewportEnabled=false`, so RT cannot retain the previous workspace
  image. The last projection stays in place for `TickMaterialInstanceEditor`.
- **RuntimeHud** — `--no-dev-ui` (or an enabled-but-hidden editor): only the
  runtime HUD is visible. GT freezes the world label and gameplay text every
  frame through `ScriptRuntimeSubsystem::HudView`; no editor section is built.
- **FullEditor** — the developer workspace is visible. Each section is
  rebuilt on its own change signal, not as one bundle.

#### Section list, owner and version source

| Section | Owner | Payload | Version source |
| --- | --- | --- | --- |
| `scripts` | `EngineLoop` | `ScriptRuntimeView` (classes, feedback, build state, undo/redo) | Existing change events: world generation/path, script play state, script build start/finish, document commands, selection-triggered refresh. Rebuilt only when an event declares it, so `CanUndo`/`CanRedo` (two `weakly_canonical` calls) are not recomputed on idle frames. |
| `animation` | `EngineLoop` | preview clip/time/playing/joint count/feature values | Time-dependent: resampled every 100 ms cycle; published only when the values differ, so paused/absent previews do not bump the version. |
| `sceneOutliner` | `UiSubsystem` | `shared_ptr<const vector<EditorSceneActorSnapshot>>`, MI model list, selected identity/slot | Actor/model `shared_ptr` identity plus `outlinerRevision`, which advances only when the pointer is replaced or the selection changes. |
| `assets` | `UiSubsystem` | `LogStore` snapshot, JSON catalog/document, thumbnail snapshot, loading flags | The libraries' own revisions (same revision returns the same `shared_ptr`); `PublishAssets` compares pointer identity, so N polls with no change publish once. |
| `performance` | `EngineLoop` | frozen `PerformanceSnapshot` + script-runtime time/counter JSON | Time series: one publication per sampled 100 ms window. Does not feed the panel build gate. |
| `viewportCamera` | `EngineLoop` | camera/exposure/debug-view/tone-map, motion-debug identity, debug-draw UI signature | Value comparison. The per-frame debug-draw geometry allocation is represented by a signature over the fields the panel shows (enabled/categories/owners/counts), so static cameras do not dirty this section every window. |
| `environmentLighting` | `EngineLoop` | bloom/CSM/environment/speed-tree authoring values and environment progress | Value comparison against the last published payload. |
| `offline` | `EngineLoop` | offline session state/settings/error/command feedback | Value comparison; the UI-owned requested-mode/notice fields stay in `UiSubsystem` and are reconciled on publication. |
| `status` | `UiSubsystem` | locale, hot-reload status, workspace visibility, active world path | Value comparison, checked every frame; only a real change bumps the version. |

`EngineLoop` owns the six GT-live sections and publishes them through
`UiViewModelSectionStore`; `UiSubsystem` owns `assets`, `sceneOutliner` and
`status` through the same slot type. `MergeSectionVersions()` produces the
single `UiSectionVersions` used for dirty-set decisions.

#### Publish, dirty and rebuild rules

- A section is immutable once published. `UiSectionSlot::Publish(value,
  revision)` replaces the `shared_ptr` only when the owner-provided revision
  changes; `PublishIfChanged(value)` does the same after a value comparison.
  An in-flight consumer can keep the old `shared_ptr` indefinitely.
- `UiSectionVersions::ChangedFrom(previous)` returns a bit mask. `UiSubsystem`
  applies only those sections to the projection and calls `SyncWorldTextBinding`
  / `SyncGameplayTextBinding` only for `status` / `scripts`. `world_text` and
  `gameplay_text` are therefore the only RmlUi bindings and are dirtied only
  when their owning section changes.
- `BuildDeveloperPanels()` runs only when `scripts`, `assets`, `sceneOutliner`
  or `status` changed (or a panel-local interaction requested one build).
  `UpdateRuntimeView()` runs for `scripts`, `animation`, `viewportCamera`,
  `environmentLighting`, `offline` or `status` changes. `performance` triggers
  neither: the Performance tab and the Log "Scripts" page consume it through
  `UpdatePerformance()`/`UpdateScriptPerformance()` on the 100 ms window at
  their own 5 Hz sampling.
- Event-driven refresh keeps a section mask instead of a global dirty flag.
  `UiAction` types map to the sections they can change (camera actions →
  `viewportCamera`, offline actions → `offline`, authoring values →
  `environmentLighting`, script commands → `scripts`/`assets`/`sceneOutliner`,
  world/preview changes → all). Selective choice, mode switches, world loads
  and editor commands therefore still become visible on the next GT frame
  (≤ one rendered frame); only genuinely unchanged sections are skipped.
- What remains per-frame: `frameIndex`/delta time/FPS, the frozen camera and
  DrawDebug overlay values (needed by the immersive overlay), input processing,
  `status` publication and input-ownership evaluation. `rmlContext->Update()`
  and `UiRenderSnapshot` publication happen only when the update scheduler says
  so (below).
- `--gpu-stats` additionally logs one `[UI] sections/s=… panelBuilds/s=…
  runtimeViews/s=… bindingDirty/s=… rmlUpdates/s=… snapshotPublishes/s=…
  skipped/s=…` line per second with the per-section breakdown. It is a
  diagnostic of the gate, not a new data path.

#### Update Skip And Snapshot Retention

`source/ui/uiUpdateScheduler.h` answers one question per visible frame: does the
UI need an `rmlContext->Update()` and a new immutable `UiRenderSnapshot` now?
`UiUpdateScheduler` is a pure decision plus the timestamp of the last real
update; it never polls DOM or widget state. Inputs:

- any view-model section version changed since the applied set;
- any RmlUi-bound input event since the last update: pointer move/down/up/leave,
  wheel, key down/up, text/composition, gamepad button/axis;
- any DOM or content mutation: panel build, tab switch, document show/hide,
  hot-reload commit, locale commit, resize, World change, binding text change,
  or an explicit forced refresh;
- a change of visible surface (hidden ↔ runtime HUD ↔ workspace);
- the fallback cadence.

The fallback cadence is **10 Hz** (`UiUpdateFallbackInterval`, 100 ms). The
reasons: EngineLoop already samples FPS/performance into the view model at
100 ms, so no displayed number can change faster; the UI RCSS defines no
`transition` or `@keyframes`, so there is no element animation to advance; and
runtime HUD text is event-driven (script lines compared by value), with the
cadence acting only as a safety net for a binding nobody dirtied.

The runtime HUD and the editor workspace load into the **same** `RmlUi::Context`.
`Context::Update()` advances every loaded document, so there is no HUD-only
update path and gating the workspace independently is impossible without a
second context. `ApplyDocumentState()` makes the two documents mutually
exclusive, so the scheduler runs once per visible surface: `RuntimeHud` for
`--no-dev-ui` (or an enabled-but-hidden editor), `Workspace` for the editor. HUD
text changes go through the same value-compared bindings and force the update
directly; the 10 Hz cadence is shared.

When the decision is negative `UiSubsystem` neither calls `Context::Update()` nor
publishes. `UiRenderSnapshotPublisher` retains the last published immutable
snapshot and RT keeps drawing it; the queue is only replaced with a complete
snapshot and is never cleared by a skip, so RT cannot observe a torn or empty
intermediate. Hidden frames keep their previous behaviour: every hidden frame
still publishes a snapshot with no UI documents (only the DrawDebug overlay) and
`sceneViewportEnabled=false`, overwriting the retained workspace image.

The first visible frame after being hidden is always a full update: the hidden
branch calls `MarkHidden()`, so the next visible evaluation reports a surface
change and publishes a complete snapshot even when no section version changed.

DrawDebug is the one intentional exception. Its geometry is rebuilt from
World/camera state outside RmlUi, so while the overlay is visible with geometry
(or the last published snapshot contained geometry) the snapshot is republished
every frame to keep moving geometry and expiry correct. The RmlUi context itself
still follows the normal update decision; `publishSnapshot` can therefore be
true while `updateRml` is false.

Interaction latency remains one GT frame for input-driven changes (the event is
processed, the flag is set, the same frame's update runs) and up to 100 ms for
purely time-driven numbers that have no event source.

`perf json` and `--perf-jsonl` do not read any UI section: they serialize
`GpuPassTimer`'s render/CPU timer snapshot directly, so throttling or hiding the
UI does not change those channels.

Hidden frames carry no stale presentation: the published `UiRenderSnapshot` has
empty vertices/indices/draw commands (except a frozen DrawDebug overlay) and no
scene-viewport rectangle. Camera projection synchronization, picking and the
editor pointer policy do not depend on the UI view model.

## Typed Boundaries

- `UiAction`: typed runtime-control mutation requests into `CommandBus`.
- `EditorCommandEnvelope`: asset-document mutation requests into `EditorCommandBus`.
- `UiViewModelSnapshot` and `MaterialInstanceEditorSnapshot`: GT read-only presentation state.
- `UiSnapshotSection<T>` / `UiViewModelSections`: versioned immutable view-model
  sections; `UiViewModelSnapshot` is the projection assembled from them for the
  panel API.
- `IEditorJsonAssetRequestSink`: read-only open/refresh intents to the GT facade.
  `MaterialInstanceEditorSnapshot.assetCatalog` and `.jsonDocument` retain
  `shared_ptr<const ...>` results without per-frame deep copies of assets/fields.
- `UiRenderSnapshot`: immutable vertices, indices, clip rectangles, blend modes,
  generation-tagged RGBA8 textures or renderer GPU-image tokens, and
  `sceneViewportRect` layout/picking metadata.
  `sceneViewportEnabled` selects offscreen presentation. A reserved
  `SceneViewportTextureId` references the current renderer-owned output
  logically; it carries no pixels, Vulkan handles or GPU ownership.

`EngineLoop::ApplyQueuedUiActions()` drains runtime requests at the stable
frame boundary. The MI runtime drains editor commands separately, validates
document revisions and performs renderer-owned preview preparation.
There is no second UI-to-renderer mutation route.

## Threading And Ownership

GT owns `UiSubsystem`, the RmlUi context, both documents, input arbitration,
reload and view updates. When the update scheduler says the visible UI changed
(or on a hidden frame), Update publishes one immutable snapshot through
`UiRenderSnapshotQueue`, which retains only the newest.
RT consumes that snapshot and otherwise keeps the last one it consumed. A
skipped frame therefore leaves the previous immutable snapshot in place on both
sides; the publisher also retains its identity. RT never traverses live documents
or asset sessions.
The JSON worker performs only bounded filesystem/parse work, never RmlUi,
World mutation or Vulkan access. GT alone publishes its completed snapshots;
shutdown joins the worker before releasing the library's snapshots.

The V1 loop waits for RT before the next engine-owned renderer mutation.
Shutdown clears the queue after stopping RT and waiting for renderer idle,
then destroys UI and renderer resources through their existing owners.

## Input

Priority is global shortcuts, editor workspace, then game.
Editor focus and modals consume only input events. Application lifecycle events
such as `PlatformEventType::Quit` continue to EngineLoop, so the native window
close button still exits while a panel has focus or a modal is open.
The editor uses `GameAndUi` with a visible
cursor; RmlUi hit testing, keyboard focus, drag capture and modal state claim
panel input. The Viewport image passes through to the game, and clicking it blurs
editor focus. A dirty-close modal claims the window. No visible UI restores
the player's requested relative mouse mode.

`InputSubsystem` drains SDL relative deltas each frame but publishes them only
when pointer ownership is Game and relative mouse mode is actually enabled.
Releasing capture cannot keep rotating the camera.

When relative mouse mode is actually enabled, EngineLoop coalesces consecutive
motion events from the same window and modifier state before UI dispatch.
It keeps the final absolute position and summed deltas; discrete events remain
ordered barriers. Free-cursor UI dragging is not coalesced. Camera motion still
uses SDL's per-frame accumulated relative state, so sensitivity is unchanged.
Scene picking only performs DOM hit testing for left-button-down events.
Light/camera helper icons claim editor input and never also trigger mesh picking.

Panel mouse-wheel and button-down events release stale scene-viewport input
ownership before dispatch to RmlUi, including after returning from immersive
mode. Mouse motion and button-up alone preserve capture so relative camera
movement and an in-progress scene gesture are not interrupted.

Restoring the editor also explicitly pulls its document to the front without
claiming keyboard focus. Hiding it transfers focus and stacking order to the
runtime document; `Show(FocusFlag::None)` alone leaves that transparent document
above the editor, blocking panel mouse input when returning from immersive mode.

**A scene-viewport hit splits by button.** The **left** button keeps the existing path:
it is the picking button (`materialInstanceSelection.cpp`), it blurs editor focus, and it
passes through to the game. The **right** button is consumed by the editor document
instead, so the workspace's context menu can open over the viewport. That is a deliberate
asymmetry with a reason: the right button has no runtime binding to break — camera look
comes from SDL relative deltas rather than a button gesture — while the left button does.
Everything else on a scene-viewport hit still returns before RmlUi, so a stray event
cannot leak into the editor DOM.

F10/gamepad Start do not open a UI page. F11 toggles immersive mode (F1 remains
a compatibility alias). Immersive mode hides the editor and HUD in the existing
window; it does not create a window, change OS fullscreen, or quit. From a model
tab it first restores the remembered level through LoadWorld and waits for the
matching committed World snapshot. Rejection or failure leaves the editor visible;
another toggle while waiting cancels entry. Returning restores the workspace layout.
The toolbar uses "Immersive Mode"; File / Exit remains Quit. Esc toggles
requested mouse capture.
Ctrl+S opens save confirmation for the active MI on a material page, or all
modified files on other pages. Save, dirty-close and About modals claim input.
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
Invalidation walks the replaced subtree (including internal select children),
with properties grouped by element id; it does not search the document once per
cached property on every text replacement.
Dock placement and panel visibility use the same property cache.

FPS is displayed in the native window title and on the Performance tab (from the frozen
performance snapshot). The editor status bar has no duplicate FPS node or refresh cadence;
frame-rate changes do not invalidate workspace layout. Ordinary variable-size labels still use
normal document layout.

UI asset fingerprints drive candidate reload. Localization, runtime document
and enabled editor document must all parse and pass required-control validation
before replacement; errors or warnings reject the candidate. Failure retains
the last valid documents. Runtime page, visibility, locale and runtime scroll
state live outside replaceable documents. MI state remains in its runtime,
not the RML document. Editor focus/scroll may reset on successful reload.

### 参数语义与编辑反馈

RmlUi range 输入在**程序化绑定更新**时也会发出 `change`（含取值舍入与钳制）。
编辑器的 `WidgetRefreshScope` 在绑定冻结快照期间抑制动作。
**只读快照绝不能被这类 UI 反馈改写。**

主方向光 Details 强度用 lux，作者 Camera Details 的 Exposure 分组承载
Manual / Auto、手动 EV100、补偿，以及自动 min/max EV100、适应速率（1/s）与目标灰度（linear）。
没有独立 Post Process 标签页或视口曝光覆盖。曝光值来自选中作者 Camera 的
**只读** actor 快照；每次编辑经 `UiAction` 与 `RuntimeCommand` 的 `SetActorProperty`
排队一个枚举字段、actor 稳定身份及 World generation。游戏线程对当前作者 Camera
的**完整候选**校验后才写回，旧代数事件被拒绝。绑定刷新永不提交编辑。
手动 EV100 只在 ev100 模式显示，自动模式显示自动参数。
作者写入的物理相机曝光归在 Manual 下并只读显示推导出的
EV100；点击 Manual 才显式选择直接输入 EV100，**没有单独的 Physical 模式按钮**。
加载或刷新 UI 不会转换物理相机场景数据。补偿在所有模式下都生效。
接受的修改形成所属 Scene 草稿；Save Content 确认后按字段补丁写入场景 JSON，
不覆盖未编辑的物理参数。未保存草稿在源摘要不变时可重放到替换 World。
独立观察相机只保留自己的位姿，显示曝光仍跟随作者 Camera。

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

### TressFX Hair Asset Viewport

Opening a `type: "tressfx"` `TF_*.json` opens the dedicated TressFX asset tab: the asset
has its own Tressfx kind and Content Browser category. The preview world is `SC_model_preview` with the hair
injected at scale 1, and no level context is required. The viewport never writes the
scene or the descriptor; rotation and reset are session state of the preview World.

TF assets contain geometry and `simulationDefaults`; character skinning and external
collision proxies belong to placements. The standalone asset preview therefore uses the
defaults without a character binding. The existing legacy skinning guard is retained:
an old descriptor that still declares `simulation.skinning` cannot be previewed by
silently stripping its binding.

Input: left-drag rotates the preview object (yaw from horizontal movement, pitch clamped
to ±89°); middle/right drag keeps camera orbit/pan and the wheel keeps zoom. Rotating the
object rather than the camera is deliberate: hair physics responds to root placement, not
to view direction. The reset button queues `ResetHairSimulation` with an empty target,
which resets every tressfx object in the preview world.

The UI only submits value-semantic `UiAction` (`RotatePreviewModel`,
`ResetHairSimulation`). The game thread converts them into `SetMeshPlacement` /
`ResetHairSimulation` runtime commands and validates that `preview_model` is still in the
active World. Rotation uses the raw World identity `preview_model`; the UI actor address
`mesh|preview_model` is not a placement identity. Rotation conversion checks the preview scene
and World generation, and command execution checks that generation again after queueing.

The asset page owns the `Simulation` defaults section: nine authored simulation values
with ranges/units from one shared table, initialised from `simulationDefaults` in the asset
draft. Live preview and Reset require enabled GPU simulation. Dragging an available live
control submits `SetHairSimulationParameter` for `preview_model`; it does not write the
asset or scene. `Save to Hair Asset` runs the standard `tressfx.modify`
(document revision) → Save Content → `tressfx.save`; the status text states that the hair asset
is affected for **every placement** and that reloading applies the values and resets the
simulation.

The hair property rows explicitly size the label and numeric columns. Numeric inputs and
sliders retain their DOM nodes during value/status refresh; values use `SetControlValue`
so focused partial text and an active slider drag survive snapshot updates. Hair slider
callbacks read the new `change` event value: RmlUi writes the control's value attribute only
after dispatching that event.

The TressFX tab reuses the existing hair preview, rotation/reset controls and simulation
control table. Its authoritative document projection and revision come from the separate
Tressfx document owner through `tressfxDocuments`; it does not open a Mesh draft.
The document service requires a `TF_` filename; the completed migration retains no `SM_` alias.

Blueprint component Details edits this attachment's `inherit|off|on` mode and sparse
simulation overrides. Removing an override restores inheritance. External collision author
data remains placement-owned; hair does not register a rigid collider.
Scene Details keeps tressfx component fields read-only and directs editing to the Blueprint
asset. The tressfx component is not part of `componentInstanceEditable`.

## Configuration

`config/config.json -> ui` retains `enabled`, `hotReload`, `developerUiEnabled`,
`developerUiVisible`, `assetRoot`, `document`, `localization`, `defaultLocale`
and `fontFaces`. The developer names now select the RmlUi editor, not a library.
`--dev-ui` enables it; `--no-dev-ui` skips editor initialization while retaining
the scene-name HUD. `ui.enabled=false` disables the complete UI subsystem.
There is no developer-UI CMake switch.

`workerThreadCount=1` renders synchronously; `2` consumes snapshots on RT.
The normal shader build path produces overlay SPIR-V from `shader/glsl/`.

## Verification

Root-registered GoogleTest target `editor_ui_tests` loads real RML/RCSS with
CPU fake sinks and covers parsing, refresh, focused input, edit batching,
revision preservation, save/dirty close, escaping and typed offline actions.
Its Vulkan-free `uiViewModelRefreshTests` cover the view-model visibility
decision and refresh schedule: first frame due, interval wait, forced event
refresh, re-armed windows, a custom 15 Hz cadence, and "hidden never builds the
full editor snapshot even when forced".
Its `uiUpdateSchedulerTests` cover the layer-3 gate: idle skip, section/input/DOM
mutation/world-change/tab-switch/locale/cadence forcing, hidden never updating,
surface switch and first-visible-frame forcing, the animated-overlay publish-only
case, and that a skipped publish retains the last immutable snapshot identity
while the queue stays empty.
Its Performance cases load the workspace document, verify the tab sits in the
Log/Renderer stack, map a frozen `PerformanceSnapshot` into summary/table values,
assert the CPU stage table and its derived `other` row, and assert 5 Hz throttling plus
unsupported/not-yet-sampled empty states.
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

2026-10-17 native-close regression: editor focus/modal capture previously consumed
`Quit` through the default event branch. Only keyboard, text and gamepad input
now enter that branch. Debug with Vulkan validation closed normally with the
Content Browser search focused; Development closed with About open (exit code 0).
Both logs recorded Quit and completed EngineLoop teardown, with no Vulkan errors.
The 450 editor tests reported 445 passes and 5 symlink skips; they do not compile
UiSubsystem, so these close checks are desktop acceptance, not automated coverage.

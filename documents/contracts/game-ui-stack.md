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

`ui/runtime-hud.rml` and `.rcss` define a non-interactive scene-name HUD.
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
- Bottom left/center: Content Browser (MI/Mesh/Scene/Texture/Terrain JSON sources
  tree, breadcrumbs, search, type filter, tiles/list, sorting, size and explicit
  refresh controls) / Renderer
  (offline controls) tabs; switching only changes presentation.
- Left/center: a scene Viewport image and game input.
- Bottom edge: status bar.
- Dirty-close modal: explicit Save/Discard/Cancel.

The workspace supports actor property editing with GT-owned scene drafts and
explicit conflict-aware scene saves; it has no undo system.
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

Same-frame edits coalesce into an ordered revision-gated atomic batch.
Validate includes pending edits. Save opens confirmation after flushing pending
edits and waits for an authoritative revision before enabling confirmation.
The shared Save Content list includes dirty Scene, Mesh, Texture and MI files, supports
individual checkbox selection, and never writes merely by opening.
Content Browser marks these types in place and offers a path-targeted Save action.
Mesh/Texture drafts enter through the [standard asset API](asset-modify-api.md).
Dirty close waits for a clean
authoritative snapshot; failed saves keep the document and dialog open.
Focused partial input is not overwritten by snapshots. Labels are RML-escaped.
Color is currently edited as Linear RGBA numbers, without an sRGB palette.
Texture edits can be saved; texture live-preview descriptor replacement remains
unavailable. Numeric/render-state preview retains the existing renderer adapter.
Content Browser selection is separate from the open document. A single click
selects an item; a double click on MI emits the existing editable-document open
command. Scene double-click reuses the existing `UiActionType::LoadWorld` transaction.
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
Rendered thumbnails are session-only; CPU import/shader preparation/texture decode
use a single asynchronous job, while GPU upload and pipeline creation remain on GT.
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
Viewport controls, RmlUi input processing and context updates remain per-frame,
so camera indicators, hover, scrolling and animations are not throttled.
Actor helper icons retain fixed layout boxes and update screen position through
draw transforms. Selected helper lines retain slots for their unclipped topology;
clipping changes transforms/visibility rather than replacing DOM. Camera motion
therefore does not invalidate workspace layout through helper markup updates.

This is binding/control invalidation, not dirty-rectangle rendering. GT still
builds immutable draw snapshots and RT uploads/composites the overlay every frame;
unchanged panel data does not yet imply reused GPU draw buffers.

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

FPS is displayed only in the native window title. The editor status bar has no
duplicate FPS node or refresh cadence; frame-rate changes do not invalidate
workspace layout. Ordinary variable-size labels still use normal document layout.

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

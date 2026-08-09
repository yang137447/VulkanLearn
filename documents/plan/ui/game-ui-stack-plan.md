# Game UI Stack Plan

## Status

Implemented for the Windows-first Vulkan runtime. The current implementation
contract lives in `documents/architecture/game-ui-stack.md`; this file remains
the decision record and adoption checklist that led to that implementation.

## Decision

Use two UI layers with separate responsibilities:

- RmlUi owns player-facing runtime UI.
- Dear ImGui owns developer-facing tools and temporary diagnostics.
- SDL3 remains the platform input and window boundary.
- VulkanLearn remains responsible for final GPU recording and presentation.

The two libraries should share engine integration points, but they should not
share widget models or application responsibilities.

## Why Two Layers

RmlUi is the default system for UI that must be styled, localized, hot-edited,
and maintained as game content. Expected uses include:

- HUD
- pause and settings menus
- inventory, task, and skill pages
- modal dialogs
- controller-navigable game screens

Dear ImGui is reserved for UI assembled directly from live C++ state. Expected
uses include:

- render graph and resource inspection
- material, lighting, and post-process controls
- performance graphs and profiler views
- temporary diagnostic panels
- rapidly changing engine tools

RmlUi can also build developer panels. Dear ImGui should only be introduced
when dynamic inspection, tables, trees, docking, or one-off tools make the
second system cheaper than continuing to build those tools as RML documents.

## Shared Runtime Boundary

The intended high-level flow is:

```text
SDL3 events
    -> UI input router
       -> RmlUi runtime context
       -> Dear ImGui developer context
       -> remaining game input

Game state
    -> immutable UI view-model snapshot
    -> RmlUi bindings

UI interaction
    -> typed UiAction
    -> CommandBus
    -> EngineLoop / gameplay system

RmlUi geometry + ImGui draw data
    -> immutable render-thread UI snapshot
    -> Vulkan UI overlay recording
    -> present
```

UI code must not directly mutate RenderGraph, MaterialInstance, Vulkan device,
or swapchain objects. Engine changes initiated by UI should cross a typed
command boundary. Read-only UI state should come from view-model snapshots
rather than raw mutable engine pointers.

## Vulkan Integration

UI should be composited after the final scene color and tone-mapping work. The
UI overlay must preserve the scene color and leave the swapchain image ready
for presentation.

The renderer-facing UI boundary should support both libraries without forcing
either library into the data-driven RenderGraph pass model. Shared concerns
include:

- swapchain format and extent
- per-swapchain-image framebuffer or dynamic-rendering state
- font and image texture uploads
- UI vertex and index buffer allocation
- clip rectangles and scissor state
- resize and shutdown ordering
- graphics command recording on the render thread

With `workerThreadCount == 2`, widget updates and interaction stay on the game
thread. The render thread consumes immutable UI draw data produced at a stable
frame boundary. The render thread must not traverse live RmlUi documents or an
ImGui context that the game thread may mutate.

## Input Policy

The UI input router should expose explicit modes:

- `GameOnly`
- `UiOnly`
- `GameAndUi`

Modal UI receives input first. Developer UI receives input only when enabled.
Game input receives events that were not consumed by an active UI layer.
Keyboard focus, pointer capture, controller focus, and text input must have one
explicit owner at a time.

The implemented runtime acceptance scope includes mouse, keyboard, clipboard,
gamepad focus navigation, UTF-8 input, IME composition, and focus-loss cleanup.
Touch, accessibility, and platform safe-area behavior remain follow-up work.

## Hot Editing

RML and RCSS assets should support development-time hot reload. Reload must use
a candidate-and-commit flow:

1. Detect a changed document, stylesheet, or referenced UI asset.
2. Parse and validate a candidate UI document.
3. Keep the last valid document active when validation fails.
4. Swap the candidate at a stable game-thread frame boundary.
5. Restore view-model state through stable element or binding identifiers.

Persistent state such as current page, selected item, scroll position, and
settings values should live outside the replaceable document tree. Focus and
transient pointer state may be reset when preserving them would be ambiguous.

Dear ImGui panels are code-defined and are not part of RML/RCSS hot reload.
They use the normal C++ build and runtime iteration path.

## Performance And Power Rules

The UI choice does not replace frame pacing. VSync, frame caps, menu refresh
policy, and idle behavior have a larger power impact than the widget library
alone.

Both UI layers should follow these rules:

- batch by compatible texture and clip state
- reuse font atlases and static textures
- avoid per-widget Vulkan resource creation
- avoid redundant CPU-to-GPU uploads
- limit transparent full-screen overdraw
- stop or reduce nonessential animation when a page is inactive
- allow menu-only or idle states to use a lower frame rate

Developer UI must be compile-time or launch-time disableable so performance
and power measurements can exclude it.

## Cross-Platform Direction

SDL3 owns portable window and input behavior. UI rendering should remain behind
a renderer interface so future non-Vulkan platforms do not affect RmlUi
documents or engine UI actions.

Expected backend directions are:

- Vulkan on Windows, Linux, and Android
- Metal or an approved Vulkan translation layer on Apple platforms
- platform-specific rendering backends for consoles when required

Platform availability must not leak into UI document authoring. Unsupported
input, text, safe-area, and accessibility behavior should be represented as
explicit platform capability differences.

## Adoption Order

1. Define `UiAction`, view-model snapshots, input ownership, and the render-thread UI snapshot boundary.
2. Integrate RmlUi for one hot-reloadable runtime control page.
3. Add resize, shutdown, error rollback, and worker-thread validation.
4. Add gamepad navigation, localization, font fallback, and game-screen composition.
5. Introduce Dear ImGui only when developer-tool requirements justify the second widget system.
6. Keep both libraries on the same input arbitration and Vulkan overlay infrastructure.

## Non-Goals

- Copying Unreal Slate source or reproducing its full feature set
- Embedding Flutter, Qt Quick, or a browser runtime into the current SDL3 window
- Allowing UI scripts to call renderer internals directly
- Treating RML, RCSS, font atlases, or compiled UI geometry as engine source code
- Replacing the game renderer or the existing SDL3 platform boundary

## Revisit Triggers

Re-evaluate this decision if any of the following becomes a hard requirement:

- commercial console support needs a vendor-certified UI runtime
- visual authoring requires a designer-first XAML or HTML toolchain
- accessibility or complex international text requirements exceed the selected stack
- UI becomes the primary application shell rather than an in-engine overlay
- maintaining two UI libraries costs more than the developer-tool productivity gained

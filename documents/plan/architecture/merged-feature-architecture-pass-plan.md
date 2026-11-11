# Merged-Feature Architecture Pass Plan

## Status

- Type: architecture implementation plan (not a contract)
- Created: 2026-11-11
- Base: `caebbd1` (master after tressfx5/valley/terrain/niagara merge)
- Assets base: `7c792fe`
- Worktree: `architecture-pass-2`, `codex/architecture-pass-2`
- State: A1, A2a, A2b, A3a, A3b, A4, A5, A6, A6b and A7 completed.
- Metrics: non-empty lines measured from the current worktree on 2026-11-12;
  blank lines are excluded. A0 baseline numbers are historical, not current.
- Runtime verification: `main_editor` / `main_game` build and the particle/fracture
  smoke scenes pass. Test targets were still building when this record was updated.
- Review: independent review initially returned **reject as written**, direction
  approved; blockers were folded into the phases and are closed by the final split
  records below.

## Baseline

The runtime/editor split and prebuilt dependency work survived the feature merge:
`vl_runtime` has zero `source/editor/**` translation units, `main_game` links no
editor target, and `VULKANLEARN_DEPS_MODE=prebuilt` is offline-capable.

A1 removed the duplicate `source/core/editorBuild.h` macro boundary. The merged
feature code had reintroduced monolithic files; the final split records below list
the current files and non-empty line counts. The original oversized-file baseline is
kept only as history and must not be used as a current-state table.

## Goals

- Keep CMake target ownership as the only runtime/editor boundary (A1 done).
- Bring the merged-feature files under the 1,500-line responsibility target by
  mechanical extraction; preserve behavior, strings, JSON and event ordering.
- Keep prebuilt deps, ccache, `clean-project` and the target graph working.

## Non-Goals

- No feature redesign, no particle/animation behavior changes.
- No new asset format, renderer feature or packaging change.
- No physical directory move; ownership remains CMake-based.

## Phases

| Phase | Scope | Status |
|---|---|---|
| A0 | Baseline tags, reproducible metrics, full test-target build and failure record | completed |
| A1 | Remove `core/editorBuild.h`, includes/static_asserts and `VL_WITH_EDITOR` definitions | completed `33f4948` |
| A2a | Mechanical split of `particleRenderer.cpp`; move helpers to their owning TUs | completed |
| A2b | Decompose `RecordSimulation` and split uniform builders into a second TU | completed |
| A3a | Mechanical split of `particleAsset.cpp`; promote shared anonymous helpers | completed |
| A3b | Decompose `ValidateEmitter` and `ParseEmitter` | completed |
| A4 | Split `skeletalPlayback.cpp`; one owner for `nextPlaybackIdentity` | completed |
| A5 | Fix/re-link `editor_ui_tests`, add string snapshots, split `particlePanelController.cpp` | completed |
| A6 | Split render graph, renderer backend, UI, shader compiler, editor command/panel, world particle, editor runtime | completed |
| A6b | Isolate `ExecuteCommand` and `ProcessCommand` bodies in dedicated TUs | completed |
| A7 | Docs, routing, boundary rules, diagram JSON and final gates | completed |

## A1: editor macro boundary

A1 introduced no new translation unit. `source/core/editorBuild.h` was deleted and
the `VL_WITH_EDITOR` / `VL_EDITOR_ONLY` definitions were removed from
`source/CMakeLists.txt`, the four editor particle headers and the tool CMake files.
`documents/contracts/editor-runtime-data-boundary.md` now describes the CMake
target-ownership model instead of the removed macro. Non-empty line counts do not
apply to deleted or build-only files.

## A2a/A2b: particle renderer

`particleRenderer.cpp` was reduced to a small facade. Shared helpers that are used by
more than one TU live in `particleRendererInternal.h`; single-TU helpers live in
their owning TU. `RecordSimulation` is 629 non-empty lines. The three uniform
builders were moved to `particleRendererSimulationUniforms.cpp`; both simulation TUs
are below 1,500 non-empty lines.

| File | Non-empty lines | Role |
|---|---:|---|
| `source/particles/particleRenderer.cpp` | 160 | facade: destructor, asset resolution, one-time warnings |
| `source/particles/particleRenderer.h` | 1,389 | private helper/state declarations |
| `source/particles/particleRendererInternal.h` | 310 | cross-TU helpers and shared constants |
| `source/particles/particleRendererInitialize.cpp` | 694 | initialization, pipelines, shutdown |
| `source/particles/particleRendererLighting.cpp` | 669 | light buffers, IBL and translucency volume |
| `source/particles/particleRendererRuntime.cpp` | 807 | scene sync, events, mesh placement |
| `source/particles/particleRendererGlobalSort.cpp` | 1,181 | global gather/sort/draw |
| `source/particles/particleRendererEmitterAssets.cpp` | 1,113 | emitter/mesh/material loading |
| `source/particles/particleRendererBuffers.cpp` | 819 | emitter buffers and dynamic content |
| `source/particles/particleRendererDescriptors.cpp` | 1,044 | base, decal and light descriptors |
| `source/particles/particleRendererSimulation.cpp` | 953 | frame loop and non-uniform simulation stages |
| `source/particles/particleRendererSimulationUniforms.cpp` | 1,022 | primary/rendering/auxiliary uniform builders |
| `source/particles/particleRendererSoftDepth.cpp` | 198 | soft-depth resources and recording |
| `source/particles/particleRendererGraphics.cpp` | 399 | graphics recording |
| `source/particles/particleRendererShadow.cpp` | 501 | shadow pipeline, prepare and recording |
| `source/particles/particleRendererDebug.cpp` | 367 | particle dump |

## A3a/A3b: particle assets

`particleAsset.cpp` is replaced by an internal header and six TUs. A3b further split
the two parser/validator bodies into module TUs. The previous `ValidateEmitter` and
`ParseEmitter` exceptions are closed; both main TUs and module TUs are below 1,500
non-empty lines.

| File | Non-empty lines | Role |
|---|---:|---|
| `source/particles/particleAssetInternal.h` | 95 | shared declarations |
| `source/particles/particleAssetJsonHelpers.cpp` | 367 | JSON/vector/scalar helpers |
| `source/particles/particleAssetCollisionAssets.cpp` | 143 | SDF and collision mesh loading |
| `source/particles/particleAssetParseEmitter.cpp` | 1,008 | parser entry points and shared parse state |
| `source/particles/particleAssetValidateEmitter.cpp` | 994 | validator entry points and shared validation state |
| `source/particles/particleAssetModuleSchema.cpp` | 170 | v2 module schema and user parameters |
| `source/particles/particleAssetLoader.cpp` | 237 | `ParticleAssetLoader` facade |
| `source/particles/particleAssetEmitterInternal.h` | 59 | parser/validator cross-TU declarations |
| `source/particles/particleAssetParseEmitterModules.cpp` | 729 | emitter module parsing |
| `source/particles/particleAssetValidateEmitterModules.cpp` | 696 | emitter module validation |

## A4: skeletal playback

`skeletalPlayback.cpp` keeps ownership of the playback facade and the single
`nextPlaybackIdentity`; sampling/timeline, notify, sync groups, movement and
pose/skinning live in dedicated TUs.

| File | Non-empty lines | Role |
|---|---:|---|
| `source/animation/skeletalPlayback.cpp` | 250 | facade, identity and tick entry points |
| `source/animation/skeletalPlaybackInternal.h` | 114 | cross-TU declarations |
| `source/animation/skeletalPlaybackMovement.cpp` | 630 | montage sections, movement, warp and foot placement |
| `source/animation/skeletalPlaybackNotify.cpp` | 212 | notify state and dispatch |
| `source/animation/skeletalPlaybackSkinning.cpp` | 175 | pose/skinning helpers |
| `source/animation/skeletalPlaybackSync.cpp` | 187 | sync groups |
| `source/animation/skeletalPlaybackTimeline.cpp` | 582 | sampling, montage timeline and blending |

## A5: particle panel

The particle panel facade keeps document ownership and cross-section coordination;
curve, emitter, flipbook, markup and refresh paths are separate TUs. `editor_ui_tests`
was re-linked, and action/data string snapshots are checked by the test targets.

| File | Non-empty lines | Role |
|---|---:|---|
| `source/editor/ui/panel/particlePanelController.cpp` | 247 | panel facade and shared state |
| `source/editor/ui/panel/particlePanelCurveController.cpp` | 320 | curve editing |
| `source/editor/ui/panel/particlePanelEmitterController.cpp` | 485 | emitter editing |
| `source/editor/ui/panel/particlePanelFlipbookController.cpp` | 460 | flipbook editing |
| `source/editor/ui/panel/particlePanelMarkup.cpp` | 320 | Rml markup builders |
| `source/editor/ui/panel/particlePanelRefresh.cpp` | 228 | panel refresh/apply paths |
| `tool/editor-ui-tests/particlePanelStringSnapshot.h` | 284 | action/data literal snapshot |
| `tool/editor-ui-tests/particlePanelStringSnapshotData.h` | 683 | generated literal payload |
| `tool/editor-ui-tests/particlePanelStringsTests.cpp` | 106 | snapshot regression |

## A6: render graph, backend, UI, shader and editor

The A6 slices are mechanical member-function or free-function boundaries. Moved
blocks are character-identical to their originals except for per-TU includes,
namespace scaffolding, internal-header declarations and the documented
default-argument moves. No call order, log string, error string, GPU write order or
event order changed.

| File | Non-empty lines | Role |
|---|---:|---|
| `source/renderGraph.cpp` | 986 | graph resources, pass state, load/validate/render initialize |
| `source/renderGraphPassResources.cpp` | 853 | resource and render-pass creation |
| `source/renderGraphFrameBuffers.cpp` | 389 | Vulkan render pass/framebuffer creation |
| `source/renderGraphFormats.cpp` | 255 | format, usage, load-op and clear-value conversion |
| `source/render/rendergraph/renderGraphCompiler.cpp` | 793 | compiler entry point |
| `source/render/rendergraph/renderGraphCompilerValidation.cpp` | 714 | compile-time validation |
| `source/render/rendergraph/renderGraphCompilerInternal.h` | 78 | compiler cross-TU declarations |
| `source/render/backend/rendererBackendVulkan.cpp` | 580 | backend lifecycle, submit, swapchain, capture |
| `source/render/backend/rendererBackendVulkanPipelines.cpp` | 293 | pipeline and pass/framebuffer lifecycle |
| `source/render/backend/rendererBackendVulkanResources.cpp` | 912 | buffer/image/view/sampler resources |
| `source/render/backend/rendererBackendVulkanDescriptors.cpp` | 354 | descriptor layouts, pools and sets |
| `source/render/backend/rendererBackendVulkanInternal.h` | 39 | backend cross-TU helpers |
| `source/hair/hairSimulationPass.cpp` | 654 | hair pass lifecycle and `Record` |
| `source/hair/hairSimulationPassDescriptors.cpp` | 433 | descriptor caches and skinning/pass recording |
| `source/hair/hairSimulationPassTrace.cpp` | 341 | trace copy/flush/finish paths |
| `source/hair/hairSimulationPassInternal.h` | 326 | hair pass declarations and frame plan |
| `source/ui/uiSubsystem.cpp` | 737 | runtime UI facade and lifecycle |
| `source/ui/uiSubsystemDocuments.cpp` | 581 | RmlUi documents, localization and snapshots |
| `source/ui/uiSubsystemInput.cpp` | 447 | input arbitration and Rml input |
| `source/ui/uiSubsystemActions.cpp` | 276 | UI actions and editor shell attach |
| `source/ui/uiSubsystemInternal.h` | 31 | UI cross-TU helpers |
| `source/shaderCompiler.cpp` | 799 | build/dependency helpers and compile policy |
| `source/shaderCompilerApi.cpp` | 523 | initialize/compile entry points |
| `source/shaderCompilerCommit.cpp` | 347 | artifact commit and manifest |
| `source/shaderCompilerCache.cpp` | 273 | cache load and identity normalization |
| `source/shaderCompilerInternal.h` | 54 | shader compiler cross-TU declarations |
| `source/editor/command/editorCommandJsonCodec.cpp` | 330 | codec facade |
| `source/editor/command/editorCommandJsonCodecInternal.h` | 173 | codec cross-TU declarations |
| `source/editor/command/editorCommandJsonCodecPayload.cpp` | 754 | payload encode/decode |
| `source/editor/command/editorCommandJsonCodecPrimitives.cpp` | 570 | primitive/vector fields |
| `source/editor/command/editorCommandJsonCodecValidation.cpp` | 215 | codec validation |
| `source/editor/ui/materialInstanceAssetEditorPanel.cpp` | 416 | panel facade |
| `source/editor/ui/materialInstanceAssetEditorPanelActor.cpp` | 291 | actor/outliner/details panel paths |
| `source/editor/ui/materialInstanceAssetEditorPanelBuild.cpp` | 408 | markup and layout builders |
| `source/editor/ui/materialInstanceAssetEditorPanelEvents.cpp` | 369 | panel event routing |
| `source/editor/ui/materialInstanceAssetEditorPanelResource.cpp` | 567 | resource/material editing paths |
| `source/world/world.cpp` | 813 | world facade and scene-object operations |
| `source/world/worldParticleInternal.h` | 28 | world particle cross-TU declarations |
| `source/world/worldParticleRuntime.cpp` | 528 | particle runtime bridge |
| `source/world/worldParticleSystem.cpp` | 291 | particle system scene operations |
| `source/editor/runtime/materialInstanceEditorRuntime.cpp` | 649 | editor runtime facade and command dispatch |
| `source/editor/runtime/materialInstanceEditorRuntimeInternal.h` | 123 | editor runtime cross-TU declarations |
| `source/editor/runtime/materialInstanceEditorRuntimePreview.cpp` | 508 | preview command path |
| `source/editor/runtime/materialInstanceEditorRuntimeState.cpp` | 438 | document/state command path |

## A6b: command exception TUs

| File | Non-empty lines | Role |
|---|---:|---|
| `source/engine/runtimeCommandExecutor.cpp` | 529 | executor facade, queue drain, mesh placement/sweep |
| `source/engine/runtimeCommandExecutorInternal.h` | 170 | executor cross-TU declarations |
| `source/engine/runtimeCommandExecutorExecuteCommand.cpp` | 1,592 | `ExecuteCommand` body |
| `source/debugConsole.cpp` | 222 | console lifecycle and line input |
| `source/debugConsoleProcessCommand.cpp` | 1,751 | `ProcessCommand` body |

## Function Exceptions

Only two functions purposely remain above 1,500 non-empty lines. Each is isolated in
its own TU; no other `source/**/*.cpp` TU exceeds 1,500 non-empty lines.

| Function | TU | Function non-empty lines | TU non-empty lines |
|---|---|---:|---:|
| `RuntimeCommandExecutor::ExecuteCommand` | `source/engine/runtimeCommandExecutorExecuteCommand.cpp` | 1,555 | 1,592 |
| `DebugConsole::ProcessCommand` | `source/debugConsoleProcessCommand.cpp` | 1,695 | 1,751 |

## A7: documentation closure

- Rewrote this plan's status and final split records from worktree measurements.
- Added glob-based routing coverage for every split family.
- Replaced stale source-file anchors in `documents/contracts/**` and
  `documents/rationale/**` with the new TU and current line numbers.
- Updated diagram IR anchors in `documents/contracts/diagrams/editor-frame-sequence.json`;
  generated HTML is not edited by hand.
- Regenerated `documents/contracts/boundary-rules.generated.md` from the audit script.

- Gate results: `validate-docs.py` passed; `architecture-boundary-audit.ps1`
  passed; rule index check passed with 91 rules; `git diff --check` is clean.

## Acceptance

- No `VL_WITH_EDITOR` / `VL_EDITOR_ONLY` / `core/editorBuild` references under `source`.
- No runtime source outside `source/editor/**` includes `editor/`.
- Final TUs are <= 1,500 non-empty lines except the two documented function TUs above.
- `main_editor`, `main_game`, prebuilt configure and feature smokes pass.
- `validate-docs.py`, boundary audit, rule index and `git diff --check` pass.

Commands use:

```powershell
cmake -S . -B build-merge -G "MinGW Makefiles" `
  -DVULKANLEARN_DEPS_MODE=prebuilt `
  -DVULKANLEARN_DEPS_ROOT=D:/YYBWorkSpace/VulkanLearn-deps `
  -DVULKANLEARN_ENABLE_EDITOR=ON -DVULKANLEARN_BUILD_GAME=ON `
  -DVULKANLEARN_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release `
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON `
  -DVULKANLEARN_ENABLE_CSHARP_BLUEPRINTS=OFF
tool\clean-project.ps1 -BuildDir build-merge -Apply
```

Feature smoke scenes: `SC_particle_showcase`, `SC_particle_ue_explosion`,
`SC_particle_ue_trails`, `SC_gasp`, `SC_fracture_playground`. C#-dependent scenes are
limited by `VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS=OFF`.

## Risks

- The particle renderer header still carries many private nested GPU types; moving
  more definitions into TUs will not reduce header fanout by itself.
- The two command TUs are intentionally above the TU target. Their function bodies
  remain isolated and must not be split by changing behavior without a separate
  invariant/golden plan.
- The render graph, backend and hair splits preserve declaration order and call
  order manually; future edits must keep the internal-header ownership rules.
- Panel extraction must keep Rml ids/action names/data attributes byte-identical;
  the A5 string snapshot tests are the guard.

## Review Record

Independent review initially rejected the plan as written: four single functions
exceeded 1,500 lines, baseline measurement was incomplete, `editor_ui_tests` was
missing a particle panel TU, explicit test CMake lists needed updates, and A1 needed
its boundary contract updated in the same phase. Those blockers were fixed in the
phase records above; the only remaining function-level exceptions are the two
command functions listed in `Function Exceptions`.

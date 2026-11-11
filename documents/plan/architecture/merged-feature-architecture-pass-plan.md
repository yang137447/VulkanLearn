# Merged-Feature Architecture Pass Plan

## Status

- Type: architecture implementation plan (not a contract)
- Created: 2026-11-11
- Base: `caebbd1` (master after tressfx5/valley/terrain/niagara merge)
- Assets base: `7c792fe`
- Worktree: `architecture-pass-2`, `codex/architecture-pass-2`
- State: A1 implemented in `33f4948`; A2-A6 pending
- Review: independent review returned **reject as written**, direction approved; blockers
  are folded into the revised phases below

## Baseline

The runtime/editor split and prebuilt dependency work survived the feature merge:
`vl_runtime` has zero `source/editor/**` translation units, `main_game` links no editor
target, and `VULKANLEARN_DEPS_MODE=prebuilt` is offline-capable.

Two regressions remain. First, the deleted `source/core/editorBuild.h` macro boundary is
gone in A1 (`33f4948`), so that item is closed. Second, merged feature code reintroduced
monolithic files. The table uses **non-empty lines**; the metrics script reports total LF
lines, not this breakdown.

| File | Non-empty lines |
|---|---:|
| `source/particles/particleRenderer.cpp` | 10,089 |
| `source/particles/particleAsset.cpp` | 4,161 |
| `source/renderGraph.cpp` | 2,409 |
| `source/engine/runtimeCommandExecutor.cpp` | 2,238 |
| `source/render/backend/rendererBackendVulkan.cpp` | 2,095 |
| `source/animation/skeletalPlayback.cpp` | 2,093 |
| `source/ui/uiSubsystem.cpp` | 1,957 |
| `source/debugConsole.cpp` | 1,956 |
| `source/editor/ui/panel/particlePanelController.cpp` | 1,930 |
| `source/shaderCompiler.cpp` | 1,877 |
| `source/editor/command/editorCommandJsonCodec.cpp` | 1,867 |
| `source/editor/ui/materialInstanceAssetEditorPanel.cpp` | 1,839 |
| `source/editor/runtime/materialInstanceEditorRuntime.cpp` | 1,678 |
| `source/hair/hairSimulationPass.cpp` | 1,701 |
| `source/world/world.cpp` | 1,588 |
| `source/render/rendergraph/renderGraphCompiler.cpp` | 1,502 |

Four functions individually exceed the target: `RecordSimulation` (1,919),
`ValidateEmitter` (1,609), `ExecuteCommand` (1,565), `ProcessCommand` (1,775). They cannot
be fixed by moving the whole function; decomposing their bodies is a separate phase.

## Goals

- Keep CMake target ownership as the only runtime/editor boundary (A1 done).
- Bring the merged-feature files under the 1,500-line responsibility target by mechanical
  extraction; preserve behavior, strings, JSON and event ordering.
- A single function larger than 1,500 lines may occupy its own file only as a documented
  exception; its body decomposition is tracked separately with explicit invariants.
- Keep prebuilt deps, ccache, `clean-project` and the target graph working.

## Non-Goals

- No feature redesign, no particle/animation behavior changes.
- No new asset format, renderer feature or packaging change.
- No physical directory move; ownership remains CMake-based.

## Phases

| Phase | Scope | Exit criteria |
|---|---|---|
| A0 | Baseline tags, reproducible metrics, full test-target build and failure record | baseline recorded; known link failures documented |
| A1 | Remove `core/editorBuild.h`, includes/static_asserts and `VL_WITH_EDITOR` definitions | done `33f4948`; editor/game build and smoke |
| A2a | Mechanical split of `particleRenderer.cpp`: init/pipelines, emitter load, buffers, descriptors, shadow/distortion/soft-depth, global sort | new files <= 1,500; particle smoke |
| A2b | Decompose `RecordSimulation` | function <= 1,500 or documented exception; particle smoke |
| A3a | Mechanical split of `particleAsset.cpp`; promote anonymous helpers to an internal header; update all 5 explicit test source lists | new files <= 1,500; asset tests |
| A3b | Decompose `ValidateEmitter` | function <= 1,500 or documented exception |
| A4 | Split `skeletalPlayback.cpp` by sampling/montage timeline, notify, sync group, movement/foot/slope/postprocess, pose/skinning; one owner for `nextPlaybackIdentity` | new files <= 1,500; animation tests |
| A5 | Fix/re-link `editor_ui_tests` (missing particle panel TU), add action/data string snapshots, then split `particlePanelController.cpp` | tests actually build and pass |
| A6 | Per-file slices: `renderGraph`, `rendererBackendVulkan`, `uiSubsystem`, `shaderCompiler`, `editorCommandJsonCodec`, `materialInstanceAssetEditorPanel`, `world`, `renderGraphCompiler`, `hairSimulationPass`; defer `runtimeCommandExecutor`/`debugConsole` body decomposition to A6b | per-slice build/tests/smoke |
| A7 | Docs, AGENTS/routing, boundary rules, diagram JSON, final gates | `validate-docs`, boundary audit, rule index; no `archify` dependency |

## Acceptance

- No `VL_WITH_EDITOR` / `VL_EDITOR_ONLY` / `core/editorBuild` references under `source`.
- No runtime source outside `source/editor/**` includes `editor/`.
- New modules <= 1,500 lines; a function-only exception must be recorded here.
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

- Particle extraction must preserve `RecordSimulation`, `CreateBuffers`,
  `CreateDescriptors`, `CreateDecalEmitterDescriptors`,
  `CreateLightEmitterDescriptors`, `DestroyEmitterResources`, `RetireEmitterResources`,
  per-image `frameUniforms`, and global sort gather→sort→draw ordering.
- `particleRenderer.h` is already ~1,346 lines and holds private nested GPU resource types;
  member-function definitions moving to new TUs keeps access but increases header fanout.
  Pure line splitting is not automatically a compile-time win.
- Shared anonymous-namespace helpers in `particleAsset.cpp` and `particleRenderer.cpp`
  must move to internal headers; `skeletalPlayback.cpp`'s `nextPlaybackIdentity` must have
  one owner.
- Panel extraction must keep Rml ids/action names/data attributes byte-identical; snapshot
  the literals before and after.
- Removing `VL_WITH_EDITOR` is safe only because no runtime TU includes the editor
  particle edit headers today; re-check after every split.
- Multiple splits touch explicit CMake source lists and the `-Wa,-mbig-obj` list; those
  edits must land with their phase, not later.

## Review Record

Independent review verdict: **reject as written**, direction approved. Blocking findings
were: four single functions exceed 1,500 lines; baseline measurement was incomplete and
omitted four files; `editor_ui_tests` is missing `particlePanelController.cpp` and cannot
be trusted as a gate; explicit test CMake lists must be updated per split; A1 needed its
boundary contract updated in the same phase. A1 is now committed (`33f4948`) with building
editor/game executables and smoke evidence.

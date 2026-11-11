# Merged-Feature Architecture Pass Plan

## Status

- Type: architecture implementation plan (not a contract)
- Created: 2026-11-11
- Base: `caebbd1` (master after tressfx5/valley/terrain/niagara merge)
- Assets base: `7c792fe`
- Worktree: `architecture-pass-2`, branches `codex/architecture-pass-2` / `codex/architecture-pass-2-assets`
- State: planned

## Baseline

After merging the four feature branches, the runtime/editor split and prebuilt dependency
work are intact: `vl_runtime` has zero `source/editor/**` translation units, `main_game`
links no editor target, and `VULKANLEARN_DEPS_MODE=prebuilt` is offline-capable.

The merged feature code reintroduced two regressions against the architecture targets:

1. A parallel macro boundary `source/core/editorBuild.h` (`VL_WITH_EDITOR` /
   `VL_EDITOR_ONLY`) duplicates the CMake target ownership rule. It is used only by
   `source/editor/particles/*` headers and no runtime source includes it.
2. New monolithic files exceed the 1,500-line responsibility target:

| File | Lines |
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

## Goals

- Remove the duplicate macro boundary and keep CMake target ownership as the only rule.
- Bring the merged-feature monolithic files back under the responsibility limit by
  mechanical extraction; preserve behavior, JSON/markup strings and events.
- Keep the prebuilt dependency workflow and `clean-project`/ccache behavior working.
- Re-run the architecture/build gates and feature smoke scenes after each phase.

## Non-Goals

- No feature redesign, no particle/animation behavior changes.
- No new asset format, renderer feature or packaging change.
- No physical directory move; ownership remains CMake-based.

## Phases

| Phase | Scope | Exit criteria |
|---|---|---|
| A0 | Baseline tags, metrics, `validate-docs`, boundary audit | baselines recorded; gates green |
| A1 | Delete `core/editorBuild.h`, its includes/static_asserts and `VL_WITH_EDITOR` compile definitions | no `VL_WITH_EDITOR`/`VL_EDITOR_ONLY`; editor target builds |
| A2 | Split `particleRenderer.cpp` into simulation, sorting, rendering, emitter and debug modules | each new file <= 1,500; particle scenes smoke |
| A3 | Split `particleAsset.cpp` into schema/parse/validate/serialize modules | each file <= 1,500; asset tests |
| A4 | Split `skeletalPlayback.cpp` (graph/state/sync/pose) | each file <= 1,500; animation tests |
| A5 | Split `particlePanelController.cpp` and panel action routing | each file <= 1,500; editor UI tests |
| A6 | Split remaining >1,500 runtime/editor files in small PR-sized slices (`renderGraph`, `runtimeCommandExecutor`, `rendererBackendVulkan`, `uiSubsystem`, `debugConsole`, `shaderCompiler`, `editorCommandJsonCodec`, `hairSimulationPass`) | per-slice tests/smoke |
| A7 | Docs, AGENTS/routing, boundary rules, archify, final gates | `validate-docs`, boundary audit, rule index, clean build |

## Acceptance

- `rg 'VL_WITH_EDITOR|VL_EDITOR_ONLY' source` empty.
- No runtime source outside `source/editor/**` includes `editor/`.
- New modules <= 1,500 lines; facades may be slightly larger only if documented.
- `main_editor`, `main_game`, prebuilt configure and feature smokes pass.
- `validate-docs.py`, boundary audit, rule index and `git diff --check` pass.

## Risks

- Particle renderer is a 10k-line file; extraction must keep pass ordering, GPU
  descriptor binding and event polling identical.
- Animation playback extraction must keep graph evaluation and sync-group behavior
  identical; animation tests are the guard.
- Panel extraction must keep Rml ids/action names/data attributes byte-identical.
- Removing `VL_WITH_EDITOR` is safe only because no runtime TU includes the editor
  particle edit headers today; re-check after every split.

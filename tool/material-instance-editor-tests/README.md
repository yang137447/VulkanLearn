# Material Instance Editor Focused Tests

This directory contains the focused fixtures and executable checks for the
Material Instance Asset Editor baseline, numeric live-preview boundary, and
render-state preview value boundary. It covers command transport,
persistence, preview-value/controller contracts, and production
document/runtime behavior without claiming Vulkan renderer integration from
the focused targets.

The harness deliberately uses three layers:

- `material_instance_editor_landed_tests` directly exercises the landed
  `EditorCommand`/JSON codec/`EditorCommandBus`, persistence, and preview value
  and protocol-boundary APIs.
- `material_instance_editor_tests` exercises fixture-driven wire/schema,
  navigation, document-session, sparse-save, conflict, texture, and
  preview-result contracts, including the real material validator and schema
  implementations.
- `material_instance_editor_production_tests` exercises the production
  document service and runtime facade. It injects a CPU-only fake adapter
  through `MaterialInstanceEditorRuntime::Config::previewAdapter` to verify
  numeric connect/apply/restore, the `renderMode`/`shadingModel`/`cullMode`
  preview draft boundary, baseline restore, and World-generation
  invalidation.

All focused targets are intentionally free of Vulkan handles and do not link
`RenderSystem`. The fake adapter verifies the value-semantic injection seam,
not renderer ownership, descriptor replacement, or GPU synchronization. The
actual renderer-owned numeric and render-state paths are implemented in the
main application; these focused targets remain CPU-only and therefore do not
prove Vulkan pipeline rebuilding, descriptor replacement, or GPU synchronization.
Manual interactive ImGui acceptance remains outside this focused test target.

## Build

The root build registers all three tests when `VULKANLEARN_BUILD_TESTS=ON`:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --target material_instance_editor_tests material_instance_editor_landed_tests material_instance_editor_production_tests -j
ctest --test-dir build -R "^material_instance_editor_(contract|landed|production)$" --output-on-failure
```

The test directory can also be configured independently from the repository root:

```powershell
cmake -S tool/material-instance-editor-tests -B build/material-instance-editor-tests -G "MinGW Makefiles"
cmake --build build/material-instance-editor-tests -j
ctest --test-dir build/material-instance-editor-tests --output-on-failure
```

The standalone build defines three executables and three CTest entries. The
`material_instance_editor_tests` target is the fixture contract harness; the
`material_instance_editor_landed_tests` target links the currently landed
typed command/codec/bus, persistence, preview value, and protocol-boundary
implementations directly. The production target links the runtime facade and
uses only the injectable fake adapter; none of the three targets instantiates
the real Vulkan renderer adapter.

## Coverage

- versioned command envelope/result schema and producer parity
- complete typed `float`/`vec2`/`vec3`/`vec4` values and stable error codes
- scene -> mesh/terrain -> section/slot -> MI navigation
- same-path document singleton, multiple origins, revision rejection, revert,
  dirty-close policy, schema/source change, and World replacement
- sparse parameter/texture candidate generation with exact numeric comparison,
  empty-object removal, and unmanaged-field retention
- `T_*.json` references, source existence, raw-image rejection, and traversal
  rejection
- BLAKE3 source conflict detection and atomic candidate replacement
- asynchronous preview request/result polling and terminal-result stability
- baseline/working serialized draft equality, dirty-draft preservation, revert,
  and post-save baseline advancement
- injected runtime numeric preview connect/apply/restore and World-generation
  cancellation, plus preview protocol payload/revision/generation boundaries
- runtime render-state preview boundary for `renderMode`, `shadingModel`, and
  `cullMode`: complete draft serialization, prepare failure reporting, dirty
  draft retention, and baseline restore without document mutation
- direct landed-interface checks for command JSON round-trips, bus de-duplication
  and revision gating, sparse candidates, persistence conflicts, and preview
  value/status semantics

The production document service validates a managed MI projection while
preserving opaque source fields during sparse save. The fixture
`MI_editor_test_opaque_extension.json` remains a regression case for that
forward-compatible preservation rule. The focused runtime test proves the
public adapter seam and render-state draft transport only; it is not evidence
that renderer-owned live pipeline rebuilding, texture live preview,
`texture.open`, the event stream, or Vulkan frame synchronization is complete.
Single-thread and double-thread application smoke tests passed on
2026-08-27 for both workerThreadCount=1 and workerThreadCount=2, with developer
UI disabled and enabled. Manual interactive ImGui acceptance remains
outstanding.

## Renderer Owner/Session Design

The focused targets intentionally do not instantiate `RenderSystem`. The
following cases are the required application-level tests for the real
`IRendererMaterialInstancePreviewOwner` and
`IRendererMaterialInstancePreviewSession` seam once a Vulkan-backed test
harness is available:

- **Capture baseline:** connect each fixture MI and assert that the owner
  captures the active World identity, normalized MI path, document revision,
  numeric values, texture identities, and all three effective render states.
- **Prepare rejection:** submit drafts changing exactly one of `renderMode`,
  `shadingModel`, or `cullMode`; assert `Failed` at `Prepare`, the diagnostic
  names pipeline rebuild, and the active `Material`, `MaterialInstance`, pass
  routing, descriptors, and runtime resource generation remain unchanged.
- **Mixed draft rejection:** combine a numeric edit with each render-state edit;
  assert the state change is not silently ignored and the numeric value is not
  partially committed.
- **Restore isolation:** after a rejected state draft, restore the captured
  baseline; assert the old live resources remain valid, the document revision
  and dirty state are unchanged, and no replacement generation is reported.
- **Generation invalidation:** change World generation or disconnect during a
  pending owner/session operation; assert the session cannot commit against
  the new World and old resources are retired only by the renderer's GPU-epoch
  policy.

These tests should be added beside the application smoke/integration harness,
not represented by the CPU fake adapter. The existing focused regression below
covers the serialized command boundary and failure-preservation behavior; it
does not prove Vulkan pipeline rebuild, descriptor replacement, pass
reclassification, or GPU-epoch retirement.

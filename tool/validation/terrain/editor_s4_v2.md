# P4 V2 Editor acceptance preparation

Preparation only, 2026-11-21. No renderer was started. Cicero still owns the GPU
window; do not run the `run` command until that work ends and the coordinator
releases the window. The implementer is not the independent P1/P4 reviewer.

## Reused path and V2 differences

The existing `editor_s4_main.cpp` still wraps the real `EditorHost` and forwards
all `IEditorHost` calls. Setting `VL_EDITOR_S4_V2_PLAN` selects the V2 scenario.
Paint/Sculpt/Undo/Redo/Cancel/Save are real `CommandBus::Queue(UiAction)` operations;
the harness never calls `QueueRefresh` to manufacture an Editor result.

`editor_s4_v2.py` prepares, runs and analyzes the V2 scenario. Its `run` command
does not call the legacy `editor_s4.py run`: that legacy command performs bootstrap
and saved full Cook and reads V1 sidecars. Do not use it for P4 V2.

The V2 runner uses the already validated World V2 publication from the Game closure,
including verified production inputs and packaged roots/coarse pages. A new full
Cook is not a prerequisite. Missing publication/input files are explicit preparation
failures, never an invitation to silently bootstrap Cook.
Both selected surfaces must carry nonempty Cook-captured `authorDependencies`
in the new Production schema. The preparer rejects missing/empty baselines before
copying resources; it never reconstructs them from current author files.

Read-only readiness check during this preparation: the author scene/recipe and
explicit adjacency exist, but
`VukanLearnResources/VT/worldV2/RVTv2_Full4x4_R8192_Default_CoverageAtlas/manifest.json`
does not yet exist. No private runnable Full16 stage was copied. Use the actual
validated publication installed by the Game work, or supply its matching resource
root and scene after delivery; do not point the runner at a historical V1 package.

The shared closure walker now recognizes version 2 and does not request the old
`.dependencies.json`, `.coverage.json`, `.height.json` sidecars. V1 legacy tooling
remains available only for historical evidence. Runtime author baselines must come
from `Service.LoadAuthorBaseline` / `Store.Production()` and verified objects.

Prepare uses ordinary file copies, never hardlinks. Author sources, shared shader
cache and source config remain unchanged. The cloned scene is put in authoring
mode. Only the private staged compute source gets a newline appended for real
changed-source shader reload; the next step restores its exact original bytes and
reloads again. A private marker is required before this mutation.

## Prepared scenario

`artifacts/p4-editor-v2-plan01.json` was generated against the actual topic assets.
It selects the explicit `Landscape_00 maxU -> Landscape_01 minU` adjacency.
The 27 steps are:

1. Baseline.
2. Paint A, Undo, Redo.
3. Begin/apply a second Paint stroke, then Cancel in a later stage.
4. Sculpt A, Undo, Redo.
5. Begin/apply a second Sculpt stroke, then Cancel in a later stage.
6. Paint at A's shared edge, then Sculpt at the same edge.
7. Paint and Sculpt B; then 12 rapid alternating Paint/Erase strokes.
8. Material numeric preview, restore, old-generation material rejection.
9. Real changed-source hot reload, exact shader restoration and reload.
10. Preview-published observation, Save A, Save B, saved-preview observation.
11. Queue another edit and World reload; deliver old-generation brush actions;
    capture the reloaded World.

Each step gets 180 ticks at 60 Hz and a screenshot. Frame counts are observation
budgets, not proof that the producer settled. Missing exact Published receipts
still fail the evidence gate; extend the stage observation budget only with an
explicit recorded change, never silently call a timeout a pass.

The wrapper holds at most two old World snapshots per selected surface and
rehashes them on subsequent ticks, including across World replacement. This
detects in-place mutation of published World height/paint. It does not prove the
immutability/lifetime of producer GPU weights, normal-frame and tangent-frame
inputs: those require native evidence below.

The fixed source camera must visibly cover both edited surfaces and the shared
edge. Inspect the prepared scene/capture plan before the GPU run. If the supplied
camera does not do so, author a private camera pose and record it; do not infer
visual correctness from off-screen CPU hashes.

## Commands

Run from `D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/terrain-openworld/VulkanLearn`.
The first block is CPU-only and was executed (with fresh numbered evidence names):

```powershell
py -3 tool/validation/terrain/editor_s4_v2_test.py
py -3 tool/validation/terrain/editor_s4_test.py
py -3 tool/validation/terrain/editor_s4_v2.py plan `
  --resource-root ../VukanLearnResources `
  --output artifacts/p4-editor-v2-plan02.json
```

After Euclid's new Service methods are implemented and the matching Release
Editor libraries have been rebuilt/frozen, build only the wrapper. This does not
start the renderer:

```powershell
py -3 tool/validation/terrain/editor_s4.py build `
  --build-dir build-vt-release `
  --output artifacts/p4-editor-v2-adapter01
```

Do not link against stale historical S4 libraries. The build records source,
library-response and executable hashes. An already linked executable does not
prove those libraries contain the current P4 implementation.

After the validated Game V2 closure is installed in the resource tree, prepare
fresh, short private stages. This copies assets but starts no renderer/Cook:

```powershell
$p4Scene = 'Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_CoverageAtlas/SC_composited_vt_4x4.json'
py -3 tool/validation/terrain/editor_s4_v2.py prepare `
  --resource-root ../VukanLearnResources --scene $p4Scene `
  --stage D:/s4v2/w2 --worker 2
py -3 tool/validation/terrain/editor_s4_v2.py prepare `
  --resource-root ../VukanLearnResources --scene $p4Scene `
  --stage D:/s4v2/w1 --worker 1
```

Only after Cicero finishes and the GPU window is released, run worker2 and worker1
serially. `runtime-bin` must contain the matching deployed DLL closure (or its
dependencies must already be on PATH). Do not start both:

```powershell
py -3 tool/validation/terrain/editor_s4_v2.py run `
  --stage D:/s4v2/w2 `
  --exe artifacts/p4-editor-v2-adapter01/editor_s4.exe `
  --runtime-bin build-vt-release/bin --timeout 600
py -3 tool/validation/terrain/editor_s4_v2.py run `
  --stage D:/s4v2/w1 `
  --exe artifacts/p4-editor-v2-adapter01/editor_s4.exe `
  --runtime-bin build-vt-release/bin --timeout 600
```

`run` exits 2 for failed or incomplete evidence. It preserves the private stage,
process/invocation records, raw log and screenshots. Normal quit is queued by the
host. Timeout termination is a failure and cannot prove teardown.

Reanalysis is CPU-only:

```powershell
py -3 tool/validation/terrain/editor_s4_v2.py analyze --stage D:/s4v2/w2
py -3 tool/validation/terrain/editor_s4_v2.py analyze --stage D:/s4v2/w2 `
  --game-evidence D:/s4v2/w2/evidence/game-publication-receipt.json
```

## Three states are separate evidence

| State | Observation | Must not be inferred from |
| --- | --- | --- |
| Author saved | Height/paint file digests change at Save, paint sidecar valid, disk reopen agrees within encoding precision | A queued brush or an advancing preview revision |
| Editor preview published | Exact ticket reaches Prepared, Submitted, Published; Host ACK follows matching publication; images/World inputs and native weights/frame inputs match | Queue acceptance, CPU70, or screenshots alone |
| Game published | Explicit later publication/export, native validator, independent Game reopen of the new identity and pixel comparison | Saving author files or Editor preview publication |

The analyzer checks the Game manifest digest stays unchanged during preview and
author Save. Only the later explicit Game publication may advance it. The runner
does not publish/export automatically. Pascal/coordinator's official V2 publication
and Game path should consume the saved private stage; no second Cook/publish
implementation is introduced here.

The optional Game receipt includes `status="passed"`, `storeVersion=2`,
`sourceEditorLogSha256`, `beforeManifestDigest`, `publishedManifestDigest`,
`nativeValidatorPassed`, `gameCleanTeardown`, and a `surfaces` map.
Each selected surface binds `authorHeightDigest`, `authorPaintDigest`,
`reopenedProductionIdentity`, `reopenPixelsCompared=true`. Attach original native
validator, Game log, manifest/production identities, screenshots and pixel metrics;
the summary JSON is not independent proof on its own.

## Native telemetry still needed from Euclid/runtime

Place the actual native trace, or a lossless normalized extract bound to its
original log, at `<stage>/evidence/native-refresh.jsonl`. Do not synthesize success
events from snapshots. The harness currently observes the real Service's published
revision and V2 baseline; it cannot observe private queue tickets or GPU fence
events through the existing public interface.

Required events/fields:

- `queue`, `prepared`, `submitted`, `published`: exact `ticket`, `surface`,
  `worldGeneration`, `editSeq`, `revision`, `contentIdentity`. Queue also needs
  `expectedRevision`; submission/publication must bind their GPU epochs.
- Publication/page-table writes/dirty ACK bind those tokens to
  `activeWorldGeneration`, `activeRevision`, `activeContentIdentity`.
  `dirty-ack.state` must be `Published` and follow the exact matching publication.
- `step` on the normalized publication events binds them to the wrapper's step
  markers/frame chronology. Every ordinary edit/Undo/Redo/Cancel/material stage
  needs its matching publication; global event presence is insufficient.
- `stale-rejected`: actual old World/revision/content task, reason
  `world-generation`/`surface-revision`/`content-identity`, `pageTableWritten=false`.
  The rejected token, current token, fence and destination table identities must
  remain visible in the original trace. If the run never creates an in-flight old
  task, report missing evidence and schedule a controlled backlog case.
- `frame-input-immutable`: before/after digests for `weights`, `normalFrame`,
  `tangentFrame`; retain through fence. Include input allocation/object identities
  and task tokens so a hash of unrelated fresh data cannot pass.
- `neighbor-dirty`: `kind=Paint` and `kind=Sculpt`, source/target surface and actual
  affected page keys/ancestors. Verify both the neighbor seam and an unchanged
  distant page. Source-plan dirty alone is insufficient.
- `baseline-v2`: `storeVersion=2`, `v1SidecarReads=0`,
  `verifiedProductionObjects=true`, `capturedDependencyCount>0`,
  `matchesStoredAuthorDependencies=true`, manifest/Production/object identities.
  Retain file-access or baseline-loader evidence for no V1 sidecar lookup.
- `dependency-reload`: changed before/after digest and
  `candidatePublished=true`; include restored-source identity and stale work
  rejection. An unchanged-source hot-reload request is not evidence.
- `author-save`: source revision, save result and actual file hashes.

Still required beyond the checker: per-stage settled production/page-table state,
failure/retry transaction recovery, no old task writing a new page table,
producer memory/epoch retirement budget accounting, normal/TES seam pixels, full
author-save/reopen precision, independent Game publication, and a reviewer who
did not implement this work.

## Preparation evidence

- New checker: final 9 CPU tests passed, `artifacts/p4-editor-v2-checker-tests04.log`.
  First attempt retained in `...tests01.log`: a Windows short/long temporary-root
  alias broke the fixture comparison; canonicalizing its own root corrected it.
- Legacy checker: 39 CPU tests passed,
  `artifacts/p4-editor-v2-legacy-checker-tests01.log`.
- C++ wrapper syntax only passed:
  `artifacts/p4-refresh-cpu/p4-editor-v2-harness-syntax01.log`.
  Existing Vulkan ignored-result warning remains. No new linked harness or GPU
  acceptance is claimed.
- No Full16 asset copy, Cook, Game export or Editor run was performed in this
  preparation turn. P4 remains incomplete; CPU70 is module evidence only.

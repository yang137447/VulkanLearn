# R17 defect closure — editor descriptor teardown leak + submit-fail query recovery

Date: 2026-11-22 · Worktree `D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn`
(branch `codex/terrain-openworld`, base `7d245c1` + working-tree fixes) · GPU: RTX 3080, one renderer at a time,
no concurrent `main_editor`/`main_game`/`main`.
No threshold, config, package, scene, material, or shader source was changed. Production source and tests only.

Result: both defects fixed and GPU-verified. Final combined-tree binaries:
`main_editor.exe` sha256 `A978424C…6087FBD`, `main_game.exe` sha256 `4FAD2735…81F3F96`.
(The editor matrix below was rerun with this final `main_editor`, so it includes the Defect 2 query-state changes too.)

## Defect 1 — editor teardown leak (VUID-vkDestroyDevice-device-05137)

### Root cause
`RenderGraph::RenderInitialize` (`source/renderGraph.cpp:975`) initializes pass descriptors in pass order and
`Renderpass::CreatePassDescriptorSetLayout` / `CreateDescriptorSets` **overwrite** `descriptorSetLayout`,
`emptyDescriptorSetLayout`, `descriptorPool` and the set vectors in place
(`source/renderGraph.cpp:1021-1022`; `source/renderGraphPassResources.cpp:267/304`).
The editor thumbnail session calls `RenderInitialize` on a reused session graph
(`source/engine/editorThumbnailRenderSession.cpp:301`). For the V2 thumbnail graph the call throws at the
`terrainFeedback` pass (its `worldTexture` inputs do not exist in the thumbnail cache) after 13 passes; because the
assignment `descriptorsReady = true` is on the same line, the flag stays false. A later thumbnail request on the same
session calls `RenderInitialize` again and overwrites the 13 already-created pass allocations. `RenderGraph::Shutdown`
(`source/renderGraph.cpp:518`) can only see the last handles, so the previous batch has no owner left.

Instrumented teardown counts (temporary diagnostic, removed before commit): **195 = 26 descriptor set layouts
(13 × Empty + geometry + 4 shadowCascade + 8 localShadow) + 13 descriptor pools + 156 descriptor sets
(13 passes × 4 sets × 3 swapchain images)**, i.e. exactly two 13-pass attempts (`editor-120f-diag3.log`).
The r16 report described them as 195 layouts; the exact composition above is what the device held.

### Fix
- New `Renderpass::DestroyPassDescriptors` (`source/renderGraphPassResources.cpp:841`) releases the pass's two
  layouts and pool (pool destruction frees its descriptor sets) and clears the RHI handle caches.
- `RenderInitialize` now calls it at the start of every pass, before any new state is touched
  (`source/renderGraph.cpp:996`); `DestroyRenderpass` reuses the same helper (`source/renderGraphPassResources.cpp:884`).
- Chinese comment records the lifetime boundary: a pass's layout/pool/set is one ownership batch and the previous
  batch must be destroyed before a retry may install new handles, otherwise `Shutdown` leaks the orphaned batch.

### GPU verification (validation on)
| run | before (r16) | after |
| --- | --- | --- |
| V2 editor 120f | exit 0, VUID 05137, 195 leaked objects (`defect1-before/r16-editor-120f.log`) | exit 0, **no VUID**, 0 leaked, `teardown complete`, 16/16 author baselines (`defect1-verify/editor-v2-120f.log`) |
| V2 editor 8f | exit 0, clean | exit 0, clean (`defect1-verify/editor-v2-8f.log`) |
| non-terrain editor 120f (`SC_fracture_playground`) | exit 0, clean | exit 0, clean (`defect1-verify/editor-baseline-120f.log`) |

Command (same for all three, scene/frames substituted):
`build-vt-release\bin\main_editor.exe --initial-scene <scene> --worker-thread-count 2 --frame-limit 30
--present-mode immediate --validation --exit-after-frames 120`
(<scene> = `Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_CoverageAtlas/
SC_composited_vt_4x4.json` or `Maps/SC_fracture_playground/SC_fracture_playground.json`).

## Defect 2 — submit-failure recovery (timestamp query not ready)

### Root cause
`EnvironmentGpuTimer` owns one timestamp query range per swapchain image. At record time `BeginFrame` read the
previous batch using recording-time `used` flags and then recorded `resetQueryPool` into the current command buffer
(`source/render/environment/environmentGpuTimer.cpp:62-77` old); `BeginProduct` set `used = true`
(`:97-103` old). On the `submitFail` frame (renderFrame 7, submission epoch 8) the whole command buffer is discarded
(`source/render/frame/renderFrameRecorder.cpp:1159`) and the recovery submission records only an empty clear
(`source/render/backend/frameAbandonVulkan.cpp:29-38`), so the environment reset/begin/end timestamps never reach the
GPU. The next acquisition of that image still saw `used = true` and blocking-read the range in
`CollectFrameResults`; the range's last executed reset came from an earlier submitted frame that wrote no timestamps
(or the range had never been written since that reset), so `RHIDeviceVulkan::ReadTimestampQueryPair`
(`source/render/rhi/vulkan/rhiDeviceVulkan.cpp:232`) returned `VK_NOT_READY`. The throw escaped `RecordIbl` at the
start of the next frame and became the fatal `Render thread failed` message
(`source/render/renderThread.cpp:175`), exit 2.

`ParticleGpuTimer` had the same false-`used` pattern; its read already catches and disables timing, so it was not the
fatal path, but it was fixed for the same ownership rule.

### Fix
- New `GpuTimestampQueryBatch` (`source/render/backend/gpuTimestampQueryBatch.h`): per-image `recording` /
  `open[]` / `submitted` state machine. A batch only becomes readable through `Submitted()` (complete begin/end pair)
  and `Aborted()` removes a discarded recording; an earlier submitted but unread batch is preserved.
- `EnvironmentGpuTimer` and `ParticleGpuTimer` use it; new `FrameSubmitted()` / `AbortFrame()`.
- `RenderSystem::SubmitFrameGpuQueries` / `AbandonFrameGpuQueries` are passed to the recorder as two context
  callbacks (`source/render/frame/renderFrameRecorder.h`). The recorder abandons at every pre-submit discard point
  (`renderFrameRecorder.cpp:1134/1147/1163`), confirms submission immediately after `backend.SubmitFrame`
  (`:1256`), and abandons in the outer failure catch when the frame was not submitted (`:1355`).
- Genuine failures are not hidden: a not-ready read after a confirmed submission is still fatal; only an explicitly
  abandoned frame is skipped.

### CPU test
`tool/renderer-backend-tests/gpuTimestampQueryBatchTests.cpp` (6 cases: complete batch only after submit, discarded
recording never read, incomplete pair rejected, duplicate begin rejected, abort preserves older submitted batch,
next recording starts clean). Whole `vl_renderer_backend_tests`: 311 tests, 310 passed; the one failure,
`TerrainFeedbackGraph.CompilesDedicatedColorAndDepthPass`, is pre-existing and unrelated (expects 5 color outputs,
current config has 7; untouched by this diff).

### GPU verification
- Before (r16): exit 2, `Vulkan timestamp query was not ready…`, `publishedChannels=0` but recovery fatal
  (`defect2-before/r16-result.json`, `r16-runtime.log`).
- After: private copy `D:\rvt-final-r16-submitfix\pkg-submit` of `p5g/C010r16` (source package unchanged, its
  `main_game.exe` still `AD8445C0…6FA5`; only the private copy's binary replaced by the fixed build) run with the
  frozen `commands/submit-fail.json` schedule, 400 frames, `--validation`, `--worker-thread-count 2`,
  `--present-mode immediate`, `--no-dev-ui`.
  Result: **exit 0**, world committed, clean teardown, `errorLines: []`, no VUID.
  `submit-fault-consumed` (seq 204) → `submit-abort-observed` (`publishedChannels=0`, `sceneSubmitCalls=0`,
  recoveryEpoch 8) → `old-world-frame-completed` (fenceCompletedEpoch 9); the failed page was not published and the
  same page was re-produced at epoch 11 with `status=0`; the process continued to frame 399 and the evidence file is
  complete (838 records + terminal). See `defect2-verify/` (result, runtime log, runtime-events JSONL).

## Build / test commands and results
```powershell
cmake --build build-vt-release --target main_editor -j 4                 # built
cmake --build build-vt-release --target main_game -j 4                   # built
cmake --build build-vt-release --target vl_renderer_backend_tests -j 4   # built
build-vt-release\bin\vl_renderer_backend_tests.exe --gtest_filter=GpuTimestampQueryBatch*   # 6/6 passed
build-vt-release\bin\vl_renderer_backend_tests.exe                                        # 310/311 (1 pre-existing)
```
GPU runs are listed above; all used the existing `build-vt-release` Release binaries. Runtime evidence for both
defects is under `artifacts/terrain-rvt-production/final-delivery/r17-defects/`
(`defect1-before`, `defect1-diagnostic`, `defect1-verify`, `defect2-before`, `defect2-verify`).

## R17 freeze / re-export
**Required for binaries, not for data.** Both fixes are production-source changes, and the R16 frozen package embeds
the old `main_game.exe`/`main_editor.exe`. A freeze must rebuild and re-export `main_game` (and `main_editor`) into
the R17 package. No scene/VT/package/material/shader/threshold re-cook or asset migration is needed; the existing
C010r16 data package and its acceptance evidence remain valid.

## Remaining risk
- Editor thumbnails for graphs containing `terrainFeedback` worldTexture inputs still fail to initialize (pre-existing,
  now non-leaking); the failure is reported as a thumbnail error, not a crash/leak. Fixing thumbnail graph trimming is
  out of this defect's scope.
- The environment timer keeps a blocking read for confirmed submits; a genuine `VK_NOT_READY` after a completed fence
  remains fatal by design (failures are not masked).
- `GpuPassTimer` and terrain production timing were not changed: they already use delayed/non-blocking reads and epoch
  gating; the abort path still notifies `GpuPassTimer` as before.

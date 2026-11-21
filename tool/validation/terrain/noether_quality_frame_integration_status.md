# Quality/Frame integration checkpoint

2026-11-21. Source/CPU checkpoint, **not GPU or Game acceptance**.
No commit, GPU launch, shared build write, or resource-worktree edit.

## Landed

- `TerrainCompositedQualityReady` reads a persistent, mutex-protected validation
  ledger and the Service resource predicate. `TakeObservation` only consumes the
  separate latest event. Missing GPU evidence remains false.
- Frame enables quality capture without an environment variable.
  `VULKANLEARN_RVT_QUALITY_DIR` only enables bounded binary export.
- Draw snapshots are taken after Service uploads. The fixed surface dictionary
  compares revision, generation, all digest words and actual page-table views
  each feedback frame.
  A changed resource version rejects earlier in-flight success.
- The validation ledger records explicit arrival sequence/time, world and graph
  generation, pose, resource version, actual capture matrix and completed epoch.
  Repeated getter calls do not consume or mutate evidence.
- Explicit revalidation requests immediately revoke getter visibility before RT
  processes the request. Ordinary small movement retains evidence; movement
  outside the validated view domain requires another capture. The view-domain
  guard can only revoke evidence, never create successful GPU counts.
- `renderGraphDepthOwner.*` retains the actual graph image/view/sampler leaf
  destructor package. Creation/destruction in `renderGraphPassResources.cpp`
  register/release `terrainFeedbackDepth` and existing scene viewport outputs.
  Recorder installs the actual `SetDepthOwnerResolver` callback.
- Capture only admits borrowed sampled single-sample 128x128 D32. The original
  depth-writing pass finishes before the sampled barrier and attachment-free
  replay; b5 is SSBO and b6 is borrowed depth. Input collection explicitly
  filters graph bindings to 0..4 before indexing the five-element array.
- Loading uses Einstein's instance cache and explicit metadata cost. Its returned
  lifetime and capture submission lifetime live in fixed eight-entry frame-fence
  slots, with completion checks before reuse. Abort drops only the unsubmitted
  transaction's loading reference.
- Editor viewport black clear is recorded before its shader-read transition and
  UI sampling, using its real graph leaf owner. No new image or owning Texture.
- Game final-target black clear is now connected as well. Backend/RHI return the
  actual `PresentSwapchainResources` owner plus generation. The leaf owns the
  native image views and swapchain and destroys them in that order only after
  the final borrowed reference drops. `VulkanManager` publishes complete leaves
  transactionally and detaches old target aliases at the existing resize safe
  point. No new swapchain or ordinary-frame `WaitIdle` path was introduced.
- The three successful explicit camera setters now request revalidation in
  `runtimeCommandExecutorExecuteCommand.cpp`; `GetCameraState` does not.
- Resize generation/extent changes revoke prior quality evidence. Fixed frame
  slots also honor the retire queue's already-confirmed completed epoch, covering
  resize's existing device-idle completion when the backend fence ring resets.
- Preparation errors revoke evidence and retry at most 4Hz. Errors after capture
  command recording rethrow, requiring the caller to discard the command buffer.
  Readback/export exceptions are surfaced, not silently latched into loading.
- Runtime captures continue after the diagnostic export cap. Export remains
  process-bounded; dropping a completed readback releases the global capture gate.
- Frame polls `QualityConvergenceFailed` before pose reset/capture and after
  readback. A failure revokes readiness, appends an Error to `ProcessLog`, emits a
  single `[RVT_QUALITY_FAILURE]` JSON event with actual last counts/pose/version,
  and throws `Terrain.QualityConvergenceFailed`. It does not silently keep loading.

## Required coordinated existing-file deltas

These are outside the assigned Service/Runtime writer boundary.

1. Service currently pre-reserves all 4 MiB of GPU driver metadata. Deduct
   `TerrainQualityAbi::FrameMetadataGpuBytes` (1,179,648 B) from that base
   reservation before reserving the Frame components against the same World
   owner: quality 128 KiB plus eight Loading entries at 128 KiB each.
   Preserve the original total reserve and the 32 MiB Workspace hard cap.
   Include the deferred Frame share in World preflight. CPU Loading metadata is
   eight times 32 KiB plus cache size, charged by its existing constructor.
2. Resource change publication should request revalidation at the actual change
   event, not at delayed readback. Camera command call sites are now installed.
3. Service must distinguish initial startup from later pose convergence.
   `TerrainCompositedQualityMonotonicMs()` uses steady-clock epoch milliseconds,
   shared by the request and Frame events. `pose_arrived.arrivedMonotonicMs`
   records explicit request execution, including any preparation after arrival.
   Frame records `world_frame_arrived`, `pipeline_prepare_started/completed`,
   `capture_submitted/completed`; world-frame arrival is **not** world commit or
   process start. Initial process-start to interactive is limited to 86.748 s,
   with actual World commit logged separately. Subsequent pose-arrival to
   interactive is limited to 10 s. The existing unconditional Service 10-second
   permanent latch still needs this correction; Frame does not move arrival
   forward to hide shader/pipeline compilation.
   The failure atomic must also advance when preparation never produces a
   readback; only updating it in `ObserveQuality` cannot bound endless retries.
4. Worker2 already reports the thrown failure through the existing
   `PollEngineRenderThreadFatalError` Diagnostics/exit-code-2 path. Worker1's
   `RunEngineRenderSubmitPhase` currently lacks a corresponding catch around
   `RenderLatestSnapshotOrLastGood`. A narrow GT error boundary is needed for
   clean exit-code-2 teardown in that mode, rather than an uncaught exception.
   The loading gate leaves platform-event and console command processing before
   gameplay input suppression, so normal close/console quit remain available.

## Next native link inputs

Recompile Frame/recorder/QualityGpu and `renderGraphPassResources.cpp`.
Also include new `terrainCompositedReadiness.cpp`,
`terrainCompositedQualityValidation.cpp` and `renderGraphDepthOwner.cpp`, plus
Einstein's latest `terrainCompositedLoading.cpp`. CMake CONFIGURE_DEPENDS discovers
the new sources; a manual archive-overlay link must explicitly include them.
Keep Euclid's shared `build-vt-release` ownership.
Also include new `render/rhi/vulkan/presentTarget.cpp` and rebuild
`vulkanManager.cpp`, `rhiDeviceVulkan.cpp`, `rendererBackendVulkanPipelines.cpp`,
camera executor and recorder. `presentTarget.h` changed: the actual GPU stage
needs the normal dependent-header rebuild, not just an old-archive CPU overlay.

## Verification and remaining execution

The combined CPU suite has 29 passing tests covering raw ABI, all eight tap pairs, exact
owner identities/revisions/epochs, non-consuming readiness, explicit arrival,
micro-motion, large-world perspective motion, graph/world replacement, concurrent
getters, graph leaf lifetime and fence-slot reuse. The binary decoder has 18
passing Python tests. Outputs are in the task-specific temporary build directory.
Latest Frame, QualityGpu, recorder, graph-resource and new helper sources compile
independently with MinGW. An eight-object relocatable link passes and exports
the real readiness getter with the validation/depth-owner helper references
resolved. A subsequent native `main_game` CPU overlay link with all sixteen fresh
entry/Frame/GPU/owner/RHI/backend/command objects also passes. The executable was
not run and is not a clean rebuilt stage. The architecture boundary audit passes.
Five further owner CPU tests intercept the two Vulkan destruction functions and
verify actual leaf destructor calls, view-before-swapchain order, last-fence
retention, generation replacement, partial-candidate cleanup and no double free.

No minimal-World GPU validation, clean Game build, black-frame pixel verification,
initial timing acceptance or pose convergence acceptance is claimed. After the
coordinated deltas, run the real minimal-World quality replay with Vulkan
validation in an assigned exclusive GPU window before staging the Full16 route.

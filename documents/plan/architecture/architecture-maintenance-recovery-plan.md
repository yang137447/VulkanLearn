# Architecture Maintenance And Boundary Recovery Plan

## Status

- Type: executable architecture repair plan
- Created: 2026-08-14
- Completed: 2026-08-14
- State: completed; retained as implementation history
- Current architecture contract:
  - `documents/architecture/vulkanlearn-architecture.html`
  - `documents/plan/architecture/ue-lite-completion-plan.md`
- Related rendering contracts:
  - `documents/rendering/shader-build-cache.md`
  - `documents/rendering/shader-hot-reload.md`
- Deferred R6 follow-up:
  - `documents/plan/architecture/renderer-cohesive-runtime-extraction-plan.md`

## Completion Record

R1-R5 and R7 were implemented. R6 was deliberately deferred because extracting
CSM state, post-process controls, environment orchestration, and RenderGraph
Vulkan state ownership in the same repair would materially expand the lifetime
risk. The exact follow-up scope and acceptance matrix are recorded in
`renderer-cohesive-runtime-extraction-plan.md`.

Implemented ownership boundaries:

- `RendererBackendVulkan::CreatePipelineFactory()` is the Vulkan owner-only
  construction path for pipeline objects; no public backend raw device, memory,
  or RHI getter remains.
- `RuntimeTestHooks` owns test state machines, feature implementations live
  under `source/engine/testing/`, and only
  `RuntimeValidationServices` may adapt tests to `RenderSystem`/`RenderGraph`.
- `ShaderReloadRuntime` owns monitor, compile worker, pending source union,
  source epochs, generations, scheduling, and reload diagnostics.
- `WorldGraphTransactionCoordinator` owns World/resource/RenderGraph/pipeline/
  runtime binding prepare and prevalidated commit orchestration.
- Initial World loading, runtime World replacement, and M_ definition reload use
  `WorldTransitionCoordinator::PrepareWorldLoad()` and isolated candidate
  `RendererResourceCache` packages. The mutable cache snapshot/restore bridge
  and old direct initial-load path were removed.
- Startup initializes only the RenderSystem resources required by candidate
  preparation before the initial transaction, then initializes the UI overlay
  after successful publication. The initial candidate publishes any missing
  process-global BRDF LUT binding together with its World-local package;
  established global bindings are immutable.

Validation completed on 2026-08-14:

```powershell
powershell -ExecutionPolicy Bypass -File tool/ue-lite-final-validation.ps1 -ReloadStressCount 20
```

The maintained Debug matrix passed build, boundary audit, CTest 3/3, all seven
Shader/World transaction tests, 120-frame smoke, procedural-sky environment
stress, 20 World reloads, bad scene/material/mesh/texture rollback, high-light
buffer retirement, resize 6/6, and RenderGraph reload 6/6. Logs:
`artifacts/ue-lite-validation/20260814-143055/`.

An isolated explicit MinGW Release configuration also passed:

```powershell
cmake -S . -B build-architecture-release-mingw `
  -G "MinGW Makefiles" `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/Software/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/Software/mingw64/bin/g++.exe
cmake --build build-architecture-release-mingw -j
ctest --test-dir build-architecture-release-mingw --output-on-failure -j 1
```

The Release build used GCC/G++ 15.1.0 and CTest passed 3/3. The final boundary
audit and focused searches found no raw backend device getter call, legacy
World loading/cache snapshot API, RuntimeTestHooks/feature-test singleton
access outside the exact adapter, ambiguous production maintenance marker, or
Shader/World `waitIdle()` call. No Vulkan validation error, device loss,
half-committed owner generation, use-after-free, or retirement leak was
observed in the completed runtime matrix.

This plan repairs architecture regressions found after the Shader incremental
build and hot-reload work. It is not a renderer redesign. Preserve the existing
transactional behavior, runtime coverage, and Vulkan-first direction while
restoring the repository's documented ownership boundaries.

## New Thread Goal

Use the following as the task contract in a new thread:

```text
Implement documents/plan/architecture/architecture-maintenance-recovery-plan.md
end to end. Preserve all existing user changes and the completed Shader/World
transaction behavior. Restore the static architecture audit without deleting or
weakening its boundary rules, split invasive runtime-test access behind narrow
test services, reduce EngineLoop ownership, and converge World loading onto one
prepare path. Run the full serial validation matrix before marking the work
complete.
```

Do not stop after making the project compile or after making the audit script
return zero. Completion requires implementation, focused tests, runtime
validation, documentation synchronization, and no known remaining boundary
violation covered by this plan.

## Baseline Findings

The 2026-08-14 baseline has strong runtime correctness:

- Debug MinGW build passes.
- `ctest --test-dir build --output-on-failure` passes 3/3 tests.
- The following runtime tests pass serially:
  - `--shader-reload-test`
  - `--shader-auto-reload-test`
  - `--shader-compute-reload-test`
  - `--shader-definition-reload-test`
  - `--shader-ui-reload-test`
  - `--shader-shutdown-inflight-test`
  - `--world-graph-transaction-test`
- Shader compile, reflection, ABI rejection, rollback, publication, World/Graph
  ownership swap, and GPU-epoch retirement behavior are covered.

The architecture is not currently at a releasable baseline because:

1. `tool/ue-lite-boundary-audit.ps1` fails.
2. Pipeline code pulls raw `vk::Device` state from
   `RendererBackendVulkan::GetDevice()`.
3. `RuntimeTestHooks` directly includes and calls `RenderSystem` and
   `RenderGraph`, violating the command/test boundary.
4. `EngineLoop` owns growing Shader monitor/worker/epoch/pending-source and
   World/Graph transaction implementation state.
5. Initial World loading and runtime World replacement use separate resource
   loading paths.
6. `runtimeTestHooks.cpp` has grown into a single subsystem-spanning test file.

The working tree was already broadly modified when this plan was created.
Never reset, revert, or overwrite unrelated user work.

## Required Reading

Read in this order before editing:

1. `AGENTS.md`
2. `documents/architecture/vulkanlearn-architecture.html`
3. `documents/plan/architecture/ue-lite-completion-plan.md`
4. `documents/rendering/shader-build-cache.md`
5. `documents/rendering/shader-hot-reload.md`
6. `tool/ue-lite-boundary-audit.ps1`
7. `tool/ue-lite-final-validation.ps1`
8. `source/engine/engineLoop.*`
9. `source/engine/runtimeTestHooks.*`
10. `source/world/loading/worldTransitionCoordinator.*`
11. `source/renderSystem.*`
12. `source/renderGraph.*`
13. `source/render/backend/rendererBackendVulkan.*`
14. `source/render/rhi/vulkan/rhiDeviceVulkan.*`
15. `source/pipeline/*`
16. `source/shader/reload/*`
17. Current `git status`, `git diff --stat`, and relevant file diffs

## Non-Negotiable Constraints

- Do not weaken, delete, or broadly allowlist a boundary audit rule to make the
  audit pass.
- A false-positive audit rule may only be changed if an equivalent or stronger
  rule replaces it and the reason is documented in the same change.
- Do not restore a cross-API RHI abstraction. Vulkan remains the only graphics
  API and public rendering types may remain Vulkan-native.
- Do not use `device.waitIdle()` for Shader or World hot reload.
- Do not change BLAKE3 identities, manifest commit semantics, source staleness
  validation, ABI rules, or GPU-epoch retirement unless required to preserve
  current behavior during extraction.
- Compile workers must remain CPU-only and must not touch Vulkan, live
  Material/PipelineFactory caches, or the manifest.
- Runtime tests that share `shader/spv/` must run serially.
- Keep final ownership publication no-throw. Any fallible work belongs in
  prepare/preflight or needs a real rollback.
- Do not mix unrelated RenderGraph, material, UI, foliage, or environment
  feature work into this repair.
- Prefer named helper types and methods over new flow-heavy lambdas.

## R1: Restore The Vulkan Device Boundary

### Problem

The current worktree introduced raw device pulls from the backend facade:

- `source/pipeline/computePipeline.cpp`
- `source/pipeline/graphicsPipeline.cpp`
- `source/pipeline/pipelineFactory.cpp`
- `source/pipeline/pipelineLayoutBuilder.cpp`

The forbidden shape is:

```cpp
rendererBackend->GetDevice();
rendererBackend.GetDevice();
```

This contradicts the current architecture contract: callers use
backend/device-boundary intent APIs; raw device state does not escape through a
public backend getter.

### Required Repair

Use the smallest design that closes the escape without creating an API-neutral
wrapper.

Preferred direction:

1. Remove external pipeline calls to `RendererBackendVulkan::GetDevice()`.
2. Remove the public backend `GetDevice()` escape if no legitimate external
   caller remains.
3. Let the owning Vulkan boundary create `PipelineFactory` and inject the
   Vulkan-native pipeline creation context during construction.
4. Keep descriptor layout/pool/set lifecycle routed through the existing
   backend/device lifecycle-handle APIs.
5. Keep shader module, pipeline layout, graphics pipeline, and compute pipeline
   creation Vulkan-native. Either:
   - inject the required `vk::Device` reference privately when the factory is
     created by the Vulkan owner, or
   - add narrowly named Vulkan backend operations for those exact objects.
6. Do not add a generic `GetRhiDevice()`, `GetNativeDevice()`, `void*` escape,
   public context bag, or new global singleton.

If raw `vk::Device` injection remains inside Vulkan-specific pipeline objects,
the constructor must not become a general public escape. Prefer a private
constructor plus owner friendship, or another construction path that only the
backend/device boundary can call.

### Audit And Tests

Extend the audit so it verifies both:

- no `rendererBackend.GetDevice()`/`rendererBackend->GetDevice()` calls outside
  the backend/device boundary;
- the public section of `RendererBackendVulkan` does not expose `GetDevice()`,
  `GetGpuMemoryProperties()`, or `GetRhiDevice()`.

Acceptance:

```powershell
rg -n "rendererBackend(\.|->)Get(Device|GpuMemoryProperties|RhiDevice)\(" source
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
cmake --build build -j
```

The `rg` command must return no production call sites.

## R2: Restore The Runtime Test Boundary

### Problem

`RuntimeTestHooks` currently includes `renderSystem.h` and `renderGraph.h` and
directly accesses their singletons. It also reaches into private `EngineLoop`
state through friendship. This makes tests effective but turns the test harness
into another owner of engine internals.

The architecture contract requires:

- startup arguments and console input publish runtime commands;
- `RuntimeTestHooks` owns test state machines;
- production systems execute operations at their normal safe points;
- tests observe explicit results and diagnostics instead of navigating global
  internals.

### Required Repair

Introduce narrow test-facing data and operation boundaries. Names may follow
the existing code style, but responsibilities must remain explicit.

Suggested types:

```text
RuntimeValidationSnapshot
    Read-only generations, resource identities, pending retire counts,
    pipeline/artifact identities, light capacity, and other assertion data.

RuntimeValidationRequest
    A test-only request for a specific fault injection or safe-point operation.

RuntimeValidationResult
    Structured outcome returned after EngineLoop/renderer code executes the
    request through the production operation.

RuntimeValidationServices
    Engine-owned facade that captures snapshots and applies test requests
    without exposing RenderSystem, RenderGraph, PipelineFactory, or backend
    object pointers to RuntimeTestHooks.
```

Rules:

1. `RuntimeTestHooks` must not include `renderSystem.h` or `renderGraph.h`.
2. `RuntimeTestHooks` must not call `RenderSystem::GetInstance()` or
   `RenderGraph::GetInstance()`.
3. User-visible behavior such as reload, resize, graph reload, and Shader reload
   must still travel through the runtime command path.
4. Deterministic failure injection may use a test-only service, but that service
   must call the same production prepare/commit operation and may not reproduce
   the operation inside test code.
5. Snapshot DTOs contain values and stable identities, not mutable engine
   pointers. A temporary weak identity used only to prove epoch retirement may
   be returned by a narrowly named test API, but must not become a general cache
   accessor.
6. Remove `friend class RuntimeTestHooks` from `EngineLoop` after all access is
   expressed through explicit APIs.
7. Do not move test phases, counters, assertions, or expected-failure state
   back into `EngineLoop`.

### Split The Test Implementation

Split `source/engine/runtimeTestHooks.cpp` by responsibility. A reasonable
layout is:

```text
source/engine/testing/runtimeTestHooks.cpp
source/engine/testing/runtimeTestFixtures.cpp
source/engine/testing/shaderReloadRuntimeTests.cpp
source/engine/testing/worldTransactionRuntimeTests.cpp
source/engine/testing/rendererLifecycleRuntimeTests.cpp
```

Exact names may match existing CMake/source layout. Keep shared fixture helpers
private to the testing module. Do not create one universal helper header that
includes every engine subsystem.

The split is complete when:

- the main hook file is orchestration, not thousands of lines of fixtures and
  subsystem assertions;
- each feature test includes only the production interfaces it needs;
- no production frame code depends on concrete test implementation types.

### Audit And Tests

Keep or strengthen the existing audit rules:

```powershell
rg -n '#include "render(System|Graph)\.h"|Render(System|Graph)::GetInstance' source/engine source/testing
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
```

Allowed matches must not occur in `RuntimeTestHooks` or its feature test
implementations. Engine-owned `RuntimeValidationServices` may access production
systems in its implementation because it is the explicit adapter boundary; the
audit must allow only that exact file, not an entire directory.

## R3: Extract Shader Reload Runtime State From EngineLoop

### Problem

`EngineLoop` currently owns:

- `ShaderFileMonitor`
- `ShaderCompileWorker`
- pending automatic Shader source sets
- pending material definition source sets
- source epochs and generation counters
- manual/automatic success, stale, and failure diagnostics
- automatic result consumption and resubmission policy
- material-definition reload scheduling

This is implementation ownership, not only lifecycle orchestration.

### Required Repair

Create a GT-owned Shader reload runtime/subsystem under
`source/shader/reload/`. It should own monitor/worker scheduling state and expose
small lifecycle operations to `EngineLoop`.

Suggested public flow:

```text
Initialize(...)
PollSources(...)
ConsumeCompletedCpuCandidate(...)
BuildNextCommitRequest(...)
NotifyCommitResult(...)
Shutdown()
```

The exact API may differ, but preserve these boundaries:

- The compile worker remains CPU-only.
- The runtime/subsystem owns pending source union and source epochs.
- `EngineLoop` supplies the stable frame/safe point and invokes the prepared
  live commit.
- World/material-definition replacement is requested through a typed request;
  the Shader subsystem must not directly own active World, RenderGraph, or
  Controller.
- Diagnostic state required by tests is returned through immutable snapshots,
  not by exposing internal containers.
- Shutdown still joins the worker before Vulkan teardown.

Move the corresponding counters and source sets out of `EngineLoop`. Keep only
dependencies and high-level lifecycle ownership that genuinely belong there.

Acceptance:

- `EngineLoop::Tick()` remains readable as top-level orchestration.
- `EngineLoop` does not implement debounce, pending-union, source-epoch, or
  worker-result state machines.
- All seven Shader/World runtime tests still pass.

## R4: Extract The World/Graph Transaction Coordinator

### Problem

`EngineLoop::ExecuteWorldGraphTransaction()` currently prepares and commits:

- candidate RenderGraph Vulkan state;
- candidate World and world-local renderer resources;
- pass-material bindings;
- graphics pipeline cache candidates;
- runtime RenderSystem binding and light capacity;
- formal Shader artifacts and generated includes;
- Controller view target;
- retirement packages for old owners.

The transaction behavior is correct, but the ownership is in the wrong class.

### Required Repair

Create a named coordinator under `source/world/loading/` or
`source/render/rendergraph/` whose responsibility comment explicitly states:

- inputs and owners it coordinates;
- prepare products it creates;
- who invokes the final commit;
- that it does not run the main loop or own the compile worker.

Suggested responsibility:

```text
WorldGraphTransactionCoordinator
    Prepares candidate World, world-local renderer resources, RenderGraph,
    pipeline cache state, runtime binding, formal artifact publication, and
    retirement records. Commits only after every fallible step succeeds.
```

Required semantics:

1. Prepare may fail without changing active owners.
2. Source digests are revalidated immediately before formal publication.
3. Formal artifact publication remains the last fallible step.
4. Live owner publication after formal artifacts is prevalidated ownership
   move/swap only.
5. World, resource cache, RenderGraph, RenderSystem, and Controller generations
   advance together.
6. Prepared retirements activate only after every live reference is swapped.
7. Failure never retires a still-active object.
8. Runtime resize retains its existing conservative `WaitIdle()` policy; do not
   mix resize redesign into this extraction.

`EngineLoop` should call a compact `Prepare/Commit` or `ExecuteAtSafePoint`
operation and handle only high-level success/failure and shutdown policy.

## R5: Converge Initial And Runtime World Loading

### Problem

Initial loading currently calls:

```text
WorldTransitionCoordinator::LoadInitialWorld()
    -> LoadWorldThroughResourceCoordinator()
```

Runtime replacement calls:

```text
WorldTransitionCoordinator::PrepareWorldLoad()
    -> World/Graph transaction commit
```

The old path mutates the active cache and uses copied rollback snapshots. The
new path builds an isolated candidate package. Keeping both will cause loader,
material binding, and failure semantics to diverge.

### Required Repair

Converge both paths on `PrepareWorldLoad()` and candidate
`RendererResourceCache` packages.

The initial path does not need to pretend that a previous World exists. It may
use a dedicated initial commit mode, but it must use the same:

- scene validation;
- renderer resource preparation;
- material/pass binding validation;
- WorldBuilder input;
- candidate resource cache representation.

After convergence:

1. Remove `LoadWorldThroughResourceCoordinator()`.
2. Remove or redirect `RequestWorldLoad()` if it bypasses the EngineLoop
   transaction.
3. Remove `WorldLocalResourceSnapshot` compatibility APIs when no caller needs
   copied rollback maps.
4. Keep startup order valid: Shader compilation, Vulkan backend, PipelineFactory,
   and RenderGraph contract must exist before renderer resource preparation.
5. Do not force initialized frame resources into the initial prepare path unless
   their ownership genuinely requires it.

Acceptance:

```powershell
rg -n "LoadWorldThroughResourceCoordinator|CaptureWorldLocalResources|RestoreWorldLocalResources" source
```

Expected result: no old World loading path remains. Any remaining
capture/restore API must have a documented non-World-transition owner and a
focused test.

## R6: Keep Large Renderer Classes From Growing Further

This phase is intentionally surgical. Do not rewrite the renderer.

### RenderSystem

Move only cohesive responsibilities that already have clear data boundaries:

- CSM cascade calculation and cached frame data into a named CSM runtime/helper;
- tone mapping and bloom parameter lookup/update into a post-process control
  helper;
- environment update orchestration into an environment runtime wrapper around
  the existing scheduler, baker, timer, and generator.

`RenderSystem` remains the frame orchestrator and `PassRuntimeServices`
implementation. Do not introduce virtual subsystem hierarchies.

### RenderGraph

Keep `RenderGraphCompiler` as the config-to-compiled-plan boundary. Separate
Vulkan resource-state ownership only where it reduces duplicated destroy,
retire, and swap code. Do not replace Vulkan-native attachment descriptions
with an API-neutral graph abstraction.

### Completion Signal

- New feature work no longer needs to add unrelated controls and state directly
  to `RenderSystem`.
- RenderGraph candidate destruction and active-state retirement use one explicit
  state package.
- Existing render pass ordering, MSAA resolve, descriptor binding, and resize
  behavior remain unchanged.

If this extraction would materially expand the patch after R1-R5 are complete,
record the exact remaining classes and responsibilities in a follow-up
architecture plan rather than performing an unsafe broad rewrite. R1-R5 are
blocking; R6 is complete when the safe cohesive extractions are done or a
concrete deferred plan is documented.

## R7: Repair Audit Clarity Failures

The baseline audit also rejects generic `TODO` and `migration` wording.

Do not delete useful design information. Replace ambiguous markers with one of:

- an explicit current limitation comment;
- an invariant comment explaining why the current implementation is safe;
- an item in the appropriate `documents/plan/` roadmap;
- clearer terminology such as `state transfer`, `schema copy`, or
  `live-value preservation` when `migration` is not describing an architecture
  migration.

Examples:

```cpp
// Current limitation: static bounds do not include SpeedTree vertex wind
// displacement. The foliage bounds roadmap owns the future expansion contract.
```

Avoid leaving open-ended TODO markers in production rendering code.

Run:

```powershell
rg -n "TODO|FIXME|HACK|XXX|\bmigration\b" source
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
```

## Validation Matrix

Run from the repository root. Shader runtime tests must be serial.

### Static And Build

```powershell
git diff --check
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### Runtime Transaction Tests

```powershell
build/bin/main.exe --shader-reload-test --exit-after-tests
build/bin/main.exe --shader-auto-reload-test --exit-after-tests
build/bin/main.exe --shader-compute-reload-test --exit-after-tests
build/bin/main.exe --shader-definition-reload-test --exit-after-tests
build/bin/main.exe --shader-ui-reload-test --exit-after-tests
build/bin/main.exe --shader-shutdown-inflight-test --exit-after-tests
build/bin/main.exe --world-graph-transaction-test --exit-after-tests
```

### Existing Runtime Regression

```powershell
build/bin/main.exe --framesmoke 120 --exit-after-tests
build/bin/main.exe --reloadstress scenes/SC_speedtree.json 20 --exit-after-tests
build/bin/main.exe --reloadfail scenes/DOES_NOT_EXIST.json --exit-after-tests
build/bin/main.exe --reloadfail-material --exit-after-tests
build/bin/main.exe --reloadfail-mesh --exit-after-tests
build/bin/main.exe --reloadfail-texture --exit-after-tests
build/bin/main.exe --lightstress 3 --exit-after-tests
build/bin/main.exe --resizestress 6 --exit-after-tests
build/bin/main.exe --graphreloadstress 6 --exit-after-tests
```

### Validation Script Maintenance

Update `tool/ue-lite-final-validation.ps1` so the maintained final validation
entry includes:

- build;
- boundary audit;
- CTest;
- the seven Shader/World transaction tests;
- existing smoke/reload/resize/graph regressions.

Keep every shared Shader-output test serial. The script must stop on the first
failure and preserve a useful log for that step.

### Release Build

Because R1 changes pipeline construction boundaries, configure and build an
isolated MinGW Release directory after Debug passes:

```powershell
cmake -S . -B build-architecture-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-architecture-release -j
ctest --test-dir build-architecture-release --output-on-failure
```

Do not delete an existing build directory as part of validation.

## Completion Definition

This plan is complete only when all of the following are true:

1. `tool/ue-lite-boundary-audit.ps1` returns zero without weakened boundaries.
2. Pipeline code does not pull raw Vulkan device state from the backend facade.
3. The backend public API has no raw device/RHI escape getter.
4. `RuntimeTestHooks` and feature test implementations do not directly access
   `RenderSystem` or `RenderGraph` singletons.
5. `RuntimeTestHooks` no longer needs friendship to navigate `EngineLoop`
   internals.
6. Shader monitor/worker/pending-source state is no longer implemented by
   `EngineLoop`.
7. World/Graph transaction prepare and commit orchestration has a named owner
   outside `EngineLoop`.
8. Initial and runtime World loading share the same candidate prepare path.
9. The old mutable-cache rollback World loading path is removed.
10. Debug build, Release build, CTest, all seven transaction tests, and existing
    runtime regressions pass.
11. No Vulkan validation error, device loss, use-after-free, half-committed
    owner generation, or retirement leak is observed.
12. Current contracts are synchronized into `documents/architecture/` or
    `documents/rendering/`; this plan is marked completed and retained only as
    implementation history.

## Final Report Requirements

The implementing thread must report:

- files and responsibilities added, removed, or moved;
- which audit violations were fixed and how;
- the final `EngineLoop`, test adapter, Shader reload, and World transaction
  ownership boundaries;
- whether the old World loading path and cache snapshot bridge were removed;
- exact build/test/runtime commands run and their outcomes;
- any deliberately deferred R6 extraction with a concrete follow-up document;
- remaining risks, if any.

# Renderer Cohesive Runtime Extraction Plan

## Status

- Type: deferred architecture follow-up
- Created: 2026-08-14
- State: planned, not implemented
- Source decision:
  `documents/plan/architecture/architecture-maintenance-recovery-plan.md`

The architecture maintenance recovery intentionally stops after restoring the
blocking Vulkan device, runtime-test, Shader reload, and World/Graph ownership
boundaries. Extracting the remaining renderer responsibilities in the same
change would materially expand the patch and its Vulkan lifetime risk.

This plan records the exact R6 follow-up. It does not change the current
renderer contract.

## Scope

### 1. CSM Runtime

Extract the cohesive cascade calculation and frame cache currently represented
by:

- `RenderSystem::ShadowCascadeFrameData`
- `RenderSystem::BuildShadowCascadeFrameData`
- `RenderSystem::CalculateCsmShadow`
- `RenderSystem::ComputeCascadeLightSpaceZBounds`
- `RenderSystem::csmSettings`
- `RenderSystem::shadowCascadeFrameData`

The new helper should consume the frozen `RenderScene`, frame index, pass size,
and `CsmSettings`, then return immutable cascade matrices, splits, and bias.
`RenderSystem` remains responsible for deciding when a shadow pass requests
that data and for copying it into frame UBOs.

Acceptance:

- identical cascade count, split, stabilization, Z-bound, and bias behavior;
- no World or mutable gameplay pointers in the helper;
- existing CSM rendering and runtime regressions remain unchanged.

### 2. Post-Process Controls

Extract tone-mapping and bloom material lookup/update currently represented by:

- `RenderSystem::SetToneMappingMode`
- `RenderSystem::SetBloomStrength`
- `RenderSystem::SetBloomThreshold`
- `RenderSystem::SetBloomKnee`
- `RenderSystem::SetBloomClamp`
- the associated cached values and pass-material weak references

The helper should expose named setters with the current validation and
diagnostic results. It may hold weak material-instance references, but it must
not own RenderGraph or duplicate pass-material creation.

Acceptance:

- runtime commands keep using the same `RenderSystem` public control API;
- JSON remains the source of valid material schema and value ranges;
- no per-frame defensive clamp is added.

### 3. Environment Runtime

Wrap the existing environment components without replacing them:

- `EnvironmentIblBaker`
- compute reload participants
- `EnvironmentGpuTimer`
- `EnvironmentUpdateScheduler`
- `EnvironmentUpdateState`
- `ProceduralSkyCubeGenerator`
- pending source cube and diagnostics snapshot

Move the orchestration in `PrepareEnvironmentResources`,
`RecordEnvironmentIbl`, and `RefreshEnvironmentUpdateDiagnostics` behind one
GT-owned runtime. `RenderSystem` still chooses the frame recording point and
passes the command buffer and swapchain image index.

Acceptance:

- dirty generation, face/mip budgets, active/pending commit, SH broadcast,
  barriers, timestamp diagnostics, and Shader compute participant behavior are
  unchanged;
- `--environmentstress 3 --exit-after-tests --no-dev-ui` passes;
- the wrapper does not own World, RenderGraph, or the main loop.

### 4. RenderGraph Vulkan State Package

Converge active, candidate, and retired graph GPU ownership around the existing
`PreparedRenderGraphState` concept. The package should own:

- MSAA and resolve image resources;
- pass descriptor layouts, pools, and sets;
- render passes and framebuffers;
- compiled plan data required by those resources;
- one explicit immediate-destroy or epoch-retire release path.

Remove duplicated take/clear/destroy bookkeeping only after the package can
support startup, resize, graph reload, World/Graph candidate prepare, active
swap, and retirement with the current behavior.

Acceptance:

- pass ordering, attachment ordering, MSAA resolve, descriptor binding, and
  resize behavior are unchanged;
- candidate failure never retires an active graph object;
- active swap is no-throw after prepare;
- `--resizestress`, `--graphreloadstress`, and
  `--world-graph-transaction-test` pass.

## Execution Order

1. CSM runtime.
2. Post-process controls.
3. Environment runtime.
4. RenderGraph Vulkan state package.

Each item should be a focused change with its own build and runtime validation.
Do not combine this work with new rendering features or an API-neutral graphics
abstraction.

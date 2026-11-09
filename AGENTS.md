# AGENTS.md

This file is the first-stop guide for AI coding agents working in this repository.

## Knowledge Base

User's personal knowledge base lives at `D:\YYBWorkSpace\GitHub\yyb-knowledge-book`. When the user mentions "知识库", refer to this path.

## Document Responsibilities

`documents/` is layered by **how much you can trust it**. Read `documents/README.md` for the full rule;
the short version:

- `README.md` (repository root)
  - human-facing project entry point: setup, build, run, navigation
- `AGENTS.md` (this file)
  - AI-facing collaboration contract: reading order, hidden assumptions, risky areas, editing guidance
- `documents/routing.md`
  - **start here for any source change.** Maps source paths to the documents you must read and the
    gate rules that guard them. Match **all** applicable rules, not the first hit
- `documents/contracts/*`
  - what the system **is now**. Every claim is checkable against source, tests, or a gate rule.
    Safe to treat as fact; if it contradicts the source, the source wins and the contract is a defect
- `documents/rationale/*` and `documents/plan/*`
  - **why** things are the way they are: decisions, evidence archives, rejected options, retired
    implementations, and what is planned. **Not contracts** — technical details there may be stale
- `documents/reference/*`
  - tutorials and study material, not implementation contracts

**Hard rule:** `contracts/` never asserts future behaviour. Anything aspirational goes to
`rationale/` or `plan/`. This exists because this repository repeatedly mistook roadmaps for current
state; several plan documents here describe features with zero source hits. See `documents/README.md`.

Documentation rot is a gate failure, not silent drift — run `py -3 tool\validate-docs.py` after touching
`documents/**`.

For verification entry points and the "change → validation" mapping, read
`documents/contracts/testing.md` (this file does not duplicate that table).

Do not mix these responsibilities casually. If a change affects one layer, update the right document
instead of stuffing everything into `README.md` or `AGENTS.md`.

## Project Summary

`VulkanLearn` is a Windows-first Vulkan learning renderer built with CMake and C++17.

Current implemented areas include:

- Optional Windows x64/.NET 8 C# Blueprint hosting, scene placement and Play/Pause/Stop;
  implemented subset and limits: `documents/contracts/csharp-blueprint-runtime.md`
- Runtime fracture verticals: `balloon` soft-body burst and `glassPane` impact fracture
  with Jolt shards, secondary glass split, support islands, asset-backed WAV audio
  with procedural fallback and a shared 320-shard global budget; current limits:
  `documents/contracts/softbody-runtime.md`,
  `documents/contracts/glass-fracture-runtime.md`,
  `documents/contracts/fracture-budget.md`, `documents/contracts/audio-runtime.md`
- Shared fracture impact world plus visible arrows and the `SC_fracture_playground`
  playable level (Left click shoot, Esc mouse look, WASD move); limits:
  `documents/contracts/projectile-shooting-runtime.md`
- Vulkan device / swapchain initialization
- Shader compilation and SPIR-V loading
- Data-driven scene loading
- CPU heightmap terrain runtime: `terrain` scene objects, TR asset preflight, chunked mesh generation,
  material binding through the existing geometry pass; implemented subset and limits:
  `documents/contracts/terrain.md`
- Data-driven render graph loading
- Material and material instance loading
- BC7/BC5 block-compressed textures from DDS/KTX2 (offline mip chains; no runtime BC mip
  generation or Basis/zstd supercompression); see `documents/contracts/texture-asset-json-v1.md`
- Jolt heightfield terrain collision (`source/terrain/terrainCollision.*`,
  `source/world/loading/terrainCollisionInstances.*`): WorldBuilder retains one plan+TRS per
  `terrain` placement; every script Play creates static bodies in the existing
  `ScriptPhysicsWorld`, and Stop/World replacement destroys them. Editor/preview loads create
  no physics world
- Shadow pass
- Post-process pipeline
- Bloom and tone mapping
- Shader BLAKE3-256 incremental build cache and transitive include invalidation
- Shader hot reload: manual/FileMonitor triggers, compile worker, ABI-compatible Material/Compute/UI replacement, M_*.json schema rebuild, GPU-epoch retirement
- Tracy and NVTX profiling markers

Shading model state (2026-09-12): the per-model implementations were deleted on purpose — ClearCoat, Cloth,
Eye, Hair, Subsurface ×3, ThinTranslucent and TwoSidedFoliage — together with their case assets. Each model is
rebuilt **from its paper**, one at a time, following `documents/plan/rendering/shading-model-alignment-plan.md`
§0.6. The interface surface is deliberately kept because it stays UE-aligned and is currently unconsumed:
`engine/materialInputs.glsl` model structs, `engine/materialSurface.glsl` fields, `engine/gbufferCodec.glsl`
layout, the `ShadingModelID` table, debug-view data plumbing and the renderMode/output macros. Paper models
are reproduced through VL offline first. Runtime promotion is a separate decision: only core models receive
deferred support; other promoted models are forward-only and must reject deferred authoring. Add the evaluator,
the enabled path's dispatch and material validation together; a ShadingModelID without a dispatch case silently
falls back to DefaultLit. Deferred models additionally need GBuffer encode/decode and customData in that change.
Infrastructure that existed only
to serve those models went with them: the four LUT-generator compute shaders (`clothLookupTables`,
`eyeCausticLut`, `hairAzimuthalLut`, `subsurfaceLookupTables`), their C++ generators/loaders, the SSS blur and
composition passes, and set 3 bindings 10..13. `deferredLighting` therefore writes `sceneColor` directly
again (single output, `loadOp=clear`) and `VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT` is now a compile-time
`#error`. Re-adding a deferred model that needs a LUT means adding the graph `input`, the pass material
texture slot and the shader sampler declaration together, keeping binding order equal to the graph input order.

The codebase mixes engine experimentation and learning-oriented iteration. Prefer small, explicit, reversible changes.

## Read Order

When onboarding to the repo, read in this order:

1. `README.md`
2. `source/main.cpp`
3. `source/commonFunction.h`
4. `source/renderGraph.h` and `source/renderGraph.cpp`
5. `source/scene/validation/sceneAssetValidator.*`, `source/world/loading/worldLoader.*` and `source/world/loading/worldBuilder.*` (scene asset validation and world build; the old monolithic `source/sceneLoader.cpp` no longer exists)
6. `source/renderSystem.h` and `source/renderSystem.cpp`
7. `config/config.json`
8. `config/renderGraphConfig.json`

Start any source change from `documents/routing.md`. The design documents themselves:

- `documents/README.md`
  - the layering rule: what is a contract, what is rationale, and the hard rule against future
    assertions in `contracts/`
- `documents/contracts/vulkanlearn-architecture.html`
- `documents/contracts/coding-guidelines.md`
- `documents/contracts/game-ui-stack.md`
  - RmlUi-only runtime/editor UI, producer-only callbacks, SDL input arbitration and immutable GT/RT snapshots
- `documents/contracts/ui-component-system.md`
  - Slate-inspired reusable RmlUi controls; widgets must not depend on World, asset commands or Vulkan
- `documents/plan/ui/rmlui-editor-plan.md`
  - completed RmlUi migration/docking/independent viewport versus future scene documents and asset workflows
- `documents/contracts/texture-asset-json-v1.md`
- `documents/contracts/terrain.md`
  - current CPU heightmap terrain runtime: TR schema, heightmap sampling, chunk mesh/normals,
    scene object integration, material limit and explicit non-goals
- `documents/contracts/material-param-authoring-and-reflection.md`
- `documents/contracts/descriptor-imageinfo-management.md`
- `documents/contracts/gbuffer-channel-ownership.md`
  - current GBuffer attachment set, per-`ShadingModelID` channel ownership, the 4+4 bit ID/flag ledger, packing precision limits, and the deferred-allocation checklist. Forward-only models do not claim GBuffer channels; every enabled path needs its own dispatch case and material validation, or the ID can silently fall back to DefaultLit
- `documents/contracts/shader-build-cache.md`
- `documents/contracts/shader-hot-reload.md`
- `documents/plan/rendering/sky-pass-environment-roadmap.html`
- `documents/plan/rendering/shading-model-alignment-plan.md`
- `documents/rationale/shading-model-case-records.md`
  - per-case evidence archive for the alignment plan (paper-curve pixel measurements, conventions pinned by measurement); `artifacts/` is gitignored, so conclusions live in this record. Rationale, not a contract
- `documents/plan/rendering/archive/`
  - **not contracts**: documents whose subject no longer exists in the repository. The per-model shading model implementations (ClearCoat / Cloth / Eye / Hair / Subsurface ×3 / ThinTranslucent / TwoSidedFoliage) and their case assets were deliberately deleted on 2026-09-12; each model is now rebuilt from its paper while the MaterialInputs interface, ShadingModelID table and GBuffer layout stay UE-aligned. `archive/README.md` records why each file is archived and exactly what was deleted
- `documents/rationale/pitfalls.md`
  - reusable, evidence-backed constraints that are not visible from local code (RCSS colour format,
    screenshot truncation, the missing quit path, console-command quoting)
- `documents/reference/rendering/tone-mapping-tutorial.html`

Current contracts live only in `documents/contracts/`. Everything under `documents/plan/` and
`documents/rationale/` is explicitly non-binding: those describe intent, history, or plans, and must not
be quoted as current behaviour.

## Build And Run

Configure:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
```

Configure fetches Jolt through `FetchContent` because terrain collision is a core CPU
dependency. Offline builds must pre-populate `build/_deps/vl_jolt-src` or provide a reachable
mirror; this is the only third-party download in the default configure path.

Build:

```powershell
cmake --build build -j
```

Executable output:

- `build/bin/main.exe`

Typical runtime expectation:

- Run from the repository root or from an environment where the project root can still be discovered.
- The app expects `config/` and `shader/` inside the repository.
- Runtime assets are loaded from `config/config.json -> resourcePath`.
- Repository-local `resources/` is not the runtime asset root. Generated runtime outputs should go under `resourcePath/Generated/`.

Current local build context:

- the active build generator is `MinGW Makefiles`
- do not assume MSVC-specific behavior in build instructions or code changes
- prefer compiler-portable CMake and C++ unless the task explicitly targets Windows toolchain specifics

## Startup Flow

The current startup sequence is split between `source/main.cpp` and
`source/engine/engineLoop.cpp`:

1. Parse launch options and runtime test flags in `source/engine/launchOptions.cpp`
2. Load `RuntimeConfig`
   - discover project root through `FileSystem`
   - read `config/config.json`
   - read `config/renderGraphConfig.json`
   - cache startup fields such as window size, `initScene`, `resourcePath`, and worker thread mode
3. Initialize `PlatformApplication`
   - initialize SDL and the Vulkan loader-facing platform layer
   - create the SDL Vulkan window and collect required Vulkan extensions
4. Initialize `EngineLoop`
   - initialize input
   - generate material parameter includes
   - compile shaders from `shader/glsl/` into `shader/spv/`
   - initialize the Vulkan renderer backend
   - create `PipelineFactory` and bind renderer resource loading services
   - load the render graph from `RuntimeConfig::GetRenderGraphJson()`
   - initialize the `RenderSystem` resources required by World/Graph candidate preparation
   - load and publish the initial World through the same
     `WorldTransitionCoordinator::PrepareWorldLoad()` +
     `WorldGraphTransactionCoordinator` transaction used by runtime replacement
   - finalize initial-only render objects such as the UI overlay after the
     World/Graph transaction commits
   - start the render thread only when `workerThreadCount == 2`
   - initialize the console subsystem
5. Queue launch-time runtime commands, if any
6. Enter the update + render loop

If a change affects boot behavior, verify it against this order.

## Important Directories

- `source/`: engine and renderer source
- `source/pipeline/`: pipeline factory, builders, graphics and compute pipeline code
- `source/resource/`: image and device texture helpers
- `source/shader/`: BLAKE3 content hashing, atomic file/commit, build-cache manifest, ABI signature, FileMonitor, compile worker, reload coordinator, compute/UI participants
- `extern/BLAKE3/`: locked official BLAKE3 C implementation (MinGW portable path)
- `config/`: top-level runtime config and render graph config
- `<resourcePath>/Maps/SC_<scene-name>/`: scene JSON files
- `<resourcePath>/Maps/SC_<scene-name>/Meshes/`: mesh description JSON and source model data
- `<resourcePath>/Maps/SC_<scene-name>/Terrains/`: terrain descriptor JSON consumed by the runtime terrain loader;
  geometry/LOD/collision limits are in `documents/contracts/terrain.md`
- `<resourcePath>/Maps/SC_<scene-name>/Materials/`: material instance JSON files
- `shader/glsl/`: shader source of truth
- `shader/spv/`: compiled shader output and debug reflection artifacts
- `extern/`: third-party dependencies
- `tool/`: profiling tools bundled in repo
- `source/render/offline/core/`: pure CPU `vl_offline_core` shared by VL's offline session and root GoogleTest suites. The former independent paper-image CLI and its fixed-case loader are retired; do not rebuild a second scene/material authoring path. See `documents/contracts/vl-offline-rendering.md`.
- Offline B-arm correctness: Eq. 3 dye multiplies the entire volume term. Eq. 15 Q uses coverage-weighted per-family mean projection plus uncovered-area NoV, not the old unweighted sum or Lambert pore fill. Keep Eval/Sample/Pdf on the same curve discretization, groom and yarn rotation settings. The former CLI's Table II presets and fixed images are historical evidence, not active VL defaults; see the archived `documents/plan/rendering/archive/offline-paper-renderer-standalone.md`. Root GTest owns the current numerical regression.
- `tool/offline-core-tests/` and `tool/offline-integration-tests/`: root GoogleTest module suites; `tool/validation/offline/` contains external VL stress/export checks and the groom fixture generator, not engine runtime test flags.
- `tool/validation/`: offline paper-case validation harness (`bmp_reader.py`, `measure_plot_curve.py`, `read_plot_text.py`, `check_plot_font.py`, `verify_geometry_term.py`, `verify_brdf_integrals.py`, `verify_cloth_sheen_albedo.py`, `verify_split_sum.py`, `measure_sphere_array.py`, `run_paper_case.ps1`, `console_inject.ps1`); pixel-level curve/UV measurement, on-figure text readback, offline geometry-term / BRDF-integral / cloth-sheen-directional-albedo / split-sum checks, and the sphere-array parameter-yardstick regression for `documents/plan/rendering/shading-model-alignment-plan.md`. These are offline scripts, **not** runtime test commands; outputs go to the gitignored `artifacts/`
  - `verify_cloth_sheen_albedo.py` is the contract for the Cloth A-arm `E_s` directional-albedo LUT (Estévez & Kulla 2017): it pins the offline integral spec (uniform hemisphere, unsoftened reciprocal core, 16×16 with a `√r` axis limited to `θv ≤ 80°`, no colour) and runs the white-furnace / layering / old-reading assertions (`S-03`/`S-04`/`S-10`/`S-27`/`S-32`). A GPU LUT generator that changes the distribution, axis parameterization, or sampling rule must change this file first.
  - `read_plot_text.py` skips the runtime-UI bounding box by default. Short figure legends sit entirely inside
    that box, so to verify their text: capture once with `config.json -> ui.enabled = false` and read that
    image with `--no-ui-overlay` (the config edit is temporary; restore it afterwards). `--legend-count` must
    match the figure's legend rows (the shader tightens the row step at 4+ rows) and `--row annotation` reads
    the right-aligned parameter note.
  - `check_plot_font.py` guards `M_brdfPlot.surface.glsl`'s bitmap font: structure checks plus ASCII art of every
    glyph. Glyph mix-ups (a `B` row block inserted at `C`) decode consistently and therefore pass readback —
    only the art (human eyes) and the pixel regression catch them.
- `documents/`: active architecture and planning documents

Offline B-arm area lighting uses a one-sided square emitter with power-heuristic
MIS between area and material-direction samples. Use the yarn radiance kernel,
never the A-arm BRDF or an additional macro cosine. Both direct estimators run
at each vertex; continuation emitter hits terminate without adding emission
again. Current VL scene fixtures author the placement, size, strength and
camera; none are recovered paper-scene metadata.

## Data Model Conventions

This project is heavily data-driven. Many runtime objects are created from JSON.

### Core config files

- `config/config.json`
  - global runtime config
  - window size
  - initial scene
  - MSAA level
- `config/renderGraphConfig.json`
  - render resources
  - pass order
  - pass inputs and outputs
  - post-process material bindings

### Resource naming patterns

- `SM_*.json`: mesh descriptor files
- `TR_*.json`: terrain descriptor files (heightmap geometry + material instance; see `contracts/terrain.md`)
- `MI_*.json`: material instance files
- `scene*.json`: scene definitions

### Scene JSON top-level fields

`SC_*.json` (and any other asset with `"type": "scene"`) carries four required top-level fields:

- `name`: display label (the UI scene selector uses it as the list label)
- `type`: must be `"scene"`
- `description`: **required, non-empty** — what the scene is for and which paper / book knowledge point
  it validates. Probe scenes must name the paper plus section or figure (e.g. `Karis 2013 §Specular G 式(4)`),
  and learning-only scenes must say they are not validation scenes. `SceneAssetValidator` rejects a scene
  without it, so a new scene cannot silently ship undocumented; runtime-test fixtures that build a scene
  in code must write one too. Written in Chinese, same as the planning documents.
- `objects`: array of scene objects (see below)
- `runtimeMode`: optional `"authoring"` (default) or `"gameplay"`. `gameplay` marks a
  native试玩 scene that advances fracture/projectile physics without a C# Play state;
  authoring scenes remain gated by Play/Pause/Editing.
- `projectile`: optional projectile recipe (`materialInstance`, `speed`, `radius`,
  `mass`, `cooldown`). Runtime uses this explicitly instead of scanning scene objects.

`balloon` and `glassPane` additionally accept an optional `profile` string selecting a
registered `DestructibleProfile` (`balloon.default`, `glass.default`, `glass.tempered`).

### Current scene object types

Scene JSON currently supports these `type` values:

- `blueprintInstance` (inline C# class or BP_ asset reference, persistent id, typed properties and component tree,
  optional root Jolt body; spawnAssets preloads runtime spawn templates;
  requires `VULKANLEARN_ENABLE_CSHARP_BLUEPRINTS=ON`)
- `mesh`
- `directionalLight`
- `pointLight`
- `spotLight`
- `squareAreaLight` (one-sided emitted radiance; the current realtime path rejects it)
- `camera`
- `environment`
- `terrain` (TR heightmap mesh; CPU chunks rendered through the geometry pass)
- `wind` (scene-global wind, at most one per scene; `direction`/`strength`/optional `gust`;
  no position/rotation; see `documents/contracts/scene-wind.md`)
- `balloon` (runtime rubber soft body; current scope and limits are in `documents/contracts/softbody-runtime.md`; Jolt impact, convex-hull shards, shard-shard contact, neck anchoring, approximate Jolt-pose coupling and asset-backed audio with procedural fallback are connected; advanced spatial audio/material are not)
- `glassPane` (runtime glass plate; current scope and limits are in `documents/contracts/glass-fracture-runtime.md`; impact-point radial fracture, Jolt convex-hull shards, attached plates, support islands, CCD projectiles, landing secondary fracture and asset-backed glass audio with procedural fallback are connected; transparent material is not)

`terrain` requires `terrainPath` and is preflighted by `TerrainAssetLoader::Prepare`, which decodes
the heightmap before a World candidate is committed. Optional scene-object `pageOrigin` /
`pageTilesPerSide` override the TR's virtual page region; each terrain object gets
`placementId = terrain|<name>`, so the same TR may be placed several times in one World
(empty `placementId` falls back to `assetPath`). The renderer expands it into chunk
`MeshObjectBuildPlan`s; there is still no separate World terrain type. `M_terrain` implements Phase C2c + P3 streaming:
up to 256 layers in the layer table; `TerrainPageManager` registers all terrains in a World into one
32×32 global page table and 4096×4096 ID/weight atlases (top-4 per texel); terrain meshes carry global
page coordinates in `texCoord1`, and the shader uses `texelFetch(pageTable)` then samples the atlases
and three texture arrays. `TerrainLayerTable::BuildImage` packs a 256×4 RGBA32F LUT for
per-layer metadata (row0 height/slope, row1 noise/blendEdge/uv, row2 pagesPerSide/pageTableBase/
sourceSet/flags, row3 avgColor/reserved; the shader still reads only row1). The page table/atlases
and layer table are World-level shared resources. Since S1 the catalog is built from a World-level
`TerrainLayerRegistry` (`source/terrain/terrainLayerRegistry.*`): layer identity is `layer.key`
(default `"<assetPath>#<name>"`), identical keys with identical parameters dedupe to one globalId,
conflicting keys / duplicate keys inside one TR / more than 256 global layers fail at load. Assignment
follows scene-object order × first appearance, so a single-TR World keeps `globalId == local index`
and its catalog/ID images are byte-identical. `TerrainWeightMapBuilder` takes the placement's
local→global map and writes globalId into the ID image and candidate lists; `TerrainPageManager`
stores that map per registration. Each MI's `TR.layers.size()` is checked against its own bound
arrays. S2 replaces the single source-texture triple with
`TerrainLayerSourceSets = map<sourceSet, TerrainSourceSetRecord>` (`source/terrain/terrainLayerSources.h`):
one set must always point at the same albedo/normal/pbr shared_ptrs, different sets may bind different
arrays (and different `sourceResolution`), uploads pick the record by `layer.sourceSet` and the source
slice through `layerSlices` (globalId → slice), and all sets still share one atlas so their three
formats must match. `TerrainPageManager` has CPU-side resident/eviction/LRU/priority/refcount APIs.
`terrainlayerstats` prints `globalLayers / sourceSets / dedupedLayers / capacity`. Multi-biome mode 0
still indexes each MI's `sampler2DArray` by globalId, so mode 0 arrays must be laid out per globalId;
mode 1 is not affected.
Registration is keyed by `placementId`, not `assetPath`; all 32×32 physical atlas slots (1024) are
allocatable — a missing page renders a neutral surface (baseColor 0.18, roughness 0.9, geometry
normal) directly in the shader, so no fallback page is reserved and a 32×32 single terrain
registers. The World raises the resident capacity to cover every registered page (capped at 1024).
`RegisterTerrain(preload=true)` records `preloadDeferredCount` and keeps going when capacity
cannot evict. The renderer creates zero page table/atlas textures, then `BuildUploadBatch` /
`RecordTerrainPageUploads` records the page-table and atlas subregion copies into one command
buffer and commits dirty only after successful upload. Production streaming uses
`RegisterTerrain(preload=false)`: each placement
prefetches only a 2×2 center set, then `TerrainPageManager::ApplyFeedback` consumes the dedicated
128×128 `terrainFeedback` pass (color + 128×128 depth, so only the nearest page wins) through a
host-visible, frame-fence-gated readback ring
(`source/terrain/terrainFeedback.*`, `source/render/resource/terrainFeedbackGpu.*`). Feedback hits
advance `lastSeenEpoch` and `lastTouchedEpoch`; misses enter a cross-frame request queue (priority desc, deferred
rotation), at most 8 `RequestPageAtEpoch` calls per frame. `TerrainUploadGpuRing`
(`source/render/resource/rendererTerrainLoader.*`) has one preallocated staging slot per frame
fence; at most 8 atlas pages are recorded into the main frame command buffer before scene passes,
and `TerrainUploadTransaction` commits only after `SubmitFrame` succeeds. Eviction no longer clears
the atlas: the physical slot is quarantined until
`max(lastSeenEpoch, lastSubmittedEpoch) + framesInFlight + 1 <= completedEpoch`, then
`ReclaimExpiredQuarantines` returns it to the immediately-allocatable pool. The effective CPU
eviction window is `max(2, framesInFlight + 1)`. The default resident budget is 512 because the
measured 32×32 wide-view working set is about 415 pages. `terrain stats` reports residency/eviction/upload
counters (`requestHits` vs `feedbackHits` are distinct) and `terrain capacity N` queues a budget
change consumed at frame start; lowering it only evicts pages outside the safety window and may
temporarily stay over budget. Only terrain with `TR.layers` and a `layerTable` material participates
in feedback; layerless M_pbr/M_unlit terrain neither registers virtual pages nor drives residency.
Current limits: no dedicated low-resolution fallback page or temporal fade (the L4e path only
blends a coarser mip toward `avgColor`), no frustum/tile-level prefetch (only forward-motion
prefetch), and no terrain editing or per-chunk/edit-time collision rebuild. Runtime chunk
streaming does unload out-of-window geometry.
CPU geometry LOD/stitching generation, load-time chunk residency, runtime streaming and LOD
selection are wired: with `lodLevels>1` (`TerrainMeshBuilder::BuildChunks(plan, lodLevel,
filter)`, TR `lodLevels`/`lodDistances`/`chunkResidencyDistances`) the loader computes chunk
AABBs from the heightmap (`ComputeChunkBounds`), keeps only the (chunk, LOD) pairs inside the
residency window (`TerrainChunkResidencyDistancesCm`, default `lodDistances × 1.1`, coarsest
always resident) and generates those meshes. LOD>0 uses boundary stitching, with the boundary
ring at `step=1` normals and coarse vertices blended toward them near the edge; there is no
skirt. The load-time window comes from the scene JSON camera; `TerrainChunkStreamer` then
consumes the render camera every frame, builds missing chunks on a worker thread, creates their
GPU objects on the game thread, and retires chunks absent for two frames. `WorldSnapshotBuilder`
picks one level per
`lodGroupId = placementId|chunkX|chunkZ` from the camera's distance to the chunk world AABB with a
10% hysteresis band; if the target level was never resident it falls back to the nearest coarser
resident level (worst case coarsest) instead of leaving a hole. The snapshot is the single
source for color, shadow, picking and MaterialInstance context; all levels of a group share one
RuntimeId. `terrainlod stats` reports per-group level/distance/switch count and
`terrainchunk stats` reports load-time geometry bytes, created/skipped chunks, per-level
vertices/indices and the current frame's draws.
`lodLevels=1` old assets still generate and bind LOD0 only, byte-identical to the legacy path.
`source/terrain/terrainCollision.*` turns a `TerrainBuildPlan` plus placement into a static
Jolt `HeightFieldShape` body in a caller-owned `PhysicsSystem` (cm→m, 16-bit height compression,
Y*X*Z degrees rotation); `ScriptPhysicsWorld::SetTerrainCollisions` owns those RAII bodies and
`ScriptRuntimeSubsystem` feeds the World's instances on every Play (cm/degree/scale validation
happens at World build). `terraincollision stats` prints the Play-mode body count and placements;
`source/physics/joltRegistration.*` is the shared process-wide Jolt registration, and
`TerrainCollisionWorld` is only a test/offline fixture. `TerrainLayerPageManager`
(`source/terrain/terrainLayerPageManager.*`) has an L2 GPU side: a 256×4 catalog, one
RGBA32F page table per mip (width capped at 4096), and three 4096²/slice `sampler2DArray`
atlases (no mip) are created World-locally and the per-layer tail pages are copied GPU→GPU
at load. Page-table texels are `R=physicalPage/G=mip/B=valid/A=priority`; eviction emits an
all-zero invalidation in the same upload batch. BC atlases cannot use `vkCmdClearColorImage`,
so they are zeroed with a 16 KiB zero-page staging copy. L3 adds the optional shader path:
`M_terrain` keeps `u_terrainLayerMode=0` sampling `layerAlbedo/Normal/Pbr` directly
(byte-identical legacy path), while mode 1 resolves top-4 IDs through the catalog, per-mip
page table (stride = page-table texel count / 256), physical page, and 4096² atlas with
half-texel page inset; invalid entries use catalog row3 `avgColor`, never layer 0.
`sourceResolution` must match the real square source extent; mode 0 only warns, mode 1
fails at load. L4a drives mode 1 residency from the existing 128×128 coverage-page feedback:
`TerrainLayerVisibilityResolver` (`source/terrain/terrainLayerVisibility.*`) turns each visible
cover page's top-4 layers into (layer, mip, that mip's full page set), limited to 16
`RequestPageAtEpoch` calls and 8 atlas-page uploads per frame through
`TerrainLayerUploadGpuRing`. Only pages that the current frame's feedback actually hit are refreshed:
pages of a visible layer that are not candidates use a short 16-epoch idle window, so residency
tracks the visible working set instead of every historical mip; pages that leave view are evicted
after a 180-epoch idle window plus the GPU safety window. `MipForPriority` has seven levels
(`>=192/>=128/>=64/>=32/>=16/>=8/else` → mip0..6) so 2K mip4 and 4K mip6 are reachable, and `TerrainLayerDesc::avgColor`
defaults to neutral 0.18 instead of black. L4b adds a precise per-slot layer feedback on the same
pass: four fixed 128×128 `R8G8B8A8_UNORM` attachments (`terrainLayerFeedback0..3`) encode
`R=layerId, G=pageIndex, B=mip|valid, A=frameTag`, sharing the cover RT's D32 depth. The pass now
takes 4 `worldTexture` inputs (page table, layer catalog, layer IDs, layer weights) and writes 5
color outputs; `TerrainLayerFeedbackGpuRing` reads the four images back into one 64 KiB ring slot per
frame, `SummarizeTerrainLayerFeedback` dedupes/stales them, and `ApplyExact` drives
`RequestPageAtEpoch`/`MarkPageUsedAtEpoch`; layer UV/mip/page math lives in
`shader/glsl/materialFunction/terrainLayerVTMath.glsl` and is shared by `mf_terrainLayerVT.glsl`
and `pass/terrainFeedback.frag`. Exact feedback is the primary driver; L4a derivation is only the
startup/backpressure fallback. `terrainlayerstats` reports layer residency/atlas format/bytes plus
visibleLayers, `feedbackHits`, `exactSlots`/`exactPages`, `requested`/`accepted`, uploads, pending and
the mip request distribution; `terrainlayer capacity N` and `terrainlayer preload` (full-preload
comparison, disables idle eviction) are console-only.
L4c adds coarser-mip prefetch on the same resolver: a missing target page also queues the
`+1`/`+2` mip pages of the same UV region (`pageX >> level`), priority ×3/4 and ×1/2, clamped by the
layer's maximum mip and the per-frame request budget; pages that are already resident do not expand.
`terrainlayer stats` adds `fallbackSlots`, `fallbackPages`, `fallbackLevels` and `targetMips`. Shader-side
`avgColor` fade-in is still **not** implemented — a missing target mip still hard-switches to the coarser
page, or to `avgColor` when no coarser page exists.
L4d adds starvation protection: when the exact layer feedback has fewer than
`minValidFeedbackTexels` (default 1) valid texels — camera inside/clipped by terrain, or all
cover pages missing — the resolver only bumps `starvedFrames`, never touches
`lastSeenEpoch`/`lastTouchedEpoch`, and adds `starvationCreditEpochs`
(`min(starvedFrames, starvationGraceFrames)`, default grace 480) to the idle window so the
resident set is preserved. `ApplyTerrainLayerExactFeedback` then returns false so
`RenderSystem` runs the L4a cover-page derivation in the same frame; recovery clears
`starvedFrames`, advances `lastValidFeedbackEpoch` and decays the credit by
`starvationCreditDecayPerFrame` (default 1). Past the grace window the credit is capped and the
resident set decays at `starvationEvictionPagesPerFrame` (default 2) pages/frame, so a long
feedback outage never locks memory. `terrainlayerstats` prints `starvedFrames` /
`lastValidEpoch` / `usedCoverFallback` / `idleGrace`.
L4e adds `u_layerForceMip` (-1 auto, 0..4 console override via
`terrainlayer forcemip N`) on both `M_terrain` and `M_terrainFeedback`; the shader and
feedback share `TerrainLayerTargetMip` in `terrainLayerVTMath.glsl`. When the sampled
`usedMip` is coarser than the target, `TerrainLayerFallbackBlend` mixes albedo toward catalog
`avgColor`, pbr toward the neutral value and normal toward the geometric normal at
`0.2 × mip-gap` (cap 0.6, gap 0 unchanged). Camera forward motion above 5 cm/frame makes
`TerrainLayerVisibilityResolver` prefetch the four finer-mip children of resident visible
pages, at most 4 requests/frame, outside the feedback request budget but still through
`RequestPageAtEpoch`. `terrainlayer stats` additionally prints `forceMip` / `prefetchMoving` /
`prefetchPages` / `prefetchRequests` / `prefetchAccepted` / `fallbackBlendPages` /
`avgColorPages`.
CPU page baking is still
synchronous inside `RequestPageAtEpoch`. Editor thumbnails do not render terrain. The contract is
`documents/contracts/terrain.md`.

If adding a new type, update both loader logic and documentation.
The historical `SC_scene01` `sunLight` object was migrated to
`directionalLight` with `intensityUnit: "lux"`; new scenes must not author
the rejected legacy `sunLight` type.

## Key Runtime Couplings

These relationships are important and easy to miss:

- `config/config.json -> initScene` decides which scene file is loaded first.
- Runtime resource lookup resolves relative asset paths from `config/config.json -> resourcePath`.
- `config/renderGraphConfig.json` defines render resources and pass order before scene loading.
- Passes with `needCreateMaterial: true` rely on material instance JSON to create pass materials.
- Material parameter validation depends on shader reflection results.
- Pass input textures depend on descriptor set and binding conventions.
- `shader/glsl/` is the editable shader source; `shader/spv/` is generated output used at runtime and for reflection.
- Shader cache validity is owned by `shader/spv/shader-build-cache.json`; the manifest is the final commit marker and must never be edited by hand.
- Hot reload candidates are compiled as in-memory artifacts on the compile worker and only published after CPU/ABI/Vulkan validation succeeds; the manifest stays at the last committed generation on failure.
- Shader publication preflight is batch-wide and uses normalized physical paths for cache hits, cache misses, and generated includes. Cache-hit outputs must be rehashed immediately before any write; a path conflict or stale hit rejects the batch with zero publication side effects.
- World/M_ reload prepares an isolated World-local resource package and `PreparedRenderGraphState`. The active World, graph, RenderSystem, Controller, and resource cache change only through prevalidated no-throw ownership swaps; prepared retirements become active only after the live swap succeeds.

## High-Risk Areas

Be careful when changing these files or systems:

- `source/commonFunction.h`
  - contains path resolution, config loading, math helpers, Vulkan utility helpers, and other mixed responsibilities
- `source/renderGraph.cpp`
  - render pass attachment ordering, MSAA resolve behavior, framebuffer construction
- `source/scene/validation/sceneAssetValidator.cpp` and `source/world/loading/worldLoader.cpp` / `worldBuilder.cpp`
  - scene JSON contract, per-object field validation, world build plan (the scene-data contract lives here)
- `source/terrain/**` and `source/render/resource/rendererTerrainLoader.*`
  - TR/heightmap preflight, vertex/index winding, chunk bounds/normals, and chunk resource keys
- `source/renderSystem.cpp`
  - frame rendering flow and descriptor usage
- `source/shaderCompiler.cpp` and `source/shader/build/*`
  - cache-hit ordering, dependency digest validation, atomic output replacement, manifest commit/rollback
- `source/shader/reload/*`
  - reload plan capture, worker/GT boundary, ABI validation, render-thread safe commit, descriptor package rebuild, epoch retirement
- `source/render/environment/*` (compute participants) and `source/ui/uiOverlayRendererVulkan.*`
  - compute/UI reload participants must keep descriptor rebuild and pipeline swap transactional
- `config/renderGraphConfig.json`
  - small ordering mistakes can break attachments or descriptor assumptions

## Current Known Fragility

The following issues are already present in the codebase and should be treated as existing constraints unless you are explicitly fixing them:

- `config/config.json` uses a sibling resource directory; launch from the checkout root and isolate resource writes when testing branches.
- Path resolution is partly inferred from current working directory.
- Some naming is inconsistent, for example `Viking/Vilking` and some include capitalization.
- `shader/spv/` contains generated artifacts, but the repo also relies on them for reflection/debug workflows.
- Several conventions are implicit in code rather than enforced by schema or tests.

Do not “clean up” these areas casually. If you touch them, explain the behavioral impact.

## Editing Guidance

When making changes:

- Prefer source-of-truth files over generated outputs.
- For shader work, edit `shader/glsl/` first.
- For shader identity/cache work, use only the stable BLAKE3-256 wrappers; never `std::hash`, timestamps, or truncated digests as persistent identity. Change the ABI description format only by bumping the `abi=` compile-policy version.
- Do not bypass the shader build cache with per-frame fallbacks, global `device.waitIdle()`, or by deleting `shader/spv` as a fix; recompiles must be driven by the documented miss reasons.
- For shader resource declarations such as `sampler2D emissionMap`, do not add `#if defined(...)` guards by default. Prefer relying on the compiler to optimize out unused resources unless the active reflection/toolchain path is explicitly verified to require guarded declarations.
- Only edit `shader/spv/` directly if the task is specifically about generated artifacts or debug outputs.
- Keep JSON structure and naming stable unless the task is a format migration.
- Avoid broad refactors unless the task clearly calls for them.
- Preserve learning-oriented readability over over-abstraction.
- Generally avoid lambda expressions in engine code. In long flow-heavy files such as `source/renderSystem.cpp`, prefer small named helper functions or clearly scoped private methods so the control flow stays readable and code size does not balloon.
- Add concise Chinese comments for non-obvious engine, rendering, and shader code, especially around data ownership, frozen snapshots, dirty state, Vulkan synchronization, resource lifetime, coordinate spaces, texture-channel semantics, mathematical approximations, and intentional performance tradeoffs. Comments should explain design intent rather than restate the code; ordinary API/type names may remain in English, but new or modified code must not leave important design boundaries undocumented.
- Shader/code migration must begin by reading and understanding the source implementation's comments together with the surrounding behavior. Preserve the original comments' useful intent in concise Chinese comments near the migrated code, including formula meaning, coordinate/UV conventions, branch rationale, source limitations, and deliberately retained quirks. Do not silently discard meaningful source comments merely because names or structure changed.
- Do not mechanically translate or copy stale source comments. Verify each comment against the migrated behavior, rewrite it for VulkanLearn's ownership boundaries, and explicitly document intentional differences from the source implementation. Obvious syntax does not need comments, but non-obvious migration decisions do.
- **Name shader math variables the way the source paper names them.** In particular the microfacet parameter
  is always `alpha` (`float alpha = roughness * roughness;` — the paper's α = Roughness²), never `a`/`a2`:
  a bare `a` meant α in one place and perceptual roughness in another, which produced a phantom
  "formula that is not in the paper" during the `D-21` audit. Keep symbols that the source itself defines as
  `a`..`e` (e.g. the Narkowicz ACES fit coefficients in `pass/toneMapping.frag`) unchanged — the rule is
  "match the source symbol", not "never use the letter a".

## Commit Titles

- Use the repository title format `【Type】short English description`, with no space after `】`.
- Choose the prefix by intent: `【Fix】` for bug/performance fixes, `【Mod】` for feature or behavior changes, and `【Doc】` for documentation-only changes.
- Check recent `git log` titles before committing. Do not omit the type prefix or substitute another title convention.

## Test Governance

Follow `documents/contracts/coding-guidelines.md#test-governance` for the
complete test policy. The non-negotiable repository rules are:

- Module and pure-logic tests must use GoogleTest and be registered through
  the root `vulkanlearn_add_gtest(...)` helper; do not add test-only `main()`
  entry points or manual `add_test(...)` registrations.
- Keep each `TEST` focused on one behavior or contract. Extend the nearest
  existing module suite instead of growing generic catch-all test files.
- The only supported Vulkan runtime test commands are `--shader-reload-test`,
  `--shader-compute-reload-test`, and `--world-graph-transaction-test`.
  Do not add, restore, or broaden runtime tests without explicit user
  agreement and a documented reason that a module test is insufficient.
- Runtime tests share `shader/spv/` and must run serially. They are final
  end-to-end validation, not a substitute for module coverage.
- Paper-case pixel/energy validation is **not** a runtime test: it runs outside
  the engine through `tool/validation/` (see the shading-model alignment plan
  §1.5.3) and must not be wired into ctest or the runtime test flags.

## Common Task Map

If asked to modify an existing Scene, Mesh or Texture descriptor:

- read `documents/contracts/asset-modify-api.md`
- use `build/bin/vl_asset_edit.exe` JSON Lines or the running Console `editor <JSON>` route
- read get_document/get_capabilities first; modify with the returned revision, validate, then explicitly save
- keep the lifecycle in one host session; separate processes do not share unsaved drafts
- standard modify stages author data; runtime application goes through formal loading
- retain direct source edits for changes outside the capability table, with the corresponding loader/schema review

If asked to add or change a scene:

- check `config/config.json`
- inspect `<resourcePath>/Maps/SC_<scene-name>/*.json`
- inspect `source/scene/validation/sceneAssetValidator.cpp` (required fields, including `description`)
- remember every scene JSON must carry a non-empty `description` saying what it is for and which paper /
  book knowledge point it validates

If asked to add or change a terrain:

- read `documents/contracts/terrain.md` first
- inspect `<resourcePath>/Maps/SC_<scene-name>/Terrains/TR_*.json`
- inspect `source/terrain/validation/terrainAssetValidator.h`, `terrainAssetLoader.*`,
  `terrainHeightmap.*` and `terrainMeshBuilder.*`
- inspect `source/render/resource/rendererTerrainLoader.*` and
  `source/render/resource/rendererResourceLoadCoordinator.cpp`
- inspect `source/scene/validation/sceneJsonValidator.h` and `sceneAssetValidator.cpp`

Heightmap/TR/geometry changes are runtime changes; World-level collision integration, terrain
editing and per-chunk collision rebuild are still feature requests, not fixes. CPU LOD/stitching
generation, runtime chunk streaming, runtime LOD selection (`WorldSnapshotBuilder` + `terrainlod stats`) and the standalone
Jolt heightfield collision module already exist; treat bugs in those paths as fixes.

If asked to add or change a render pass:

- inspect `config/renderGraphConfig.json`
- inspect `source/renderGraph.cpp`
- inspect `<resourcePath>/Common/Materials/Pass/*.json`
- inspect corresponding shaders in `shader/glsl/pass/`

If asked to add or change a material:

- inspect `<resourcePath>/Maps/SC_<scene-name>/Materials/*.json`
- inspect `source/material.cpp`
- inspect `source/materialInstance.cpp`
- inspect shader reflection-dependent code paths

If asked to debug startup failures:

- inspect `source/main.cpp`
- inspect `source/commonFunction.h`
- inspect `config/config.json`
- confirm runtime working directory assumptions

If asked to add or change shader build-cache/hot-reload behavior:

- inspect `documents/contracts/shader-build-cache.md` and `shader-hot-reload.md`
- inspect `source/shaderCompiler.*`, `source/shader/build/*`, `source/shader/reload/*`
- respect the GT/worker split: compile workers must not touch Vulkan or live Material/PipelineFactory caches
- respect the staleness protocol: `latestObservedSourceEpoch` is a fast rejection signal, stable source identities are unioned while pending, and every candidate still requires commit-time primary/dependency digest validation; generation alone is not proof that source bytes are current
- keep Material Surface/Shadow, Compute descriptor packages, and UI blend-variant pairs transactional (all-or-nothing) with GPU-epoch retirement
- keep World/RenderGraph prepare isolated from active owners, make the final ownership commit no-throw, and activate prepared retirement packages only after the live references have been swapped
- runtime validation tests must run serially because they share `shader/spv/`

## Preferred Agent Behavior

When working in this repo:

- Start by understanding whether the change is engine code, render graph data, scene data, material data, or shader data.
- Trace the runtime path from JSON/config to loader to runtime object creation.
- Call out hidden assumptions explicitly in your final response.
- Prefer concrete fixes over vague architecture advice.
- If a requested change exposes an implicit convention, document that convention nearby.
- **Data correctness is guaranteed at the source**: Do not write redundant defensive code (like per-frame `clamp()` or `max()` bounds checking in shaders) if the upstream configuration (e.g., JSON materials, configuration files) is responsible for providing valid data. Trust the data source and keep runtime execution clean.

## Good First Documentation Follow-Ups

These are useful future improvements, but they do not need to block normal tasks:

- add JSON schema files for scene, material instance, and render graph formats
- replace absolute `projectPath` with project-root discovery or environment override
- split `source/commonFunction.h` by responsibility
- add a startup flow doc and a project structure doc

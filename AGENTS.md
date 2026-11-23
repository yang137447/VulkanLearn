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

- Runtime/editor target split: `main_editor`/`vl_editor`/`vl_editor_asset_core`
  over `vl_runtime`, and runtime-only `main_game`; `source/CMakeLists.txt`
  filters editor sources from `vl_runtime` and rejects editor includes at
  configure time. See `documents/contracts/vulkanlearn-architecture.html`.
- Optional Windows x64/.NET 11 C# Blueprint hosting, scene placement and Play/Pause/Stop;
  implemented subset and limits: `documents/contracts/csharp-blueprint-runtime.md`.
  The UE Third Person Template replication lives in `Maps/SC_third_person` (native pose
  graph runtime, State Alias and 1D/2D BlendSpace); it needs the C# build and `--autoplay`.
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
- GPU particle system: data-driven `PS_*.json` v1/v2 module stacks, compute simulation,
  billboard/ribbon/mesh/beam/distortion rendering, GPU-count indirect draws, global alpha
  sorting, events, runtime emitter/budget/user-parameter APIs and telemetry, plus
  `renderMode: "light"` GPU per-particle point lights (reuse the simulate pool, dedicated
  `particleLightEmit` compute, fixed 64-slot per-image SSBO, set 0 binding 4) and
  `renderMode: "decal"` CPU particle decals (shared fixed-step/burst simulation, up to 64
  total visible DBuffer decals); current schema, ownership and limits:
  `documents/contracts/particle-system.md`
- Particle graphics now opt into full scene lighting: billboard/ribbon receive all
  directional/point/spot lights plus GPU particle lights and IBL through a particle-owned
  set 0 binding 6..9 contract (wrapped Lambert, `billboardLightingStrength` blend),
  while mesh particles use the complete DefaultLit path. Transparent ordering uses the
  authoring `sortOrder` layer, and the global alpha pool is one draw item inside the same
  depth order instead of being drawn last.
- Decals now write a six-channel DBuffer including a premultiplied `dbufferEmissive`
  (additive after lighting, feeds Bloom). Mesh particle materials read albedo/normal/ORM/
  emissive textures through a dedicated set 1. Mesh, billboard and ribbon particles can
  cast CSM and local-light shadows (`castShadows`, masked alpha for sprites). Local
  point/spot shadows use one octahedral slice per light in a `D32` texture array with up
  to eight selected lights, per-light caster culling and transactional shader reload.
  `environment.fog` drives the quarter-resolution 32-layer volumetric fog/light-shaft
  pass before Bloom.
- BeforeLighting DBuffer decals: data-driven scene `decal` objects, unit-cube projection,
  a dedicated `decalStencil` D24S8 pass (unit-cube bit0 union mask, stencil gate on the
  decals pass; sceneDepth stays a sampled input so there is no feedback loop), sceneDepth
  reconstruction, stable sort instances, DBuffer A/B/C/D + Mask composition,
  DecalResponse channel masks, sampled albedo/normal/ORM textures with procedural FBM
  fallback and GPU-side per-decal material descriptors (`albedoMap`/`normalMap`/`ormMap`,
  no Material Graph); current scope and limits: `documents/contracts/decal-runtime.md`
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
composition passes, and the LUT sampler slots that used to live at set 3 bindings 10..13. Those bindings are
now occupied by the decal DBuffer inputs (10..14), so a rebuilt LUT model must allocate free slots instead.
`deferredLighting` therefore writes `sceneColor` directly
again (single output, `loadOp=clear`) and `VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT` is now a compile-time
`#error`. Re-adding a deferred model that needs a LUT means adding the graph `input`, the pass material
texture slot and the shader sampler declaration together, keeping binding order equal to the graph input order.

The codebase mixes engine experimentation and learning-oriented iteration. Prefer small, explicit, reversible changes.

## Read Order

When onboarding to the repo, read in this order:

1. `README.md`
2. `source/main_editor.cpp`, `source/main_game.cpp` and `source/engine/application.cpp`
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
cmake --build build --target main_editor main_game main
```

Executable output:

- `build/bin/main_editor.exe`: editor composition root, links `vl_editor` + `vl_runtime`
- `build/bin/main_game.exe`: runtime-only composition root, links `vl_runtime` only
- `build/bin/main.exe`: POST_BUILD copy of `main_editor.exe` for existing scripts

Target graph and options:

- `main_editor -> vl_editor -> vl_runtime`; `vl_editor` also links
  `vl_editor_asset_core` and `vl_editor_widgets`
- `main_game -> vl_runtime`; no `vl_runtime -> vl_editor` edge
- `VULKANLEARN_ENABLE_EDITOR=OFF` builds only the game flavor
  (`main_game`) and drops editor targets from the graph
- `VULKANLEARN_BUILD_GAME=OFF` builds only editor targets
- `VULKANLEARN_BUILD_LEGACY_MAIN=OFF` suppresses the `main.exe` copy
- both flavor options `OFF` fails configure; at least one executable is required
- editor sources stay under `source/editor/**`, `source/render/editor/**` and
  editor-only engine/widget files but are compiled only by editor targets;
  configure reads every `vl_runtime` source and rejects editor includes

Runtime smoke:

```powershell
build\bin\main_editor.exe --initial-scene Maps/SC_fracture_playground/SC_fracture_playground.json --exit-after-frames 60
build\bin\main_game.exe --initial-scene Maps/SC_fracture_playground/SC_fracture_playground.json --exit-after-frames 120 --worker-thread-count 2 --no-dev-ui
```

`--exit-after-frames <N>` exits with code 0 after `N` completed `Tick()` calls.
`--frame-limit` is a 1..1000 Hz pacing cap, not a frame count. The console
`quit` command requests the same clean shutdown path. `--autoplay` starts
blueprint scenes in Play for this process only (it overrides
`config/config.json -> scripts.autoPlay` without writing the file), which is
how AI runs a playable map headlessly.

Game-flavor packaging requires a Release game cache and the game binary:

```powershell
cmake --build build-game --target main_game vl_export_game
build-game\bin\main_game.exe --shader-force-rebuild --exit-after-frames 2
build-game\bin\vl_export_game.exe --project-root . --flavor game `
  --initial-scene Maps/SC_fracture_playground/SC_fracture_playground.json `
  --output artifacts/game-package
powershell -NoProfile -ExecutionPolicy Bypass -File tool\validate-game-package.ps1 `
  -PackagePath artifacts/game-package -VerifyTool build-game\bin\vl_export_game.exe
```

The exporter rejects `main.exe`, Debug shader caches and missing
`vl_build_flavor.json`; it copies the runtime UI manifest closure, all cached
SPV output roles (including debug reflection), MI-referenced `M_*.json` and
`shaderEvaluation` sources, and the gameplay default `MI_glass_crack.json`.

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

The current startup sequence is split between the entry point,
`source/engine/application.cpp` and `source/engine/engineLoop.cpp`:

1. `main_editor.cpp` or `main_game.cpp` calls `VL::RunApplication` with an
   `IEditorHost` factory (`EditorHost` or `NullEditorHost`)
2. Parse launch options and runtime test flags in `source/engine/launchOptions.cpp`
3. Load `RuntimeConfig`
   - discover project root through `FileSystem`
   - read `config/config.json`
   - read `config/renderGraphConfig.json`
   - cache startup fields such as window size, `initScene`, `resourcePath`, and worker thread mode
4. Initialize `PlatformApplication`
   - initialize SDL and the Vulkan loader-facing platform layer
   - create the SDL Vulkan window and collect required Vulkan extensions
5. Initialize `EngineLoop`
   - store the injected editor host; a null factory falls back to `NullEditorHost`
   - build the runtime render graph from `config/renderGraphConfig.json` and
     remove the `editorGrid` pass when `IEditorHost::IsEditorFlavor()` is false
   - initialize input
   - generate material parameter includes
   - compile shaders from `shader/glsl/` into `shader/spv/`
   - initialize the Vulkan renderer backend
   - create `PipelineFactory` and bind renderer resource loading services
   - load the render graph from `RuntimeConfig::GetRenderGraphJson()`
   - initialize the `RenderSystem` resources required by World/Graph candidate preparation
   - load and publish the initial World through the same
     `WorldCandidatePreparer::Prepare()` +
     `WorldGraphTransactionCoordinator` transaction used by runtime replacement
   - finalize initial-only render objects such as the UI overlay after the
     World/Graph transaction commits
   - start the render thread only when `workerThreadCount == 2`; the render loop is
     bounded async, not a recording-only thread
   - initialize the console subsystem
   - initialize the editor host through `EditorHostInit`; the host attaches
     `EditorUiShell` to the single `UiSubsystem` context
6. Queue launch-time runtime commands, if any
7. Enter the update + render loop

If a change affects boot behavior, verify it against this order.

### Render Loop Concurrency

`workerThreadCount == 1` is fully synchronous. `workerThreadCount == 2` runs the
bounded async pipeline in `source/render/renderThread.*`:

- At most one frame may be outstanding (`RenderFrameBudget::kMaxFramesAhead = 1`).
  `SubmitFrame()` never waits; GT continues the rest of the tick and the next
  simulation step while RT records, then `WaitForRenderFrameSlot()` blocks before
  publishing the next snapshot if that frame has not finished. Do not add a
  per-frame `WaitForRenderThreadIdle()` after `SubmitFrame()`.
- The snapshot mailbox is latest-wins. If a pending snapshot is replaced, its
  `hairSimulationDeltaSeconds` accumulates into the replacement, so dropped frames
  cannot lose simulation time.
- Structural renderer mutations stay at explicit safe points: swapchain resize,
  render-graph reload, World/Graph transaction commit, shader reload, offline-mode
  switch, material-instance preview commit, blueprint asset commit, terrain
  paint/sculpt, and shutdown. Per-frame publishing and simulation must not call
  `device.waitIdle()`.
- `RendererResourceCache` getters, `ResourceRetireQueue`, `MaterialInstance`
  parameter state, the thumbnail registry, and the Vulkan submission gate are
  synchronized for concurrent GT/RT access. Resolved draws hold strong
  `RenderableObject` references; retirement epochs must cover
  published-but-not-yet-recorded snapshots.
- Soft-body and glass dynamic meshes do not write host-visible buffers directly
  from GT. They publish through `RenderSystem::QueueDynamicVertices` /
  `QueueDynamicMesh`, and `RenderableObject` keeps three frame slots so an
  in-flight frame never aliases the slot GT is writing.

## Important Directories

- `source/`: engine and renderer source
- `source/editor/**`, `source/render/editor/**`: editor-only sources; only
  `vl_editor` compiles them (CMake ownership, not physical relocation)
- `source/render/{frame,shadow,postprocess,environment,terrain,hair,picking,preview,world,renderable,resource/terrain}/`:
  `RenderSystem`/`RenderableObject`/terrain-loader production modules
- `source/engine/{frame,ui,script,window,perf}/`: `EngineLoop` production modules
- `source/editor/ui/panel/`, `source/editor/service/material/`: decomposed editor
  panel and material document service modules
- `source/pipeline/`: pipeline factory, builders, graphics and compute pipeline code
- `source/resource/`: image and device texture helpers
- `source/shader/`: BLAKE3 content hashing, atomic file/commit, build-cache manifest, ABI signature, FileMonitor, compile worker, reload coordinator, compute/UI participants
- `source/particles/`: `particleSystem` asset loading/validation, the GPU compute + billboard
  runtime, the GPU per-particle light output path, the shared CPU decal emitter simulation,
  and the retained CPU light reference runtime used only by module tests
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
- `tool/game-export/`, `tool/validate-game-package.ps1`: game-flavor closure,
  flavor marker validation and packaged `main_game.exe` smoke
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
- `SKM_*.json`: skeletal mesh descriptor files (mesh + `skinning.skeleton` / `skinning.clips`)
- `SK_*.json`: skeleton descriptor files
- `MI_*.json`: material instance files
- `scene*.json`: scene definitions

### Static ground authoring

- Large static surfaces (ground, plazas, platforms) must not be one mesh scaled up to cover the
  whole area: UVs and texel density then stretch with distance and the surface reads as blurry.
- When the model is too small, stitch it from multiple blocks at a sane per-block scale, with
  edges aligned. Physics may still be a single simplified box.
- Reference: `SC_valley_echo`'s floor visual is a 6x6 grid of 2000 cm `SM_cube` tiles while
  collision stays one 12000x100x12000 cm box. Its material instance
  (`Maps/SC_valley_echo/Materials/MI_echo_ground.json`) derives from the world-projected
  `M_gaspGrid` shader, so grid lines keep a fixed cm size instead of stretching with the mesh.

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
- `particleSystem` (`particlePath` references a `PS_*.json`; a PS file may contain up to 16
  `emitters[]`, each expanded into its own GPU emitter; current schema and limits are in
  `documents/contracts/particle-system.md`)
- `decal` (BeforeLighting DBuffer decal; current scope and limits are in
  `documents/contracts/decal-runtime.md`; unit-cube projection, sceneDepth
  reconstruction, DBuffer A/B/C/D + Mask composition, DecalResponse channel
  masks, sampled albedo/normal/ORM textures (procedural FBM fallback) and
  per-decal GPU material descriptors (`albedoMap`/`normalMap`/`ormMap`, no
  Material Graph) are connected; stencil and non-deferred receivers are not)
- `terrain` (TR heightmap mesh; CPU chunks rendered through the geometry pass)
- `wind` (scene-global wind, at most one per scene; `direction`/`strength`/optional `gust`;
  no position/rotation; see `documents/contracts/scene-wind.md`)
- `balloon` (runtime rubber soft body; current scope and limits are in `documents/contracts/softbody-runtime.md`; Jolt impact, convex-hull shards, shard-shard contact, neck anchoring, approximate Jolt-pose coupling and asset-backed audio with procedural fallback are connected; advanced spatial audio/material are not)
- `glassPane` (runtime glass plate; current scope and limits are in `documents/contracts/glass-fracture-runtime.md`; impact-point radial fracture, Jolt convex-hull shards, attached plates, support islands, CCD projectiles, landing secondary fracture and asset-backed glass audio with procedural fallback are connected; transparent material is not)

`terrain` requires `terrainPath` and is preflighted by `TerrainAssetLoader::Prepare`
before a World candidate is committed. Each placement has `placementId = terrain|<name>`;
the same TR can be placed multiple times with distinct names. Internal callers with an empty
placementId fall back to assetPath. Scene-object `pageOrigin` / `pageTilesPerSide` override
that placement's coverage region, not the composited material resolution. The renderer expands
terrain into chunk `MeshObjectBuildPlan`s; World rendering still uses ordinary mesh objects.

The only terrain material runtime standard is **compositedVT + resident tiling height**.
TR assets must explicitly declare `compositedVT.enabled=true` and a resource-relative
`packagePath` shared by all terrains in one World; Editor/Cook additionally requires a shared
`productionSourcePath` author recipe. Current Store is sparse V2, generator3, raw RGBA8;
V1 fallback and TR `residentPages`/`heightBudgetBytes` are rejected. The World pool and
cross-World Candidate/Active/Retiring admission use the unchanged P0Frozen hash and caps.
Rendering requires validated real production inputs, not dependency hashes alone. Cook, dependency identity,
material-page residency/feedback, height atlas/LUT, budgets and publication rules belong to
[the composited VT contract](documents/contracts/terrain-composited-vt.md).
The default scene in `config/config.json` is
`Maps/SC_lookdev_article/SC_lookdev_article.json` (the neutral lookdev article
composition: colour calibrator mesh, lookdev camera and the neutral studio HDRI).
The RVTv2 V2 Full16 CoverageAtlas publication lives at
`Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_CoverageAtlas/SC_composited_vt_4x4.json`
under the gitignored resource `Generated/` tree and is selected with
`--initial-scene`; a fresh resources clone does not contain it until that
publication is produced locally. Production/unattended runs of a blueprint scene
add `--autoplay` to enter Play without pressing the editor button.
The standard terrain map is `Maps/SC_pegasus_rvt/SC_pegasus_rvt.json`: an authored
scene plus 16 `Terrains/TR_pegasus_rvt_tile_*.json`, with cook inputs under
`Author/` (tile heightmaps, paint sidecars and `world.author.json`), shared layer
texture descriptors/sources, material instances and the water mesh inside the same
map directory. Its cooked package is the gitignored
`VT/worldV2/SC_pegasus_rvt/`; rebuild it with
`main_editor.exe --terrain-vt-cook --initial-scene Maps/SC_pegasus_rvt/SC_pegasus_rvt.json --exit-after-frames 2`.
`Generated/Validation/TerrainCompositedVT/Variants/...` is validation evidence, not
the map source. `Maps/SC_pegasus_rvt/SC_pegasus_rvt_play.json` is the playable variant:
the same terrain plus the UE GASP Echo character on the old tile-05 spawn, with
`runtimeMode=gameplay`; it needs a C#-enabled build and `--autoplay` to enter Play.
Default selection is not proof of complete runtime/package acceptance; check the scoped evidence
linked from [the acceptance entry](documents/reference/rendering/terrain-composited-vt-acceptance.md).
Gates that were explicitly deferred rather than passed (full P4 editing, authoring-mode fault
consumption, per-class memory attribution, per-phase independent sign-off, R17 re-export and
V1 artifact removal) are listed in the deferral section of
`documents/rationale/terrain-rvt-production-execution.md`. The active producer uses World + WorldPadding compute;
the old five compute stages and depth-replay capture pass are deleted. Quality imports the
original feedback pass's 7-MRT winner payload (two 128² RGBA32_UINT images) plus final depth,
not a second visibility replay. Complete captures are module evidence, not interactive acceptance.
RawAuthorSource kind3 binds verified author bytes separately from author projection digests;
it does not become generator input or a base-page hash dependency.
shader files, quality interfaces, root-ready, Cook and far-view results are not final evidence.
Keep the P3 Repeat-129 padding counterexample distinct from Clamp support proofs, and bind
final claims to the actual binary, generator source/artifact hashes and independent audit.

`TerrainLayerRegistry` assigns World-global layer IDs in scene/first-appearance order.
Layer identity is `layer.key` (default `<assetPath>#<name>`); identical keys and parameters
deduplicate, while conflicts, duplicate keys within a TR and more than 256 global layers fail.
Coverage uses the placement's local-to-global mapping. Layer catalog, sourceSet/slice mappings,
Paint, coverage page table and ID/weight inputs remain dependencies for compositing, near-field
detail, TES or editing; they must not be removed merely because the former layer-source streaming
path was retired. They do not provide an alternative base-material runtime.

CPU geometry remains heightmap-based. `TerrainMeshBuilder` generates chunk LODs and boundary
stitching without skirts; the shared boundary uses LOD0 samples/normals. Load-time residency
uses chunk world AABBs and `chunkResidencyDistances` (default `lodDistances * 1.1`),
with the coarsest level always resident. `TerrainChunkStreamer` builds missing CPU chunks on
a worker, uploads on GT at safe boundaries and retires out-of-window GPU resources by epoch.
`WorldSnapshotBuilder` selects one resident level per `placementId|chunkX|chunkZ` using
camera-to-AABB distance and 10% hysteresis, falling back to a coarser resident level.
Color, shadow, picking and material context consume the same snapshot; levels share a RuntimeId.
`terrainlod stats` / `terrainchunk stats` expose selection and geometry residency.

Landscape Paint/Sculpt retain authoring sessions and immutable World snapshots. Sculpt updates
macro geometry/bounds through the composited sculpt commit and safe vertex-update queue;
Paint refreshes coverage/composited author data. Saving heightmap and Paint are separate
operations and do not themselves prove Cook or runtime acceptance.
Geometry, TR schema, placement, coverage and Editor details belong to
[the terrain contract](documents/contracts/terrain.md).

`terrainCollision.*` creates static Jolt heightfields from macro height in the caller-owned
PhysicsSystem (cm to m, 16-bit height compression, Y*X*Z rotation).
World retains each placement's plan/TRS; every Play passes them to
`ScriptPhysicsWorld::SetTerrainCollisions`. Stop, repeated Play and World replacement destroy
the old RAII bodies. Ordinary Editor loading creates no physics world. Near-character patches
also sample macro height; resident tiling height/TES displacement does not enter collision or
foot IK. `terraincollision stats` reports Play collision state; process-wide registration is
shared through `EnsureJoltRegistered`, and `TerrainCollisionWorld` is a test/offline fixture.

Retired dual-path sampling, layer-source page management/feedback and old debug controls are
documented only in [the historical snapshot](documents/rationale/terrain-legacy-runtime-contract-snapshot.md).
That snapshot's paths, commands, screenshots and test counts are historical evidence, not current
implementation instructions or acceptance results.

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

## Worktree Isolation For C# Blueprints

C# blueprint builds are scoped by `resourcePath`, not by Git branch. The build service never
inspects the current branch.

- Every `.cs` under `<resourcePath>/Common` and `<resourcePath>/Maps` enters one `Gameplay.dll`;
  there is no per-map or per-blueprint assembly.
- CMake generates `<resourcePath>/Generated/Runtime/CSharp/Gameplay.csproj`, whose
  `ProjectReference` points at the `script/VulkanLearn.Runtime/VulkanLearn.Runtime.csproj`
  of whichever worktree ran configure last.
- Runtime compile candidates go to
  `<resourcePath>/Generated/Runtime/ScriptBuilds/<session>`.

A worktree that changes C# blueprint code or assets must therefore have its own resources
worktree and its own build directory:

- code: `...\VulkanLearn-worktrees\<topic>\VulkanLearn` on `codex/<topic>`
- assets: `...\VulkanLearn-worktrees\<topic>\VukanLearnResources` on
  `codex/<topic>-assets`

Treat each resources worktree as an independent branch. Merge a topic's assets into the
resources master only when the matching code change lands, and merge resources master back
into a topic only when that topic deliberately needs the new baseline. Do not bulk-reset or
bulk-distribute resources master across unrelated topic worktrees; doing so overwrites
branch-local assets and defeats the isolation this layout exists to provide.

Do not point `config/config.json -> resourcePath` at the shared master
`D:\YYBWorkSpace\GitHub\VulkanLearnResources`, including through a junction. Sharing it means
all worktrees compile the same blueprint source set, each configure rewrites the same
`Gameplay.csproj` and can leave another worktree referencing the wrong branch's Runtime API
project, and branch-local blueprint edits leak into every other worktree. Separate
`build/bin/managed` directories still keep `Gameplay.dll` apart, and a running process only
swaps its own in-memory assembly generation; the hazard is the shared source root and
generated project, not the loaded assembly.

Before blueprint work, verify the resource root is not a junction/shared checkout:

```powershell
(Get-Item <workspace>\VukanLearnResources).Target
git -C <workspace>\VukanLearnResources status --short --branch
```

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

- inspect `source/main_editor.cpp`/`source/main_game.cpp` and `source/engine/application.cpp`
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

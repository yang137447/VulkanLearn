# AGENTS.md

This file is the first-stop guide for AI coding agents working in this repository.

## Knowledge Base

User's personal knowledge base lives at `D:\YYBWorkSpace\GitHub\yyb-knowledge-book`. When the user mentions "知识库", refer to this path.

## Document Responsibilities

Use repository documents according to their role:

- `README.md`
  - human-facing project entry point
  - current setup, build, run, and navigation information
- `AGENTS.md`
  - AI-facing collaboration contract
  - repo reading order, hidden assumptions, risky areas, and editing guidance
- `documents/README.md`
  - top-level index for formal design documents
  - explains document categories and what belongs in each one
- `documents/architecture/*`
  - current formal architecture and coding conventions
  - use these when the task touches system boundaries, naming, ownership, or thread/data flow
- `documents/rendering/*`
  - current rendering contracts
  - use these when the task touches implemented material, texture, descriptor, or shader-authoring rules
- `documents/plan/*`
  - future-facing or partially implemented plans
  - use these when the task touches roadmap work that is not fully implemented yet
- `documents/reference/*`
  - tutorials and study material
  - use these for background learning context, not as implementation contracts

Do not mix these responsibilities casually. If a change affects one layer, update the right document instead of stuffing everything into `README.md` or `AGENTS.md`.

## Project Summary

`VulkanLearn` is a Windows-first Vulkan learning renderer built with CMake and C++17.

Current implemented areas include:

- Vulkan device / swapchain initialization
- Shader compilation and SPIR-V loading
- Data-driven scene loading
- Data-driven render graph loading
- Material and material instance loading
- Shadow pass
- Post-process pipeline
- Bloom and tone mapping
- Shader BLAKE3-256 incremental build cache and transitive include invalidation
- Shader hot reload: manual/FileMonitor triggers, compile worker, ABI-compatible Material/Compute/UI replacement, M_*.json schema rebuild, GPU-epoch retirement
- Tracy and NVTX profiling markers

The codebase mixes engine experimentation and learning-oriented iteration. Prefer small, explicit, reversible changes.

## Read Order

When onboarding to the repo, read in this order:

1. `README.md`
2. `source/main.cpp`
3. `source/commonFunction.h`
4. `source/renderGraph.h` and `source/renderGraph.cpp`
5. `source/sceneLoader.h` and `source/sceneLoader.cpp`
6. `source/renderSystem.h` and `source/renderSystem.cpp`
7. `config/config.json`
8. `config/renderGraphConfig.json`

Design references currently live under `documents/`:

- `documents/README.md`
- `documents/architecture/vulkanlearn-architecture.html`
- `documents/architecture/coding-guidelines.md`
- `documents/rendering/texture-asset-json-v1.md`
- `documents/rendering/material-param-authoring-and-reflection.md`
- `documents/rendering/descriptor-imageinfo-management.md`
- `documents/rendering/shader-build-cache.md`
- `documents/rendering/shader-hot-reload.md`
- `documents/plan/rendering/sky-pass-environment-roadmap.md`
- `documents/reference/rendering/tone-mapping-tutorial.html`

Current implementation contracts stay in `architecture/` and `rendering/`. Unimplemented or partially implemented roadmaps stay in `plan/`. Historical implementation notes are intentionally not kept as live docs.

## Build And Run

Configure:

```powershell
cmake -S . -B build -G "MinGW Makefiles"
```

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
- Repository-local `resources/` is not the runtime asset root. Generated runtime outputs should go under `resourcePath/generated/`.

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
- `<resourcePath>/scenes/`: scene JSON files
- `<resourcePath>/models/`: mesh description JSON and source model data
- `<resourcePath>/terrains/`: terrain description JSON files
- `<resourcePath>/materials/`: material instance JSON files
- `shader/glsl/`: shader source of truth
- `shader/spv/`: compiled shader output and debug reflection artifacts
- `extern/`: third-party dependencies
- `tool/`: profiling tools bundled in repo
- `documents/`: active architecture and planning documents

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
- `TR_*.json`: terrain descriptor files
- `MI_*.json`: material instance files
- `scene*.json`: scene definitions

### Current scene object types

Scene JSON currently supports these `type` values:

- `mesh`
- `terrain`
- `directionalLight`
- `pointLight`
- `spotLight`
- `camera`
- `environment`

If adding a new type, update both loader logic and documentation.

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
- `source/sceneLoader.cpp`
  - JSON parsing, material loading, texture loading, shader binding validation
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

- `config/config.json` currently contains an absolute `projectPath`.
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

## Common Task Map

If asked to add or change a scene:

- check `config/config.json`
- inspect `<resourcePath>/scenes/*.json`
- inspect `source/sceneLoader.cpp`

If asked to add or change a terrain:

- inspect `<resourcePath>/scenes/*.json`
- inspect `<resourcePath>/terrains/TR_*.json`
- inspect `source/terrain.h` and `source/terrain.cpp`
- inspect `source/sceneLoader.cpp`
- keep World Creator surface textures in material / texture asset JSON unless the task explicitly adds terrain layer blending

If asked to add or change a render pass:

- inspect `config/renderGraphConfig.json`
- inspect `source/renderGraph.cpp`
- inspect `<resourcePath>/materials/pass/*.json`
- inspect corresponding shaders in `shader/glsl/pass/`

If asked to add or change a material:

- inspect `<resourcePath>/materials/*.json`
- inspect `source/material.cpp`
- inspect `source/materialInstance.cpp`
- inspect shader reflection-dependent code paths

If asked to debug startup failures:

- inspect `source/main.cpp`
- inspect `source/commonFunction.h`
- inspect `config/config.json`
- confirm runtime working directory assumptions

If asked to add or change shader build-cache/hot-reload behavior:

- inspect `documents/rendering/shader-build-cache.md` and `shader-hot-reload.md`
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

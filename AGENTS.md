# AGENTS.md

This file is the first-stop guide for AI coding agents working in this repository.

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
  - future-facing architecture direction
  - use these when the task touches long-term system evolution
- `documents/rendering/*`
  - feature-domain roadmaps
  - use these when the task touches rendering roadmap work

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
- `documents/rendering/sky-pass-environment-roadmap.md`
- `documents/rendering/terrain-worldcreator-mvp.md`
- `documents/rendering/tone-mapping-tutorial.html`

These are the currently active planning documents. Historical implementation notes are intentionally not kept as live docs.

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
- Runtime assets are expected under the repository sibling directory `../VulkanLearnAssets/resources`.
- Repository-local `resources/` is now reserved for placeholder docs and generated local outputs such as `resources/generated/`.

Current local build context:

- the active build generator is `MinGW Makefiles`
- do not assume MSVC-specific behavior in build instructions or code changes
- prefer compiler-portable CMake and C++ unless the task explicitly targets Windows toolchain specifics

## Startup Flow

The current startup sequence in `source/main.cpp` is:

1. Initialize SDL and Vulkan loader
2. Load config JSON
3. Create SDL Vulkan window
4. Compile shaders from `shader/`
5. Initialize `VulkanManager`
6. Create `PipelineFactory`
7. Generate BRDF LUT
8. Load render graph from `config/renderGraphConfig.json`
9. Load scene from `config/config.json -> initScene`
10. Initialize `RenderSystem`
11. Enter update + render loop

If a change affects boot behavior, verify it against this order.

## Important Directories

- `source/`: engine and renderer source
- `source/pipeline/`: pipeline factory, builders, graphics and compute pipeline code
- `source/resource/`: image and device texture helpers
- `config/`: top-level runtime config and render graph config
- `../VulkanLearnAssets/resources/scenes/`: scene JSON files
- `../VulkanLearnAssets/resources/models/`: mesh description JSON and source model data
- `../VulkanLearnAssets/resources/terrains/`: terrain description JSON files
- `../VulkanLearnAssets/resources/materials/`: material instance JSON files
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
- Runtime resource lookup resolves relative asset paths from `../VulkanLearnAssets/resources`.
- `config/renderGraphConfig.json` defines render resources and pass order before scene loading.
- Passes with `needCreateMaterial: true` rely on material instance JSON to create pass materials.
- Material parameter validation depends on shader reflection results.
- Pass input textures depend on descriptor set and binding conventions.
- `shader/glsl/` is the editable shader source; `shader/spv/` is generated output used at runtime and for reflection.

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
- For shader resource declarations such as `sampler2D emissionMap`, do not add `#if defined(...)` guards by default. Prefer relying on the compiler to optimize out unused resources unless the active reflection/toolchain path is explicitly verified to require guarded declarations.
- Only edit `shader/spv/` directly if the task is specifically about generated artifacts or debug outputs.
- Keep JSON structure and naming stable unless the task is a format migration.
- Avoid broad refactors unless the task clearly calls for them.
- Preserve learning-oriented readability over over-abstraction.

## Common Task Map

If asked to add or change a scene:

- check `config/config.json`
- inspect `resources/scenes/*.json`
- inspect `source/sceneLoader.cpp`

If asked to add or change a terrain:

- inspect `resources/scenes/*.json`
- inspect `resources/terrains/TR_*.json`
- inspect `source/terrain.h` and `source/terrain.cpp`
- inspect `source/sceneLoader.cpp`
- keep World Creator surface textures in material / texture asset JSON unless the task explicitly adds terrain layer blending

If asked to add or change a render pass:

- inspect `config/renderGraphConfig.json`
- inspect `source/renderGraph.cpp`
- inspect `resources/materials/pass/*.json`
- inspect corresponding shaders in `shader/glsl/pass/`

If asked to add or change a material:

- inspect `resources/materials/*.json`
- inspect `source/material.cpp`
- inspect `source/materialInstance.cpp`
- inspect shader reflection-dependent code paths

If asked to debug startup failures:

- inspect `source/main.cpp`
- inspect `source/commonFunction.h`
- inspect `config/config.json`
- confirm runtime working directory assumptions

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

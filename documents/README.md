# Documents

This folder contains repository-visible design documents that are still useful for current work.

## Responsibility Split

Documentation in this repository is split by responsibility:

- repository root `README.md`
  - human-facing project entry point
  - current build, run, and navigation guidance
- repository root `AGENTS.md`
  - working contract for AI coding agents
  - read order, repo assumptions, editing constraints, and risk hints
- `documents/README.md`
  - top-level index for formal design documents
  - explains document categories and what belongs in each one

`documents/` should not duplicate the full content of `README.md` or `AGENTS.md`.

- If a document is about current setup and how to start, it belongs in `README.md`.
- If a document is about how an AI agent should work in this repo, it belongs in `AGENTS.md`.
- If a document is about architecture, rendering contracts, future work, or learning references, it belongs under `documents/`.

## Structure

- `architecture/`
  - current formal architecture and coding conventions
  - system boundaries, ownership, thread/data flow, and accepted naming rules
- `rendering/`
  - current rendering contracts that are implemented or actively used by code
  - data formats and ownership rules that contributors should follow now
- `plan/`
  - future or partially implemented work, plus completed decision records retained for context
  - roadmaps, migration blueprints, long-term rendering plans, and deprecated design options; current contracts do not live here
- `reference/`
  - tutorials, courses, and study material
  - useful background that is not itself a live implementation plan

## Formal Architecture

- `architecture/vulkanlearn-architecture.html`
  - VulkanLearn V1 formal architecture HTML
  - covers Platform/Input, EngineLoop, World, Asset/Material/Shader, Renderer/Vulkan Backend/FrameGraph, Config/Debug/Test, design patterns, GT/RT data boundaries, WorldSnapshot publish/consume, resource lifetime, hard cutover status, acceptance checks, and risk records
  - naming rule: `UE-Lite` is only the earlier architecture route name and validation-script context; new C++ engine code uses the `VL` namespace
- `architecture/coding-guidelines.md`
  - general C++ coding conventions
  - currently records public header responsibility comment rules

## Current Rendering Contracts

- `rendering/descriptor-imageinfo-management.md`
  - texture `vk::DescriptorImageInfo` ownership and descriptor write lifetime rules
- `rendering/environment-update-scheduler.md`
  - implemented environment dirty generations, face/mip budgets, active/pending commit, SH swapchain broadcast, Vulkan barriers, and GPU timestamp diagnostics
- `rendering/csm-shadow-map-m1.md`
  - implemented Basic CSM contract for shadowMap array resources, per-layer framebuffer views, runtime CSM settings, shader UBO fields, and debug views
- `rendering/material-param-authoring-and-reflection.md`
  - material parameter include generation, Set 1 schema ownership, and per-pass reflection validation
- `rendering/material-mesh-pass-composition.md`
  - implemented Material Evaluation, Base/ShadowDepth templates, Composer identity, and Shadow routing contract
- `rendering/shader-build-cache.md`
  - implemented BLAKE3-256 shader identities, versioned build-cache manifest, batch-wide publication preflight, stale cache-hit rejection, atomic artifact commit, and startup failure semantics
- `rendering/shader-hot-reload.md`
  - implemented source-epoch/digest staleness protocol, pending source union, frozen compilation snapshots, Material/Compute/UI transactions, M_*.json live-state migration, World/Graph staging, and GPU-epoch retirement
- `rendering/texture-asset-json-v1.md`
  - texture asset JSON V1 fields, defaults, material instance references, and loader behavior

## Plans

Architecture plans:

- `plan/architecture/architecture-maintenance-recovery-plan.md`
  - executable 2026-08-14 repair plan for restoring Vulkan device/test boundaries, extracting Shader and World/Graph transaction ownership from EngineLoop, converging World loading paths, and rerunning the complete architecture validation matrix
- `plan/architecture/modern-engine-refactor-blueprint.html`
  - earlier modern-engine refactor blueprint kept as planning context
- `plan/architecture/ue-lite-completion-plan.md`
  - completed UE-Lite/VulkanLearn V1 architecture baseline, hard boundary regression rules, and validation matrix context

Rendering plans:

- `plan/rendering/lightweight-material-shadow-caster-plan.md`
  - superseded first-stage execution record for explicit ShadowCaster variants; current behavior is defined by the CSM and Material Mesh Pass contracts
- `plan/rendering/common-masked-shadow-caster-plan.md`
  - superseded Common Opaque / Masked experiment retained as historical design context; the lightweight material ShadowCaster plan is the current direction
- `plan/rendering/csm-shadow-map-roadmap.md`
  - four-phase cascaded shadow route: Basic CSM, screen-space shadow mask, stability/seam control, and custom filtering
- `plan/rendering/deferred-gbuffer-ue-aligned-plan.html`
  - deferred GBuffer, UE legacy slot alignment, M_/MI_ material layering, and forward/deferred shader structure plan
- `plan/rendering/foliage-speedtree-sss-wind-roadmap.md`
  - markdown route for SpeedTree, foliage SSS, wind, and multi-pivot work
- `plan/rendering/foliage-speedtree-sss-wind-roadmap.html`
  - HTML reading version of the foliage / SpeedTree route
- `plan/rendering/material-module-system.md`
  - future material module boundary, public/private semantics, and dependency ordering
- `plan/rendering/material-multipass-pass-tag-plan.md`
  - future Material Multi-Pass asset model, PassTag matching, common RG hooks, render-state JSON draft, and draw-list execution plan
- `plan/rendering/material-shader-variant-and-debugview-options.md`
  - deprecated shader variant options retained as context; use the deferred GBuffer plan for the current direction
- `plan/rendering/shader-incremental-compile-and-hot-reload-plan.md`
  - completed staged Shader build-cache and hot-reload plan; the stable P1-P4 contracts have migrated to `rendering/shader-build-cache.md` and `rendering/shader-hot-reload.md`
- `plan/rendering/shader-incremental-compile-and-hot-reload-completion-fix-plan.md`
  - completed 2026-08-13 R1-R4 audit history covering async staleness, M_ live-state migration, World/RenderGraph atomic commit, publication hardening, shutdown, and the final validation matrix
- `plan/rendering/environment-cubemap-ibl-unification-plan.md`
  - executable migration plan that limits environment-type branching to cubemap generation and unifies SH/prefilter through one GPU IBL baker
- `plan/rendering/uds-replication-roadmap.md`
  - UDS / UDW runtime feature scope, staged implementation plan, estimates, risks, and acceptance criteria
- `plan/rendering/sky-pass-environment-roadmap.md`
  - independent Sky Pass, procedural sky, dynamic environment IBL, and frame-spread update route
- `plan/rendering/sky-pass-environment-roadmap.html`
  - HTML reading version of the Sky Pass route
- `plan/rendering/shadow-mode-material-pass-plan.html`
  - superseded explicit `shadowMode` proposal retained as historical context; current Common ShadowCaster selection derives from `renderMode`
- `plan/rendering/speedtree-sdk-data-probe.md`
  - SDK-backed SpeedTree data probe before final runtime foliage format decisions
- `plan/rendering/terrain-worldcreator-mvp.md`
  - World Creator terrain intake MVP and future terrain boundaries
- `plan/rendering/weather-gi-long-term-roadmap.html`
  - long-term weather, reflection, GI, DDGI, and Lumen-like system route

UI documents:

- `architecture/game-ui-stack.md`
  - current implementation contract for the RmlUi runtime layer, optional Dear ImGui developer layer, SDL3 input arbitration, immutable UI snapshots, hot reload, and Vulkan overlay recording
- `plan/ui/game-ui-stack-plan.md`
  - implemented decision record and adoption checklist for the two-layer UI stack

## References

- `reference/architecture/rendering-engine-patterns-course.html`
  - rendering-engine design pattern course and study map
- `reference/rendering/tone-mapping-tutorial.html`
  - tone mapping tutorial, exposure/curve/bloom responsibility notes, and VulkanLearn integration suggestions

## Classification Rule

Use these rules when adding or moving documents:

- Current implemented contracts go under `architecture/` or `rendering/`.
- Future work, partially implemented routes, migration plans, and deprecated design options go under `plan/`; completed decisions may remain there only as history, while the implementation contract moves to `architecture/` or `rendering/`.
- Tutorials and background study material go under `reference/`.
- Historical notes that no longer guide implementation should be deleted or folded into a current document instead of staying as live guidance.

Completed or obsolete implementation notes should not stay here as active docs. Git history is the archive.

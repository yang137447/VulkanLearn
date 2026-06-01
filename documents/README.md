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
  - future or partially implemented work
  - roadmaps, migration blueprints, long-term rendering plans, and deprecated design options kept for context
- `reference/`
  - tutorials, courses, and study material
  - useful background that is not itself a live implementation plan

## Formal Architecture

- `architecture/vulkanlearn-architecture.html`
  - VulkanLearn V1 formal architecture HTML
  - covers Platform/Input, EngineLoop, World, Asset/Material/Shader, Renderer/RHI/FrameGraph, Config/Debug/Test, design patterns, GT/RT data boundaries, WorldSnapshot publish/consume, resource lifetime, migration status, acceptance checks, and risk records
  - naming rule: `UE-Lite` is only the earlier architecture route name; new C++ engine code uses the `VL` namespace
- `architecture/coding-guidelines.md`
  - general C++ coding conventions
  - currently records public header responsibility comment rules

## Current Rendering Contracts

- `rendering/descriptor-imageinfo-management.md`
  - texture `vk::DescriptorImageInfo` ownership and descriptor write lifetime rules
- `rendering/material-param-authoring-and-reflection.md`
  - material parameter include generation and shader reflection as the runtime binding truth
- `rendering/texture-asset-json-v1.md`
  - texture asset JSON V1 fields, defaults, material instance references, and loader behavior

## Plans

Architecture plans:

- `plan/architecture/modern-engine-refactor-blueprint.html`
  - earlier modern-engine refactor blueprint kept as planning context

Rendering plans:

- `plan/rendering/compute-bloom-roadmap.md`
  - bloom migration from fullscreen graphics passes to compute shader plus mip pyramid
- `plan/rendering/deferred-gbuffer-ue-aligned-plan.html`
  - deferred GBuffer, UE legacy slot alignment, M_/MI_ material layering, and forward/deferred shader structure plan
- `plan/rendering/foliage-speedtree-sss-wind-roadmap.md`
  - markdown route for SpeedTree, foliage SSS, wind, and multi-pivot work
- `plan/rendering/foliage-speedtree-sss-wind-roadmap.html`
  - HTML reading version of the foliage / SpeedTree route
- `plan/rendering/material-module-system.md`
  - future material module boundary, public/private semantics, and dependency ordering
- `plan/rendering/material-shader-variant-and-debugview-options.md`
  - deprecated shader variant options retained as context; use the deferred GBuffer plan for the current direction
- `plan/rendering/sky-pass-environment-roadmap.md`
  - independent Sky Pass, procedural sky, dynamic environment IBL, and frame-spread update route
- `plan/rendering/sky-pass-environment-roadmap.html`
  - HTML reading version of the Sky Pass route
- `plan/rendering/shadow-mode-material-pass-plan.html`
  - explicit `shadowMode` material contract, common opaque shadow path, masked material shadow variants, and rollout plan
- `plan/rendering/speedtree-sdk-data-probe.md`
  - SDK-backed SpeedTree data probe before final runtime foliage format decisions
- `plan/rendering/terrain-worldcreator-mvp.md`
  - World Creator terrain intake MVP and future terrain boundaries
- `plan/rendering/weather-gi-long-term-roadmap.html`
  - long-term weather, reflection, GI, DDGI, and Lumen-like system route

## References

- `reference/architecture/rendering-engine-patterns-course.html`
  - rendering-engine design pattern course and study map
- `reference/rendering/tone-mapping-tutorial.html`
  - tone mapping tutorial, exposure/curve/bloom responsibility notes, and VulkanLearn integration suggestions

## Classification Rule

Use these rules when adding or moving documents:

- Current implemented contracts go under `architecture/` or `rendering/`.
- Future work, partially implemented routes, migration plans, and deprecated design options go under `plan/`.
- Tutorials and background study material go under `reference/`.
- Historical notes that no longer guide implementation should be deleted or folded into a current document instead of staying as live guidance.

Completed or obsolete implementation notes should not stay here as active docs. Git history is the archive.

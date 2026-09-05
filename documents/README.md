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

- `architecture/asset-organization.md`
  - runtime resource repository standard: `Common/`, `Maps/SC_<scene-name>/`,
    type subdirectories, ownership rules, and generated-output boundaries
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
- `rendering/debug-runtime-commands.md`
  - background camera control and swapchain screenshot commands for renderer debugging
- `rendering/environment-update-scheduler.md`
  - implemented environment dirty generations, face/mip budgets, active/pending commit, SH swapchain broadcast, Vulkan barriers, and GPU timestamp diagnostics
- `rendering/csm-shadow-map-m1.md`
  - implemented Basic CSM contract for shadowMap array resources, per-layer framebuffer views, runtime CSM settings, shader UBO fields, and debug views
- `rendering/material-param-authoring-and-reflection.md`
  - material parameter include generation, Set 1 schema ownership, and per-pass reflection validation
- `rendering/material-mesh-pass-composition.md`
  - implemented Material Evaluation, Base/ShadowDepth templates, Composer identity, and Shadow routing contract
- `rendering/shader-structure-and-material-function.md`
  - current Shader Structure, UE-style Material Inputs, Material Function composition, Shading Model boundaries, and MF/Pass validation rules
- `rendering/car-paint-shading-model.md`
  - implemented ClearCoat-based car paint contract, custom data packing, and authoring parameters
- `rendering/thin-translucent-shading-model.md`
  - implemented UE 5.8 Legacy Thin Translucent closure, dual-source blend path, scalar fallback, sorting, and lamp-cover authoring contract
- `rendering/neox-character-alignment-contract-v1.md`
  - NeoX 角色从 MTG/源 Shader/纹理/glTF 到 VulkanLearn Material、RenderState、Pass 和验证的当前对齐合同
- `rendering/subsurface-shading-models.md`
  - implemented Subsurface, PreintegratedSkin, and SubsurfaceProfile contracts, including Compute-only lookup generation, GBuffer packing, lighting-lobe composition, profile filtering, and validation
- `rendering/neox-skin-effect-alignment-contract-v1.md`
  - NeoX `pbr_skin` 到现有 `PreintegratedSkin` 的输入、MF、能量、RenderState 和验证规则；不新增 Shading Model
- `rendering/shader-build-cache.md`
  - implemented BLAKE3-256 shader identities, versioned build-cache manifest, batch-wide publication preflight, stale cache-hit rejection, atomic artifact commit, and startup failure semantics
- `rendering/shader-hot-reload.md`
  - implemented source-epoch/digest staleness protocol, pending source union, frozen compilation snapshots, Material/Compute/UI transactions, M_*.json live-state migration, World/Graph staging, and GPU-epoch retirement
- `rendering/texture-asset-json-v1.md`
  - texture asset JSON V1 fields, defaults, material instance references, and loader behavior
- `rendering/eye-shading-model.md`
  - implemented Eye contract covering ForwardOpaque single-shell, Deferred GBuffer V1 fallback, dual-shell inner/cornea passes, Compute-only caustic LUT, local SSS composition, authoring/LOD fields, runtime validation, and transactional Compute reload
- `rendering/cloth-shading-model.md`
  - current Cloth v2 contract covering v1-compatible and anisotropic Charlie direct lighting, dual Compute-only directional-albedo LUTs, versioned GBuffer tangent/anisotropy ownership, Forward/Deferred shared evaluator, energy compensation, reload, and explicit diffuse IBL fallback
- `rendering/two-sided-foliage-shading-model.md`
  - current TwoSidedFoliage ID 6 contract covering UE Legacy Transmission closure, independent Opacity/Subsurface authoring with BaseColor A fallback, Forward/Deferred shared evaluator, unified worldNormal semantics, and ShadowDepth boundaries

## Plans

Architecture plans:

- `plan/architecture/architecture-maintenance-recovery-plan.md`
  - completed 2026-08-14 repair record for restoring Vulkan device/test boundaries, extracting Shader and World/Graph transaction ownership from EngineLoop, converging World loading paths, and rerunning the complete architecture validation matrix
- `plan/architecture/renderer-cohesive-runtime-extraction-plan.md`
  - deferred R6 follow-up for CSM frame data, post-process controls, environment orchestration, and RenderGraph Vulkan state-package ownership
- `plan/architecture/csharp-actor-runtime-plan.md`
  - planned in-process C# gameplay Actor runtime using `*.actor.cs` source files, explicit native/managed ABI, World/Component lifecycle, scene binding, services, reload, and validation phases
- `plan/architecture/modern-engine-refactor-blueprint.html`
  - earlier modern-engine refactor blueprint kept as planning context
- `plan/architecture/ue-lite-completion-plan.md`
  - completed UE-Lite/VulkanLearn V1 architecture baseline, hard boundary regression rules, and validation matrix context

Rendering plans:

- `plan/rendering/material-instance-imgui-editor-plan.md`
  - planned command-only UE-style MI Asset Editor: scenes reveal referenced assets, documents own numeric/texture configuration and atomic save, runtime preview is an optional bridge, and ImGui/Console/tests/AI share one versioned command protocol
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
- `plan/rendering/two-sided-foliage-shading-model-development-plan.md`
  - UE5.8 Legacy TwoSidedFoliage Shading Model 接入路线、GBuffer/Pass 合同、验证矩阵和后续边界
- plan/rendering/neox-b-f-3725-skin-p0-baseline-v1.md
  - _f_3725 Skin P0 源槽位、目标 MI、纹理通道、指纹和运行时 smoke 基线
- `plan/rendering/neox-b-f-3725-skin-effect-alignment-plan.md`
  - `b_f_3725` 身体/脸部 Skin 的分阶段对齐计划、阶段门和验证矩阵
- `plan/rendering/foliage-speedtree-sss-wind-roadmap.html`
  - HTML reading version of the foliage / SpeedTree route
- `plan/rendering/subsurface-shading-models-development-plan.md`
  - completed 2026-08-22 implementation record for Subsurface, PreintegratedSkin, and SubsurfaceProfile; the current contract lives in `rendering/subsurface-shading-models.md`
- `plan/rendering/cloth-shading-model-development-plan.md`
  - completed 2026-08-23 Cloth MVP implementation record; the current contract lives in `rendering/cloth-shading-model.md`, while Charlie-specific IBL prefilter remains a documented target extension
- `plan/rendering/cloth-shading-model-v2-anisotropy-upgrade-handoff.md`
  - executed 2026-09-01 handoff for upgrading Cloth ID 8 with NeoX Silk/Cloth anisotropy while preserving the existing Shading Model identity; anisotropic IBL remains an explicit follow-up
- `plan/rendering/hair-shading-model-development-plan.md`
  - executable Hair Shading Model route from contract freeze and CPU Reference through versioned LUT, Forward/Deferred evaluator, Card coverage/shadow, Hair IBL, and multiple scattering
- `plan/rendering/shading-model-validation-plan.md`
  - shading model 具象验证案例、资产授权、场景构图、分阶段落地和验收矩阵
- `plan/rendering/eye-shading-model-development-plan.md`
  - completed 2026-08-23 Eye implementation record; the current Forward/Deferred/dual-shell, Compute LUT, local SSS, reload, validation, and performance contracts live in `rendering/eye-shading-model.md`
- `plan/rendering/material-module-system.md`
  - future material module boundary, public/private semantics, and dependency ordering
- `plan/rendering/neox-character-shader-feature-inventory.md`
  - NeoX b_f_3725 material-family inventory, verified source-channel contracts, landed 35-slot migration status, and remaining visual calibration scope
- `plan/rendering/neox-b-f-3725-character-restoration-handoff.md`
  - 2026-08-24 b_f_3725 restoration handoff covering frozen constraints, source assets, 35-slot mapping, regeneration commands, procedural-sky scene, validation, intentional differences, and external resource transfer requirements
- `plan/rendering/neox-b-f-3725-hair-effect-alignment-plan.md`
  - executable b_f_3725 Hair effect alignment plan that freezes the shared UE-aligned Hair Shading Model and confines NeoX texture, TBN, coverage, variation, and authoring semantics to Material Functions and assets
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

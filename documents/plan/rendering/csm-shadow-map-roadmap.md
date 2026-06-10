# CSM Shadow Roadmap

## Purpose

This document defines the staged technical plan for directional-light cascaded
shadow maps in VulkanLearn.

The route is intentionally split into four phases:

```text
M1. Basic CSM
M2. Screen-Space Shadow Mask
M3. CSM Stability And Seam Control
M4. Custom Shadow Filtering
```

The phases should be implemented in order. Each phase must remain small enough
to build, smoke test, inspect in RenderDoc, and commit independently. Avoid
mixing seam tuning, custom filtering, or frame-spread updates into the first CSM
implementation.

## Current Baseline

- `config/renderGraphConfig.json` defines one single-layer `shadowMap` depth
  resource.
- The `shadow` pass writes one `D32_SFLOAT` shadow map.
- Deferred lighting samples `shadowMap` as a single shadow texture.
- `UBOGlobal` carries one directional-light shadow matrix.
- Pass behavior is still coupled to existing pass runtime conventions.
- Forward materials have a shadow path, but the primary opaque path is deferred
  through the GBuffer.

## Ownership Rules

- RenderGraph owns render resource shape and pass connections.
- `config/renderGraphConfig.json` owns shadow texture resolution, format, array
  layer count, pass order, and pass inputs/outputs.
- `config/config.json` owns algorithm settings such as cascade count, shadow
  distance, split lambda, quality mode, and bias values.
- Runtime CSM code must consume parsed runtime configuration instead of reading
  JSON directly from `RenderSystem`.
- Shadow pass material instances remain shared. Per-cascade matrices, splits,
  and bias values are frame/global data, not material instance parameters.
- Deferred opaque lighting may use a screen-space shadow mask after M2.
- Forward, transparent, and special materials must keep a direct CSM sampling
  fallback unless a later phase explicitly replaces it.

Descriptor ownership stays:

```text
Set 0 = frame/global data
Set 1 = material/material-instance data
Set 2 = object data
Set 3 = current RenderGraph pass input textures
```

## Target Data Split

RenderGraph example for the basic CSM shadow resource:

```json
{
    "name": "shadowMap",
    "type": "texture2DArray",
    "format": "D32_SFLOAT",
    "widthSize": 2048,
    "heightSize": 2048,
    "arrayLayers": 4,
    "usage": ["depthStencilAttachment", "sampled", "transferSrc", "transferDst"]
}
```

Runtime config example:

```json
"csm": {
    "enabled": true,
    "cascadeCount": 4,
    "shadowDistance": 300.0,
    "splitLambda": 0.65,
    "bias": [
        [0.0010, 1.0, 0.0, 0.0],
        [0.0015, 1.5, 0.0, 0.0],
        [0.0020, 2.0, 0.0, 0.0],
        [0.0030, 3.0, 0.0, 0.0]
    ]
}
```

`shadowDistance` is the maximum camera-forward distance covered by the main
directional light. `splitLambda` controls the practical split blend between
uniform and logarithmic distribution. Startup validation should ensure
`csm.cascadeCount == shadowMap.arrayLayers` when CSM is enabled.

## M1. Basic CSM

Goal: make cascaded shadow maps work in the current renderer with the smallest
useful feature set.

The first implementation should use four explicit shadow passes and one fixed
2D texture array:

```text
shadowCascade0 -> shadowCascade1 -> shadowCascade2 -> shadowCascade3
    -> geometry -> deferredLighting -> sky -> bloom -> toneMapping

shadowMap layer 0 = cascade 0
shadowMap layer 1 = cascade 1
shadowMap layer 2 = cascade 2
shadowMap layer 3 = cascade 3
```

### Deliverables

- Add RenderGraph support for a 2D array shadow resource:
  - optional resource `type`, including `texture2DArray`
  - `arrayLayers`, defaulting to `1`
  - one sampled array view for shader descriptors
  - one per-layer 2D view for framebuffer attachments
- Add per-output `layer` for shadow pass outputs.
- Render four explicit shadow passes, one per `shadowMap` layer.
- Derive the shadow cascade index from the output layer.
- Add parsed CSM settings in `RuntimeConfig`.
- Expand frame/global shader data for:
  - `lightViewProj[4]`
  - `cascadeSplits`
  - first-version per-cascade bias values
- Build per-frame cascade split distances and light matrices once per frame.
- Switch basic deferred shadow sampling to array-layer CSM sampling.
- Keep forward shadow sampling compiling against the same CSM helper.
- Add minimum debug visibility:
  - cascade index view
  - shadow factor view

### Scope Boundaries

- Use the existing shadow pass draw path.
- Keep all cascades the same resolution.
- Keep one shared shadow material instance.
- Do not add screen-space shadow mask yet.
- Do not add custom filtering yet.
- Do not tune seam fixes beyond the minimum required for a usable smoke test.

### Acceptance

- Existing non-CSM render graph resources still load.
- RenderDoc shows one depth texture array with four layers.
- Each cascade pass writes the expected layer.
- Deferred lighting samples the selected cascade layer.
- Forward shadow code compiles with the shared helper.
- The app passes a frame smoke test.

## M2. Screen-Space Shadow Mask

Goal: move CSM resolve out of deferred lighting and into one fullscreen pass.

The intended flow becomes:

```text
shadowCascade0..3 -> geometry -> shadowMask -> deferredLighting
    -> sky -> bloom -> toneMapping
```

The shadow mask pass reconstructs or reads the current opaque surface position,
selects the cascade, samples `shadowMap`, and writes a single-channel shadow
factor. Deferred lighting then samples `shadowMask` instead of directly sampling
CSM.

### Deliverables

- Add a `shadowMask` render resource, initially full resolution.
- Prefer a compact format:
  - `R8_UNORM` for final factor
  - `R16_SFLOAT` if debug precision is useful during development
- Add a fullscreen `shadowMask` pass.
- Feed the pass with the depth/world-position data needed to resolve opaque
  shadows.
- Move deferred CSM sampling from `deferredLighting` into `shadowMask`.
- Update `deferredLighting` to consume `shadowMask`.
- Add debug views for raw `shadowMask` and final lighting shadow factor.
- Keep direct CSM sampling available for forward and transparent paths.

### Transparent And Special Material Rule

Screen-space shadow mask is valid for the foremost opaque surface represented by
the current depth/GBuffer. It is not a complete replacement for direct shadow
sampling on alpha-blended transparent geometry.

Use this split:

```text
Deferred opaque:
    sample shadowMask

Forward opaque / alpha test:
    may sample CSM directly, or sample shadowMask when the path is screen-space
    compatible

Transparent alpha blend:
    keep direct CSM sampling by default
    allow cheap shadowMask sampling only as an explicit approximation
```

### Acceptance

- Deferred lighting no longer needs to select CSM cascades.
- Toggling the shadow mask debug view shows the same shadow coverage used by
  final deferred lighting.
- Forward and transparent materials still have a direct shadow path available.
- The pass can later be changed to half resolution without altering CSM cascade
  rendering.

## M3. CSM Stability And Seam Control

Goal: make the CSM result stable, predictable, and resistant to visible cascade
seams.

This phase addresses visual correctness and synchronization after the main data
flow is established. It should not introduce custom shadow filtering yet.

### Deliverables

- Stable light-space projection for each cascade.
- Texel snapping to reduce shadow swimming.
- Projection padding for filter footprints.
- Shader-side valid-UV padding near cascade borders.
- Centered cascade transition blending.
- Separate per-cascade bias concepts:
  - shader compare bias
  - raster depth bias / slope bias
  - border/filter padding
- Clear UBO update ownership and barriers for repeated global UBO updates in one
  command buffer.
- Debug tools for:
  - cascade boundaries
  - transition bands
  - per-cascade shadow factor
  - shadow mask output

### White-Line Background

The earlier white-line investigation should be treated as a layered stability
lesson, not as proof of one single root cause.

Likely visual contributors:

- PCF or hardware compare filtering can reach outside the selected cascade's
  valid projected rectangle near the boundary.
- A hard cascade split exposes differences in resolution, projection, and bias.
- Missing synchronization around repeated global UBO updates can amplify the
  seam or make matrices appear inconsistent between passes.

The intended fix set is therefore combined:

```text
projection padding
+ valid-UV padding
+ centered transition blending
+ per-cascade bias separation
+ correct UBO update barriers
```

### Acceptance

- Camera movement does not cause obvious cascade swimming.
- Cascade boundaries are acceptable in normal lighting.
- Debug views clearly show cascade selection and transition areas.
- Shadow pass global data cannot be overwritten before earlier shader reads have
  been ordered correctly.
- Seam fixes are documented near the code or in rendering docs when implemented.

## M4. Custom Shadow Filtering

Goal: make shadow filtering behavior controllable by the renderer instead of
depending entirely on hardware compare sampler behavior.

This phase should happen after the shadow mask pass exists, because the shadow
mask pass is the natural centralized place for custom filtering.

### Deliverables

- Add a non-compare depth sampling path for `shadowMap`.
- Keep hardware compare sampling available as a low-cost mode.
- Add small fixed kernels first:
  - 2x2
  - 3x3
  - optional Poisson pattern
- Handle out-of-bounds taps explicitly:
  - skip invalid taps
  - clamp to valid cascade region
  - or fall back to neighboring cascade only when transition logic allows it
- Support per-cascade filter radius.
- Allow quality modes to select hardware compare or custom PCF.
- Optionally add half-resolution `shadowMask` and upscale after basic custom
  filtering is stable.

### Acceptance

- Custom filtering does not sample outside a cascade layer by accident.
- Hardware compare and custom PCF can be compared through a quality setting.
- The shadow mask debug view reflects the selected filtering mode.
- The implementation remains small enough for mobile/low-end modes to disable
  expensive filtering.

## Quality Modes

Quality settings are future-facing, but the design should leave room for them:

```text
Low:
    fewer cascades or lower resolution
    hardware compare
    no custom filtering

Medium:
    3 or 4 cascades
    hardware compare
    transition and padding enabled

High:
    4 cascades
    shadowMask
    small custom PCF

Ultra:
    larger map or higher shadowMask quality
    custom PCF
    optional temporal or denoise experiments
```

Mobile-oriented modes should prefer small kernels, lower cascade counts, compact
mask formats, and optional half-resolution shadow mask.

## Non-Goals

These are intentionally outside the four-phase plan unless a later document
promotes them:

- Shadow atlas packing.
- Mixed cascade resolutions.
- Vulkan multiview or layered rendering through geometry shaders.
- Point light and spot light shadows.
- VSM, EVSM, MSM, PCSS, or virtual shadow maps.
- Static shadow caching.
- Cascade frame-spread updates.
- Temporal shadow mask filtering.
- Contact shadows.
- Ray traced shadows.

## Implementation Notes

- Do not commit future CSM config fields before the corresponding source code
  consumes and validates them.
- Keep RenderGraph shape changes separate from shader sampling changes.
- Keep screen-space shadow mask separate from basic CSM.
- Keep seam stabilization separate from custom filtering.
- Validate each phase with:
  - `cmake --build build -j`
  - a frame smoke test
  - RenderDoc inspection for resource shape and pass order when applicable
- If a phase exposes an implicit convention, document the convention in the
  relevant `documents/rendering/` contract after implementation.

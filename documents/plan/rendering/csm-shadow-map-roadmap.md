# CSM Shadow Map Roadmap

## Goal

Add cascaded shadow maps for the main directional light while keeping the current
render flow readable:

```text
shadow -> geometry -> deferredLighting -> sky -> bloom -> toneMapping
```

The first implementation should use a fixed-size 2D texture array:

```text
shadowMap layer 0 = cascade 0
shadowMap layer 1 = cascade 1
shadowMap layer 2 = cascade 2
shadowMap layer 3 = cascade 3
```

This keeps descriptor binding, shader sampling, and RenderDoc inspection simple.
Different cascade sizes, atlas packing, multiview, and virtual shadow maps are
future optimizations, not first-version requirements.

## Current Baseline

- `config/renderGraphConfig.json` defines one `shadowMap` depth resource.
- `shadow` pass writes one `D32_SFLOAT` depth texture.
- `deferredLighting` samples `shadowMap` as `sampler2DShadow`.
- `UBOGlobal` carries one `lightViewProj`.
- Forward material output already has a shadow sampling path, but the active
  opaque path is deferred through GBuffer.

## Target Contract

- The engine owns one CSM resource for the main directional light.
- All cascade layers have the same width, height, format, and sampler.
- Each cascade has its own light view-projection matrix.
- Deferred and forward lighting share the same CSM sampling function.
- Shadow split distances are stored in global shader parameters.
- The first CSM version renders cascades one by one into per-layer framebuffers.

Example render graph resource:

```json
{
    "name": "shadowMap",
    "format": "D32_SFLOAT",
    "widthSize": 2048,
    "heightSize": 2048,
    "arrayLayers": 4,
    "usage": ["depthStencilAttachment", "sampled", "transferSrc", "transferDst"]
}
```

## Milestones

### M1. Array Shadow Resource

Add render graph support for `arrayLayers`.

Deliverables:

- Parse `arrayLayers`, defaulting to `1`.
- Create `shadowMap` as a 2D array image when `arrayLayers > 1`.
- Create one sampled array view for shaders.
- Create one 2D layer view per cascade for framebuffer attachments.
- Keep non-array resources on the existing path.

Acceptance:

- Existing single-layer shadow map still works when `arrayLayers` is absent.
- RenderDoc shows `shadowMap` as a depth texture array with 4 layers.

### M2. Per-Cascade Shadow Pass

Render the shadow pass once per cascade layer.

Deliverables:

- Add shadow pass framebuffers for each `shadowMap` layer.
- Loop over cascade index in `PassRuntime::RecordShadowPass`.
- Update the shadow global UBO with the selected cascade matrix before each draw.
- Keep the existing common `M_shadow` material and object draw path.

Acceptance:

- Each shadow map layer receives depth data.
- Layer 0 through layer 3 can be inspected independently in RenderDoc.

### M3. CSM Camera Data

Compute cascade split distances and light matrices.

Deliverables:

- Add a CSM settings block in code or config:
  - cascade count: `4`
  - shadow distance
  - split lambda
  - map resolution
- Split the camera frustum into cascade ranges.
- Calculate one stable orthographic light projection per cascade.
- Apply texel snapping to reduce shimmer.
- Store all cascade matrices and split distances for later lighting passes.

Acceptance:

- Moving the camera does not cause obvious shadow swimming.
- Cascade ranges follow the camera frustum and directional light.

### M4. Shader CSM Sampling

Replace single shadow map sampling with CSM sampling.

Deliverables:

- Change deferred shadow input to `sampler2DArrayShadow`.
- Add global shader data:

```glsl
#define CSM_CASCADE_COUNT 4
mat4 lightViewProj[CSM_CASCADE_COUNT];
vec4 cascadeSplits;
```

- Select cascade from view-space depth.
- Sample the selected array layer.
- Reuse the same CSM sampling helper in forward lighting.

Acceptance:

- Deferred lighting receives shadow from the correct cascade layer.
- Forward shadow code compiles against the same CSM helper.

### M5. Debug Views

Add simple CSM debug modes.

Deliverables:

- Cascade index view.
- Shadow factor view.
- Optional selected layer view for shadow map inspection.

Acceptance:

- Cascade boundaries are visible in debug mode.
- Shadow factor debug mode matches the final lighting result.

### M6. Quality Pass

Tune first-version quality without expanding scope.

Deliverables:

- Add fixed-size PCF for CSM sampling.
- Add slope or normal bias if acne is visible.
- Add simple cascade edge blending only if seams are obvious.

Acceptance:

- Default scene has stable shadows.
- Cascade transitions are acceptable without atlas or virtual shadow features.

## Non-Goals For First Version

- Different cascade resolutions.
- Shadow atlas packing.
- Vulkan multiview.
- Geometry shader layered rendering.
- Point light or spot light shadow integration.
- VSM, EVSM, MSM, PCSS, or virtual shadow maps.
- Shadow caching for static casters.

## Recommended Implementation Order

1. Add array resource support without changing shader behavior.
2. Render all cascade layers with the existing single-cascade matrix as a smoke test.
3. Add real cascade split and matrix calculation.
4. Switch deferred lighting to `sampler2DArrayShadow`.
5. Wire forward lighting to the same helper.
6. Add debug views.
7. Tune bias and PCF.

## Main Risks

- Render graph image views must distinguish sampled array views from per-layer
  attachment views.
- `UBOGlobal` layout changes affect every shader that includes
  `common/commonUbo.glsl`.
- Shadow pass UBO updates must not overwrite camera-pass global data needed by
  deferred lighting.
- Cascade split selection must use the same depth convention on CPU and shader.
- PCF must not sample outside the selected array layer.

## Future Upgrades

- Shadow atlas for mixed-resolution cascades and local lights.
- Per-cascade shadow caster culling.
- GPU-driven shadow draw submission.
- Shadow cache for static geometry.
- Contact shadows for near-field detail.
- Ray traced shadow comparison path.
- Virtual shadow maps for large-scene experiments.

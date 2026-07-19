# CSM Shadow Map M1 Contract

This document records the implemented M1 contract for directional-light cascaded
shadow maps.

## Ownership

- `config/renderGraphConfig.json` owns the shadow render resource shape:
  - `shadowMap.type = "texture2DArray"`
  - `shadowMap.arrayLayers`
  - one explicit `shadowCascadeN` pass per layer
  - each shadow pass output declares `layer`
- `config/config.json` owns algorithm settings under `csm`:
  - `enabled`
  - `cascadeCount`
  - `shadowDistance`
  - `splitLambda`
  - `lightSpaceCasterBounds`
  - per-cascade `bias`
- `RuntimeConfig` parses and validates `csm`; `RenderSystem` consumes the parsed
  settings and does not read JSON directly.

When CSM is enabled, startup validation requires:

```text
config.json csm.cascadeCount == renderGraphConfig.json shadowMap.arrayLayers
```

## RenderGraph Resource Views

`texture2DArray` resources create two kinds of image views:

- one full `VK_IMAGE_VIEW_TYPE_2D_ARRAY` view for shader descriptors
- one `VK_IMAGE_VIEW_TYPE_2D` view per array layer for framebuffer attachments

Shadow pass framebuffer construction uses the output `layer` to select the
per-layer attachment view. Pass input descriptors use the full array view.

## Pass Flow

M1 uses explicit passes instead of layered rendering:

```text
shadowCascade0 -> shadowCascade1 -> shadowCascade2 -> shadowCascade3
    -> geometry -> deferredLighting -> sky -> bloom -> toneMapping
```

Each `shadowCascadeN` pass has `type: "shadow"` and writes `shadowMap` layer
`N`. `PassRuntime` dispatches pass behavior from the compiled pass type:
`shadow`, `geometry`, or `postProcess`. Pass `name` remains the unique identity
used for ordering, pass binding, debug labels, and commands. Material and
pipeline cache identity uses shader/state plus render-pass compatibility, not
the pass name. Pipeline state is validated and stored in
`CompiledRenderGraphPass`; Material loading consumes that compiled state and
does not reparse raw RenderGraph JSON.

## Shader Contract

`UBOGlobal` carries:

- `mat4 lightViewProj[4]`
- `vec4 cascadeSplits`
- `vec4 shadowBias[4]`

Shadow sampling uses `sampler2DArrayShadow`. The selected cascade layer comes
from camera view-space depth compared against `cascadeSplits`.

Shadow caster rendering has one common path plus generated or explicit material
paths:

- Common Opaque uses `MI_shadow -> M_shadow -> shadow.vert/.frag`.
- M_ assets with `shaderEvaluation` can compose a `ShadowDepth` pass from the
  same vertex/surface semantics used by Base.
- A shader named `xxx` may still provide a complete `xxx.shadow.vert/.frag`
  pair as the highest-priority explicit override.

The resolved draw route is fixed:

| Material state | Shadow route |
| --- | --- |
| Complete `.shadow` override | Explicit material ShadowCaster |
| Transparent without override | Skip |
| Opacity Mask, mesh-position modification, or two-sided | Generated ShadowDepth |
| Ordinary Opaque | Common Opaque |

There is no Common Masked pipeline and no independent `shadowMode`. A partial
override pair emits an asset warning and then continues through generated/common
routing. Compile failure, missing Evaluation required for generation, or a
descriptor-contract mismatch fails material/world loading instead of silently
falling back.

`Material` owns its Base pipeline and optional ShadowDepth pipeline. Generated
ShadowDepth inherits the effective MI render mode, macros, shading model, and
cull mode. It reuses the object's existing descriptor package:

```text
Set 0: Surface global descriptor set
Set 1: original MaterialInstance parameters and textures
Set 2: Surface object descriptor set
Set 3: forbidden for the first ShadowCaster version
```

Set 1 layout always comes from the complete M_ `MaterialDescriptorSchema`.
Base and Shadow reflection are validated as schema subsets; Set 0/2 Shadow
bindings must remain compatible with the Base engine contract. Descriptor writes
use the union of bindings actually used by Base and ShadowDepth. No Shadow
MaterialInstance, copied parameter block, or additional material/object
descriptor package is created.

The detailed shader composition and identity rules are defined in
`material-mesh-pass-composition.md`.

Common Opaque keeps the existing independent Shadow descriptor contract:

```text
Set 0: current shadow Renderpass descriptor set
Set 1: unused
Set 2: per-object shadow descriptor set
```

All CSM passes reference the same `MI_shadow` asset. MaterialInstance cache
identity is the normalized MI asset path. Material/pipeline identity includes a
render-pass compatibility key built from attachment format/sample information
and subpass attachment references. The CSM passes must have matching
compatibility keys and pipeline state before they may share the canonical
Opaque pipeline; incompatibility is a load error.

## Light-Space Coverage

Each cascade fits its XY orthographic coverage from the camera split frustum.
By default, the shadow camera Z range is expanded from current RenderScene draw
packet world bounds projected into directional-light space, but only for objects
whose light-space XY bounds overlap the cascade coverage. This includes casters
outside the camera split that may still cast shadows into it without expanding
every cascade to the full scene depth. If no overlapping draw packets are
available, the Z range falls back to the split frustum corner bounds.

`csm.lightSpaceCasterBounds` exists only as a diagnostic/performance fallback;
the intended default is `true`.

Current implementation note: when this option is enabled, `RenderSystem` scans
the current frame's draw packets once per cascade to compute light-space Z
bounds. This is acceptable for the current small scenes. If larger scenes make
this visible in CPU profiling, precompute light-space draw bounds once per
shadow frame and reuse them across cascades.

Debug views:

- `debugview 7`: final shadow factor
- `debugview 11`: selected cascade index

M1 intentionally does not implement screen-space shadow masks, cascade seam
blending, texel snapping beyond the existing projection helper, custom PCF, or
frame-spread cascade updates.

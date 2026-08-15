# CSM Shadow Map M1 Contract

This document records the implemented M1 contract for directional-light cascaded
shadow maps.

## Ownership

- `config/renderGraphConfig.json` owns the shadow render resource shape:
  - `shadowMap.type = "texture2DArray"`
  - `shadowMap.arrayLayers`, which is the physical cascade capacity
  - one explicit `shadowCascadeN` pass per layer
  - each shadow pass output declares `layer`
- the first `directionalLight` object in each scene is the primary directional
  light and owns artist-facing settings under `shadow`:
  - `castShadows`
  - `dynamicShadowDistance`
  - `dynamicShadowCascades`
  - `cascadeDistributionExponent`
  - `cascadeTransitionFraction`
  - `shadowDistanceFadeoutFraction`
  - `shadowBias`
  - `shadowSlopeBias`
  - `shadowCascadeBiasDistribution`
- C++ `CsmSettings` defaults apply when the primary directional light omits
  `shadow`.

The scene contract accepts only these UE-style names. Unknown fields are a
scene-load error instead of being ignored or translated by a compatibility
layer.

`dynamicShadowCascades` selects how many layers from the RenderGraph capacity
are active. Inactive shadow passes still clear and transition their configured
layer, but skip scene drawing.

The runtime derives the internal per-cascade receiver bias:

- `constantDepthBias = shadowBias * 0.006 *
  (1 + shadowCascadeBiasDistribution * cascadeIndex)`
- `slopeMultiplier = shadowSlopeBias * 8`
- the derived values are implementation data and are not serialized

`cascadeDistributionExponent` places split `i` at
`pow((i + 1) / cascadeCount, exponent)` across the dynamic shadow distance.
Larger values concentrate more shadow-map resolution near the camera.

- `WorldBuilder` parses and validates the primary directional light's shadow
  settings; `RenderSystem` consumes the prepared World settings and does not
  read scene JSON directly.

M1 startup validation requires:

```text
renderGraphConfig.json shadowMap.arrayLayers == 4
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
`shadow`, `geometry`, `forwardTransparent`, or `postProcess`. Pass `name` remains the unique identity
used for ordering, pass binding, debug labels, and commands. Material and
pipeline cache identity uses shader/state plus render-pass compatibility, not
the pass name. Pass type strings are parsed once at the RenderGraphCompiler
boundary; compiled and runtime state use `RenderGraphPassType`. Pipeline state is validated and stored in
`CompiledRenderGraphPass`; Material loading consumes that compiled state and
does not reparse raw RenderGraph JSON.

## Shader Contract

`UBOGlobal` carries:

- `mat4 lightViewProj[4]`
- `vec4 cascadeSplits`
- `vec4 shadowBias[4]`
- `vec4 csmParameters`
  - `x`: cascade transition fraction
  - `y`: shadow distance fadeout fraction
  - `z`: active cascade count
  - `w`: dynamic shadow distance

Shadow sampling uses `sampler2DArrayShadow`. The selected cascade layer comes
from camera view-space depth compared against `cascadeSplits`.
The receiver applies slope-aware derived bias and a four-tap bilinear PCF
kernel. Inside the configured transition fraction, the shader samples the
current and next cascades and blends their visibility. The final section of the
dynamic shadow distance fades the result to fully lit. The depth-compare sampler
uses a lit clamp-to-border value so taps outside a cascade cannot wrap to the
opposite edge.

Shadow projection and filtering have separate shader ownership:

- `common/lighting.glsl` selects the cascade, projects the receiver, and applies
  receiver bias.
- `common/shadowFiltering.glsl` owns hardware comparison sampling, the current
  four-tap PCF kernel, and the `FilterShadowMap()` policy entry point.

The current four-tap kernel still uses hardware linear comparison filtering for
each tap. Future quality tiers should select their filtering implementation at
`FilterShadowMap()` instead of duplicating CSM projection or bias logic.

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

Common Opaque shadow casting uses the authored `M_shadow` back-face culling
default. Self-shadow acne is controlled through receiver bias instead of
globally switching closed meshes to front-face culling. Generated material
ShadowDepth passes continue to honor their authored cull mode for masked,
two-sided, and position-modified geometry.

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

`lightSpaceCasterBounds` remains an engine implementation setting with a C++
default of `true`; it is not an artist-facing scene property.

Current implementation note: when this option is enabled, `RenderSystem` scans
the current frame's draw packets once per cascade to compute light-space Z
bounds. This is acceptable for the current small scenes. If larger scenes make
this visible in CPU profiling, precompute light-space draw bounds once per
shadow frame and reuse them across cascades.

Debug views:

- `debugview 7`: final shadow factor
- `debugview 11`: selected cascade index

## Runtime Tuning

The RmlUi runtime control page exposes a dedicated Shadows tab. UI mutations
travel through `UiAction`, `RuntimeCommand`, and `RuntimeCommandExecutor`;
RmlUi never edits renderer state or GPU buffers directly.

The current panel exposes:

- Cast Shadows
- Dynamic Shadow Distance
- Dynamic Shadow Cascades
- Cascade Distribution Exponent
- Cascade Transition Fraction
- Shadow Distance Fadeout Fraction
- Shadow Bias
- Shadow Slope Bias
- Shadow Cascade Bias Distribution
- full and cascade-index debug views

`RenderSystem` owns the live values and invalidates cached cascade frame data
after a tuning command. The explicit **Save to Scene** action atomically writes
the current values to the active scene's primary `directionalLight.shadow`
object. Slider movement never writes to disk. Loading or switching scenes
applies that scene's shadow settings through the normal World/RenderGraph
transaction.

M1 intentionally does not implement screen-space shadow masks, custom PCF, or
frame-spread cascade updates.

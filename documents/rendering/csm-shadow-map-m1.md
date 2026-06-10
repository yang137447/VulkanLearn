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
used for ordering, material binding, cache keys, debug labels, and commands.

## Shader Contract

`UBOGlobal` carries:

- `mat4 lightViewProj[4]`
- `vec4 cascadeSplits`
- `vec4 shadowBias[4]`

Shadow sampling uses `sampler2DArrayShadow`. The selected cascade layer comes
from camera view-space depth compared against `cascadeSplits`.

Debug views:

- `debugview 7`: final shadow factor
- `debugview 11`: selected cascade index

M1 intentionally does not implement screen-space shadow masks, cascade seam
blending, texel snapping beyond the existing projection helper, custom PCF, or
frame-spread cascade updates.

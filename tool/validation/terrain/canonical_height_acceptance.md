# Canonical production height helper: independent oracle acceptance

The main thread selected canonical source-texel coordinates and four
`texelFetch` samples with full-float bilinear interpolation in production
`SampleTerrainHeightAtlas`. Validation changes here do not modify production.
This is a numerical-filtering precision improvement; it does not promise
bit equality with the old hardware `textureLod(source,mip2)` result.

## Acceptance criterion

The gate is the **actual production helper versus an independent double source
bilinear oracle**, with maximum error <=2 R16 units, plus exact source-mip2 /
atlas stored-data equality. The threshold is unchanged. The old
hardware-source equality criterion is retained as historical diagnostics,
not used to decide the new mathematical criterion.

`height_bilinear_reference.py` contains the independent CPU reference. It has
no dependency on GLSL, the production helper, production layout functions or
the GPU manual-reference implementation. It calculates
`p=fract(uv)*sourceDim-0.5`, four repeat neighbors and double-precision
interpolation directly from the source pixel array. Known 2x2 center, negative
repeat, fractional and wrap-boundary anchors are checked before dispatch.

The shader includes the actual production helper, invokes it and writes its
float result. The GPU manual-source reference reads the source image using
`textureSize`, independently of the atlas mapping. Its agreement with the helper
is supplementary; the independent double oracle provides the mathematical
acceptance evidence.

## Actual production-helper runs

Raw evidence:

- `artifacts/terrain-composited-vt/independent-gpu-bake/canonical-helper-real-v1/`
- `artifacts/terrain-composited-vt/independent-gpu-bake/canonical-helper-bounded-final/`
- `artifacts/terrain-composited-vt/independent-gpu-bake/canonical-helper-regression/`

The real-image suite dispatches 12 times, with 221,952 sample points on RTX 3080:
the original discontinuity input, all seven real height PNG planes in pairs,
and all seven together in the native 28-page 816x680 atlas. Padded variants remain
additional regressions; padding is not required by the selected production fix.
Negative/repeated UVs, source repeat edges/corners, internal page boundaries and
fractional samples are included.

Real source PNGs are decoded with their authored flip convention. GPU linear
blits produce mip2, and the actual source mip2 and atlas images are read back.
Each stored page/border texel is independently compared to source mip2.

Results:

| Check | Observed | Gate |
| --- | ---: | --- |
| Actual helper versus independent double | max 0.004916192 R16 units | <=2, passed |
| Actual helper versus GPU manual source | 0 R16 units | supplementary <=2, passed |
| Actual GPU atlas data mismatches | 0 | exact equality, passed |
| Canonical four-neighbor value mismatches | 0 | exact equality, passed |
| Unmapped source result | exactly 0.5, zero failures | passed |
| Helper repeat V=0 versus V=1 | zero difference | passed |
| Source mip2, atlas data, UV samples, source images/amplitudes | unchanged versus historical run | recorded hash/byte comparison |

The broader bounded material run additionally retains sloped TBN, layer mixing,
UV rotation/scale, adjacent-page borders and explicit two-mip trilinear checks.
Its maximum albedo/normal/ORM errors remain 1 byte each, normal-angle error
0.778644 degrees, and independent height error is 0.003488660 R16 units.
The deliberately wrong albedo-page order still fails its material threshold.

The bounded material run checks atlas input-payload equality; actual GPU
image-copy/readback data equality is exercised by the real-height suite.
Material shared-evaluator self-comparison does not independently prove material
equations. Height mathematical acceptance uses the separate double oracle.

## Original failure point, retested

Original UV: `(-0.00096893310546875, 1.0)`. Source neighbors:
`6350, 27771, 58117, 14002`; axis weights: `0.251953125, 0.5`.

| Value | R16 units |
| --- | ---: |
| Independent double source bilinear | 29374.587890625 |
| Current production helper | 29374.588881880045 |
| GPU manual source | 29374.588881880045 |
| Legacy hardware source | 29223.999382853508 |
| Current helper versus double error | **0.000991255045** |
| Current helper versus legacy hardware difference | 150.589499026537 |

The old hardware atlas/source difference of 173.001266 units and the expanded
286-unit discontinuity failure have not been overwritten or reclassified as
passes. `bounded-final/report.json` retains its original failed status and
SHA-256 `d190cda1f43a72a91c417c73a4b479c872bbaf99329487ce923b5c33360864bf`.
The hardware-filter diagnosis also remains available under
`height-diagnosis-seven-layer-final`.

## Per-unit physical error

For global displacement multiplier=1, distance fade=1 and unit direction:

```text
mm per R16 unit = displacementM * 1000 / 65535
physical error mm = errorR16 * mm per R16 unit
```

Multiply by the actual multiplier/fade/direction length when evaluating geometry.
Full native seven-layer atlas results:

| Layer | mm / R16 unit | Helper-double max R16 | Helper-double max mm | Helper-hardware-source max mm |
| --- | ---: | ---: | ---: | ---: |
| Gravel Ground | 0.0009155413 | 0.004450918 | 0.0000040750 | 0.00561118 |
| Rock Face | 0.0038147555 | 0.004453719 | 0.0000169899 | 0.03658235 |
| River Water | 0 | 0.000141120 | 0 | 0 |
| Scree Debris | 0.0018310826 | 0.004916192 | 0.0000090020 | 0.01150846 |
| River Silt | 0.0003051804 | 0.004105151 | 0.0000012528 | 0.00238538 |
| Sea | 0 | 0.000130194 | 0 | 0 |
| Beach Sand | 0.0001525902 | 0.004576207 | 0.0000006983 | 0.00281751 |

Across the paired-plane and full seven-layer runs, the largest real-asset
helper-double physical error is approximately 0.00001771mm. Hardware baseline
differences are explicitly different statistics and are not a pass gate.
Per-layer reports also include the physical equivalent of the unchanged
two-R16 threshold.

## Shader identity, reproduction and limits

Actual helper SHA-256 during the run:
`b4524213715e2a42d635f3e1c13212fd6d35b5ef1a4943418367a1d507bc1acd`.
The helper file was checked unchanged between compile and final readback.
Raw readbacks, test/source hashes, command arguments and history comparisons
are retained. No engine C++ rebuild was performed; only the raw validation
harness and shaders were compiled.

```powershell
py -3 tool/validation/terrain/diagnose_composited_height.py `
  --output artifacts/terrain-composited-vt/independent-gpu-bake/canonical-height-new-run
py -3 tool/validation/terrain/validate_composited_bounded_source.py `
  --output artifacts/terrain-composited-vt/independent-gpu-bake/canonical-bounded-new-run
```

Output directories must be new. No renderer or Cook is launched by these tools.
The next main-thread Cook must refresh dependency/content identity for the new
filtering semantics. This validation does not update or certify existing Cook
manifests. Source mip2 contents, UV tiling inputs and displacement amplitudes
are unchanged in the tested inputs.

This passes the bounded numerical height criterion, not all S1. Renderer
TES/detail spatial alignment, fixed-LOD0 real image parity, multi-LOD visual
quality and cross-device/arbitrary-size coverage remain outside this result.

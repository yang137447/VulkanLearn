# Local frozen weight overlay CPU checkpoint

2026-11-21. This is a bounded CPU regression and diagnostic record, not a P3
completion or GPU/visual/TES acceptance.

The production change is limited to `source/terrain/terrainCompositedWeightOverlay.cpp`.
The overlay keeps the existing layer-major R32_FLOAT values, original imported
base, dimensions, linear consumer, and separate local-to-global mapping. It does
not normalize raw layers or replace them with coverage top4.

At each imported texture center the helper inverts the existing Local consumer
margin transform to surface UV, then applies the actual mesh heightmap UV
transform before calling `TerrainLayerPaintData::Blend`. Previously the second
transform was missing. The full plan already reaches `RefreshLocalWeights`;
no new Service Capture parameter is needed.

## Native reproduction

The audit executable links the actual C++ overlay, Paint Blend, Paint file reader
and heightmap UV helpers. It reads real LocalLayerArray TR/MI/height/Paint/imported
weight inputs for tiles 05/06/09/10. Native float32 arithmetic reproduces the
reported discrepancy. Normalized texture taps are a CPU emulation of the current
GPU evaluator, not GPU execution or bitwise GPU equivalence.

Each edge has 2049 paired positions including endpoints. Results below are
finite-sample maxima, not continuous-domain bounds.

| Edge | Direct margin Paint delta | Before overlay delta | Corrected overlay delta | Corrected overlay vs direct Paint maximum error |
| --- | ---: | ---: | ---: | ---: |
| 05/06 | 0.009178847 | 0.663672209 | 0.004123807 | 0.250111997 |
| 09/10 | 0.010613918 | 0.726037383 | 0.005338788 | 0.253848433 |
| 05/09 | 0.007211149 | 0.630467474 | 0.005118936 | 0.333791435 |
| 06/10 | 0.010280907 | 0.700665057 | 0.005032092 | 0.274788857 |

Central corner layer0, direct margin Paint / corrected frozen overlay:

| Tile | Direct | Frozen |
| --- | ---: | ---: |
| 05 | 0.720031738 | 0.681448400 |
| 06 | 0.722645938 | 0.684262097 |
| 09 | 0.717213929 | 0.678427458 |
| 10 | 0.719869733 | 0.681282640 |

At interior surface UV `(0.371, 0.629)`, tile09 layer0 is
`0.394163162 / 0.394343883`. Other layers, corners, internal points, UVs and
worst edge sample vectors are in the native JSON receipts.

**resample limitation is not proven equivalence**

The current overlay semantics first sample the 512² Paint into the existing
515² frozen array, then linearly read that frozen array. This is not equivalent
to directly sampling the original Paint at every query point. The last table
column measures that distinction; it is not an error against an independently
executed GPU frozen-overlay reference. A focused non-affine unit fixture retains
the distinction explicitly: direct Paint is 0.875 while the frozen linear
consumer gives 0.75 at the same surface point.

The acceptance reference is the current GPU Local evaluator consuming the same
frozen raw overlay, including its normalized UV, filtering, local layer traversal,
mapping, strict `>` top4 insertion, and subsequent normalization. The CPU top4
fixture mirrors only that traversal/insertion for regression purposes. Equal
positive Local weights retain local traversal order; this is not Coverage's
global-ID tie policy. No shader execution is claimed by these tests.

The original author mismatch is preserved and reported separately. Smaller
paired overlay deltas do not authorize smoothing authors or establish visual
equivalence. Final dual-mode visual, normal LOD, cross-boundary footprints and
TES gates remain pending. If those fail, investigate against the retained
frozen evaluator semantics. This checkpoint does not increase resolution,
relax budget or change Service, texture shaders or author files.

## Focused regressions and reproduction

The existing module test file now also covers nine painted raw layers,
nonsequential mapping, raw R32 byte roundtrip through an unpainted rebuild,
no normalization, full/partial/zero opacity, holes restoring each layer's
original base, immutable revision snapshots and undo/redo replay, missing
import/Paint/mapping layers, top4 after filtering with a rear layer entering
the result, exact positive ties, invalid mesh UV input, and differing
import/heightmap dimensions.

Run from the checkout root:

```powershell
cmake -S tool/validation/terrain/weight-overlay-cpu -B artifacts/terrain-rvt-production/weight-overlay-cpu/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build artifacts/terrain-rvt-production/weight-overlay-cpu/build --target weight_overlay_tests weight_overlay_audit -j 4
artifacts/terrain-rvt-production/weight-overlay-cpu/build/weight_overlay_tests.exe --gtest_output=xml:artifacts/terrain-rvt-production/weight-overlay-cpu/regression-tests.xml
artifacts/terrain-rvt-production/weight-overlay-cpu/build/weight_overlay_audit.exe ../VukanLearnResources artifacts/terrain-rvt-production/weight-overlay-cpu/final-native.json
```

This independent target uses the already populated GoogleTest source under
`build/_deps`; it does not configure or build the root runtime or
`build-vt-release`, and does not start GPU work.

Receipts under `artifacts/terrain-rvt-production/weight-overlay-cpu/`:
`before.json`, `after.json`, `final-native.json`, `regression-build.log`,
`regression-tests.log`, `regression-tests.xml`, `source-hashes.json`.
The pre-fix overlay cpp SHA256 was
`deb5a335dc0347f95a33266d7f25182a98020e9793033be6fd27dac28ee9827e`.
Final source, evaluator-reference and executable hashes are in
`source-hashes.json`. The latter freezes identities without implying GPU use.

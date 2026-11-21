# S1 real-image quality acceptance and evidence history

Current result: **R8192 / resident192 real fixed-LOD0/noTES quality gate passed,
with independent visual review completed**. Canonical height independently
passed the double-bilinear oracle. Implementation work in this validation task
is finished; final product packaging and final audit remain with the main thread.

## Final evidence update: R8192 / 192 slots

The fixed-geometry real 2x2 quality gate **passed** in
`artifacts/terrain-composited-vt/independent-gpu-bake/s1-r8192-budget192-run/report.json`.
The main thread completed 18 captures and reported driver exit0; the saved status
is `screened-pass-awaiting-human-review`. This follow-up independently inspected
the report and all six far/mid/near base/beauty image pairs, including original-size
center crops. No renderer was started by this review and no saved run report was
rewritten.

The unchanged gate is mean absolute RGB8-component error <=3 and p95 <=8 for
the visible-terrain union **and every qualifying fixed 4x4 grid ROI**, followed
by visual review. Sky/background is excluded using same-camera actual raster
ShadingModel masks. The direct/composite mask XOR is zero at every shot.

| Shot | Terrain pixels | Base union mean / p95 | Beauty union mean / p95 | Worst base grid mean / p95 | Worst beauty grid mean / p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| far | 145,943 | 2.0866 / 7 | 1.4131 / 5 | 2.2630 / 8 | 2.0965 / 7 |
| mid | 921,600 | 0.8774 / 3 | 0.6276 / 2 | 1.0881 / 4 | 0.7962 / 3 |
| near | 921,600 | 0.7959 / 3 | 0.5839 / 2 | 1.1626 / 4 | 0.8629 / 3 |

All grid ROIs pass. The far p95 boundary of 8 is retained as measured, not
rounded down or hidden by the union mean. Independent visual inspection found
no conspicuous new page/tile seam, wrong layer, silhouette displacement, or the
previous large composite-detail loss in these captured views. Fine texture
differences remain visible, particularly far; this is screened equivalence
within the stated gate, not pixel equality or a general beauty-art approval.
The authored overview retains its existing strong global coloration in both
branches.

The review contact sheets and original-image SHA-256 inventory are saved in
`artifacts/terrain-composited-vt/s1-final-independent-review/`:
`far-base-pair.png`, `mid-base-pair.png`, `near-base-pair.png`,
`far-beauty-pair.png`, `mid-beauty-pair.png`, `near-beauty-pair.png`,
and `audit.json`. Original BMPs, masks, differences, camera poses, exact commands
and per-ROI results remain in the main run directory.

### Resolution and working-set choice are part of the result

Accepted fixture:
`Generated/Validation/TerrainS1/Variants/Real2x2_R8192_HeightBlend3/SC_composited_vt_2x2.json`,
8192x8192 / 0.123046875m per virtual texel, strength3, static distance fade0,
192 resident slots per surface, four real tiles. The source-array direct
reference uses mode0. Both sides use LOD0, no TES/detail material and zero
displacement; base-color diagnostics symmetrically disable retained
detail/macro/global modulation.

The previous R8192/64 run
`artifacts/terrain-composited-vt/independent-gpu-bake/s1-filter-v2-r8192-run/report.json`
remains `roi-screen-failed`. At its mid capture Landscape_00 recorded
resident62/64, queued72, reading1, cpuBytes3,773,184 and cumulative estimated
parent fallbacks17,368. The requested fine working set exceeded that pool;
an unsettled frame is not valid evidence of intrinsic filtering quality.
This is a measured budget failure, not a reason to relax the ROI gate.

In the final 192-slot run all captures satisfy three consecutive pre-screenshot
working-set snapshots with queued/reading/cpuBytes/dirty/stale/readFailures=0.
Observed resident states include 155/192 for the largest surface. Thus the
pool increase is a bounded working-set selection, not unlimited residency or
a global hard-cap increase.

The final mid log records:

```text
worldGpuCap=268435456
gpuBudgetScope=cache-excludes-source
cacheBytes=210644416
sourceBytes=186696804
accountedGpuPayloadBytes=397341220
sourceTextures=7 unknownSourceTextures=0
```

Cache payload is approximately 200.89MiB, within the unchanged 256MiB World
cache limit. The approximately 178.05MiB source arrays are separately counted;
total accounted payload is approximately 378.93MiB. This is not a claim that
total terrain GPU memory, including source arrays, fits within 256MiB, nor a
complete allocator/driver VRAM measurement. Zero queues and CPU fallback
estimates also do not constitute per-pixel GPU sampled-mip telemetry.

### Passed scope and remaining work

The actual canonical height helper separately passed the independent
double source-bilinear oracle at unchanged <=2 R16 units: real seven-layer
maximum error0.004916191, stored GPU atlas data mismatches0. The original
173-unit hardware-filter comparison remains a historical failed record,
not reclassified under the new criterion. See
[canonical height acceptance](canonical_height_acceptance.md).

Together these close the tested fixed-LOD0/noTES real-image gate and canonical
height numerical counterexample. Earlier R4096 filter failures, R8192/64
working-set failure and raw hardware-filter failures remain immutable evidence.
They have not been overwritten or retroactively declared passes.

This is **not all S1/S0–S5 acceptance**: final product normal geometry LOD
transitions, tessellation/displacement and TES/detail spatial alignment,
additional nearSlope/mountain views, full-scene human quality and performance/
streaming stress are separate work owned by the main thread after final-product
assembly. The present review claims no additional renderer execution or those
unperformed checks.

## Historical preparation and bounded evidence

Everything below this heading records the earlier preparation and failed
experiments. Statements such as “not verified” describe that historical stage,
not the final R8192/192 result above. The retained original reports are immutable.

### Standalone mip0-center test status

`artifacts/terrain-composited-vt/independent-gpu-bake/center-filter-v2-actual01/`
contains a successfully compiled actual production object and commands log.
Its first standalone CPU-test link failed on unresolved engine symbols;
it contains **no GPU readback and no report.json**, so there are no trustworthy
center-reconstruction GPU error numbers to report from that directory.

The later `center-filter-v2-ready/prepared.json` records successful production
C++ compilation, 43 actual-Validate CPU checks and GLSL compilation, with
`gpuDispatched=false`. It is preparation evidence only, not a raw GPU pass.
The unchanged default4, mip0-only single-sample permission and coarse single-sample
rejection are covered by those CPU checks. No further raw run is required or
claimed here; the executed real R8192/192 paired draw above is the final quality
evidence for the implemented producer and sampler combination.

Current canonical-helper follow-up:
[independent double-oracle acceptance](canonical_height_acceptance.md).
The main thread selected full-float filtering; the original hardware-comparison
failures below remain historical evidence.

Height filtering follow-up: [exact CPU/manual GPU diagnosis](height_filter_diagnosis.md)
localizes the original failure and records a test-only padding counterfactual.
The original failure and strict threshold remain unchanged.

At that earlier stage this was a partial acceptance record, not a declaration
that the real-image gate had passed. Production
shader/C++ changes belong to the coordinating agent. These tools only dispatch
production GLSL, assemble validation inputs and measure GPU readbacks.

## Bounded source test

```powershell
py -3 tool/validation/terrain/validate_composited_bounded_source.py `
  --output artifacts/terrain-composited-vt/independent-gpu-bake/my-new-bounded-run
```

The output directory must be new. The script builds the raw Vulkan harness
outside every engine build/cache, compiles production `terrainCompositedBake.comp`,
and dispatches 17x17 workgroups for each 136x136 page. It retains SPIR-V,
generated reference GLSL, commands, input bytes, raw output planes, hashes and
the report. No renderer is launched.

Inputs are spatially varying albedo/normal/ORM textures with six authored mips,
two spatially mixed layers, noncontiguous global IDs 13/207, distinct scales
1.3/0.7 and rotations 0.37/-0.81 radians, and a varying sloped 8x8 normal/tangent
frame with negative handedness. These are bounded test textures, not real
Pegasus texture pixels. Source albedo is sampled through an sRGB view.

The bake produces two adjacent pages at virtual resolution 256x128 and a coarse
page at 128x64, each with 16 area samples per texel. Uploaded pages use a deliberately
permuted physical-slot order. The GPU reference calls production `EvaluateBase`
and its shared evaluator at the four source texel neighborhoods of each virtual
mip, then interpolates. `MF_TerrainCompositedVTAtMip` samples the real uploaded
pages at explicit target mips 0/0.25/0.5/0.75/1. This tests composition and
quantization against direct GPU source evaluation; it does not independently
prove the shared material equations or execute the actual fragment path.

The generated include removes only the production `#version`, relocates its
include path, renames main and relocates its diagnostic word beyond the eight
readback planes. Derivative functions are stubbed solely to compile the compute
wrapper; derivative-based mip selection is not tested.

Observed on RTX 3080 (18,496 comparison samples):

| Check | Threshold | Observed | Result |
| --- | --- | --- | --- |
| Albedo byte error | <=2 | 1 | passed |
| Local XYZ normal byte error | <=2 | 1 | passed |
| Normal angular error after decode | <=1 degree | 0.77865 degrees | passed |
| ORM byte error, including RMS roughness | <=2 | 1 | passed |
| Adjacent-page overlapping border texels | 0 mismatches | 0 | passed |
| Wrong albedo physical-page order control | must fail material threshold | max error 30 bytes | rejected |
| Height mip2 regular-grid comparison | <=2 R16 units | 0 | passed |
| Height mip2 internal page boundary | <=2 R16 units | 1.001 | passed |
| Height mip2 repeat seam, fractional positions | <=2 R16 units | 173.002 | **failed** |

Clamp versus repeat bake dispatches differ in 12,968 bytes. Repeat composite
sampling parity is not established by this observation.

Height source is 1024x1024 R16, with fixed mip2=256x256, two slices and eight
repeat-bordered pages in a 408x408 atlas (three columns, matching the bounded
production layout). The CPU copies exact mip2 texels, with no CPU material
evaluation. GPU sampling uses the production `SampleTerrainHeightAtlas`.
This does not exercise the production uploader, Store or Cook.

Height values deliberately include sharp discontinuities at repeat seams.
The failing normalized error is 0.002639831. Device subTexelPrecisionBits=8:
different normalized-coordinate transforms for source and physical atlas are a
possible explanation for the fractional-seam difference, not a proven root cause.
The strict two-R16-unit threshold has not been relaxed. The script exits 1 and
records `heightPassed=false` even though `materialPassed=true`.

Distance-fade coverage uses the separately owned
`composited_height_distance.comp`: four-argument `TerrainCompositedUsePage`,
distanceFade 0/1 × strength 0/3, nine distances around 8m and 45m.
`validate_composited_bake_gpu.py` retains all raw readbacks and per-combination
statistics. Observed albedo error <=1 byte, normal/ORM error 0 and route errors 0.

## Real 2x2 source-array comparison

```powershell
py -3 tool/validation/terrain/prepare_composited_s1_comparison.py `
  --output artifacts/terrain-composited-vt/independent-gpu-bake/real-r1024.json
py -3 tool/validation/terrain/prepare_composited_s1_comparison.py `
  --virtual-resolution 4096 `
  --output artifacts/terrain-composited-vt/independent-gpu-bake/real-r4096.json
```

These write only `Generated/Validation/TerrainS1` in the topic assets worktree.
Real tile00/01/04/05 height/Paint inputs are copied there; real layer sources are
referenced. Original tiles, author inputs and preexisting Cook packages are not
changed. Variants have independent Cook paths, and need a real Cook.

Each resolution has strength 0 and 3 pairs. Direct uses full source arrays,
`u_terrainLayerMode=0`, rather than the old layer-VT path. Both sides use static
`u_heightBlendDistanceFade=0`; composite must therefore stay on VT at all distances.
Cameras, lighting, height, Paint, layer parameters and base MIs match.

Parity is deliberately restricted to `lodLevels=1`, with no detail/TES material,
base `M_terrain` (TES macro=0) and displacement multiplier=0. This avoids
confounding canonical macroLOD0 cache normals with decimated mesh normals.
The coordinator's separate audit reports coarse-LOD transition differences up to
64 degrees; this harness did not reproduce that measurement. Stable canonical
local XYZ normals are a technical choice, and multiple LODs still require human
visual review. Severe artifacts require a production decision by the main thread.

R1024 is a fast fixture at 0.984375m/texel, not the final quality target.
R4096 is 0.24609375m/texel, with 304,518,144 material payload bytes per surface,
1,218,072,576 bytes for four surfaces before height/manifest overhead.
Resident capacity remains 64 pages per surface. Streaming Cook supports the
current 4GiB per-surface reader limit; actual R4096 Cook is owned by the main
thread and has not been run here.

The pair driver defaults to preparation. `--stage-only` validates and snapshots
the resource closure and creates fresh GLSL/config/UI/script inputs under a short
private artifacts path, with no shared shader SPV/cache and no junctions:

```powershell
py -3 tool/validation/terrain/run_composited_s1_comparison.py `
  --exe build/bin/main_editor.exe `
  --output artifacts/terrain-composited-vt/independent-gpu-bake/my-new-stage `
  --stage-only
```

Actual execution requires both `--execute` and `--renderer-released`, and must
only follow an explicit renderer release notification. Cook remains owned by the
main thread: this driver requires its completed Validation manifests and copies
each entire Cook directory into private resources. It never starts a Cook.
It then captures matching far/mid/near camera poses and must not run alongside
the main thread's renderer. Captures include validation logs and descriptive
lit-image byte statistics; they do not automatically pass a quality gate.

Historical proposed R4096 procedure: first review close ground, slopes, layer boundaries, two-tile seams and
the four-tile intersection with fixed LOD0/noTES. The then-proposed
terrain-only lit-RGB screening threshold is mean absolute error <=3 bytes and
p95 <=8 bytes became the frozen, executed gate in the final result above.
A terrain mask/ROI must be established before treating these
numbers as acceptance thresholds; full-frame sky/background statistics are
insufficient. Preserve the bounded material limits of 2 bytes/1 degree as the
separate numerical gate. Human review must additionally reject obvious blur,
seams and wrong normal orientation. TES/detail and coarse-LOD images need
separate review and must not be folded into the fixed-geometry cache error.

At the historical preparation stage no renderer parity images had been captured.
That gap is closed by the final masked R8192/192 draw and independent image
review above. Compressed-page error, complete TES/detail alignment, product
multi-LOD visual coverage and complete S0–S5 acceptance are not established by
this fixed-geometry quality record.

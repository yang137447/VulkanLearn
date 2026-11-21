# S1 single-pair masked real draw comparison

Use only the chosen strength3 Cook; the driver defaults to `--pair 3` and
never requires the strength0 package when strength3 is selected.
No renderer or Cook was launched while implementing this driver.

## Main-thread Cook input

```text
Generated/Validation/TerrainS1/Variants/Real2x2_R4096_HeightBlend3/SC_composited_vt_2x2.json
```

The paired direct scene is in the same directory:
`SC_legacy_vt_2x2.json`. It needs no Cook. Four tile packages use
`Cook/tile_00`, `tile_01`, `tile_04`, `tile_05`, each containing 1,372 material
pages. This is one four-surface material set, not two strength sets.

Sparse override correction:

- Strength3 omits `u_heightBlendStrength`, inheriting M_terrain default 3.
- Both strength0/3 omit `u_terrainLayerMode`, inheriting default 0.
- Strength0 retains its nondefault `u_heightBlendStrength=0`.
- Both retain `u_heightBlendDistanceFade=0` and `u_layerDisplacementScale=0`;
  their schema defaults are 1.
- Generator prunes every parameter whose value equals the current schema default,
  including generated direct/composite/unused detail MIs.
- Driver checks effective values using schema defaults, not key presence.
- Private capture overrides are pruned by the same rule.

R4096 strength0 and strength3 were regenerated with this correction. Original
product fixtures, production schema and the eight TerrainS1 author height/Paint
files were not modified. Author hashes match the preexisting freeze manifest.
Do not reuse a partial/stale package from before the correction.

If rebuilding this specific fixture is necessary:

```powershell
py -3 tool/validation/terrain/prepare_composited_s1_comparison.py `
  --virtual-resolution 4096 --pair 3 `
  --output artifacts/terrain-composited-vt/s1-strength3-prepared-new.json
```

Do not rerun preparation unnecessarily after successful Cook, because it writes
author JSON and a new comparison plan.

## Private execution after explicit renderer release

From the code worktree root:

```powershell
py -3 tool/validation/terrain/run_composited_s1_comparison.py `
  --exe build/bin/main_editor.exe `
  --virtual-resolution 4096 --pair 3 `
  --output artifacts/terrain-composited-vt/s1-r4096-strength3-roi-run01 `
  --execute --renderer-released --settle-frames 180 --timeout 900
```

The output directory must be new. `--stage-only` with another new output
directory prepares the short private project/resources without launching a
renderer. Current preparation passed and selected only four strength3 manifests.
The driver does not Cook. It copies completed Cook directories and resource
closures under `artifacts/s1/<8-character-id>/`, with a private GLSL/SPV cache,
no shared-cache writes, and no root shader/schema edits.

Three shots are captured: far, mid, near from the selected capture plan.
Each shot captures mask, base-color and beauty for direct and composite:
18 bounded sequential processes. They share the isolated project/cache, run
with MSAA=1, fixed LOD0, no TES/detail material and zero displacement.

## Raster-derived mask

Each mask uses the same camera and actual terrain geometry, production
`debugview 12` (ShadingModel) and a zero-intensity environment background.
The only visible opaque objects allowed in this fixture are DefaultLit terrain;
the driver rejects unexpected scene object types and authored enabled fog.

`ResolveMaterialDebugView` outputs `DefaultLitID/15 = 1/15` on terrain.
With MSAA=1, the resulting palette must contain only black sky or constant
DefaultLit gray (17 for linear framebuffer or approximately 73 for sRGB).
Any other nonblack/chromatic value fails mask validation instead of being
silently treated as terrain. Empty masks fail.

Both branches' raw mask images and binary PGM masks are saved. The comparison
uses their union so an uncovered/wrong silhouette cannot hide an error.
Any direct/composite silhouette XOR pixel also fails the screen gate.
The mask changes only scene environment intensity; TR/material identities
and geometry stay unchanged.

This is actual raster coverage, not a projected AABB that contains sky or
occluded background. Mask generation occurs before inspecting color differences.

## Base-color diagnostic and beauty

Base-color capture uses production `u_debugMode=3`, which emits albedo as
emissive. Since the production shader applies detail/macro/global color before
this branch, both private MIs temporarily use:

```text
u_debugMode=3
u_detailStrength=0
u_macroStrength=0
u_globalColourWeight=0
u_globalColourWeightNear=0
u_globalAoStrength=0
u_globalWetnessStrength=0
```

Those are retained parameters excluded from the production static bake identity.
Static strength, UV, fade and weight selection remain unchanged. All temporary
parameters are serialized sparsely and original private files are restored.
Author resources remain unchanged.

`debugview 2` displays the emissive channel, but it is not a raw-debug tone-map
bypass in the current shader. Therefore the driver explicitly uses `tonemap 0`,
`bloom strength 0`, and EV100=-log2(1.2), compensation=0, giving camera exposure
scale 1. The fixture tone-mapping saturation is 1. This produces clamped
base-color output through normal framebuffer encoding; it is a masked RGB8
rendered-color screen, not an HDR/linear-float material readback.

Beauty restores original authored material, exposure and environment settings,
with `debugview 0`. Its image screen is recorded separately and cannot stand in
for base-color comparison. Normal/ORM numerical evidence remains the bounded
GPU suite; the driver does not invent an independent normal/ORM render gate.

## Frozen ROI decision

The driver always checks:

1. The union mask of all visible terrain pixels.
2. Every cell of a predetermined 4x4 screen grid containing at least 64 terrain
   pixels, intersected with that same union mask.

For every ROI, mean absolute RGB8-component error must be <=3 and nearest-rank
p95 must be <=8. The grid, minimum sample count and thresholds are fixed before
capture; it does not pick the most convenient patch after looking at differences.
Each channel/shot records ROI rectangles, sample counts, mean, p95 and maximum.
Raw images, union masks and masked maximum-component difference PGMs are saved.
No full-frame average is used to pass this gate.

CPU checks verify that a 64-pixel terrain mismatch hidden in otherwise identical
sky fails both terrain-union and local-grid gates despite a passing full-frame
average. Unknown sky colors are rejected, identical terrain passes, and sparse
capture overrides preserve effective static parameters.

Any mask, silhouette or ROI failure is reported as failure with nonzero exit.
Logs/images remain available for root-cause investigation. Do not raise
thresholds, dilute the ROI with sky or replace the failed ROI after seeing it.
Fix actual defects through the owning production agent and rerun.

A passing screen yields `screened-pass-awaiting-human-review`, never complete
S1 acceptance. Human review must inspect near/mid/far terrain, slopes, boundaries,
tile seams and the four-tile intersection. Product-normal geometry LOD/TES/detail
and its earlier coarse-LOD frame differences require separate visual review.

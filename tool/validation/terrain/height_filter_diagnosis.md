# Height mip2 filtering diagnosis

Historical hardware-filter diagnosis. The main thread subsequently selected
canonical four-fetch float filtering; see
[current independent-oracle acceptance](canonical_height_acceptance.md).
The padding counterfactual and original failed records below are preserved.

This follow-up is validation-only. No production source, original source image,
Cook output or renderer was modified or launched. Original
`bounded-final/report.json` remains failed, with SHA-256
`d190cda1f43a72a91c417c73a4b479c872bbaf99329487ce923b5c33360864bf`.
The original two-R16-unit acceptance threshold is unchanged.

## Evidence and reproduction

Final evidence is under
`artifacts/terrain-composited-vt/independent-gpu-bake/height-diagnosis-seven-layer-final/`.
`report.json` links every case and records source/evidence hashes. Each case has
`samples.bin`, `gpu-source-mip2.bin`, `gpu-height-atlas.bin`, `analysis.json` and
its exact input bytes. `commands.json` retains actual compiler/dispatch commands,
device name, driver version, subTexelPrecisionBits and stdout.

```powershell
py -3 tool/validation/terrain/diagnose_composited_height.py `
  --output artifacts/terrain-composited-vt/independent-gpu-bake/height-diagnosis-new-run
```

The output directory must be new. `--synthetic-only` is available for fast
reproduction of the original discontinuity case. No engine build/cache is used.

The independent CPU reference evaluates double-precision bilinear interpolation:
`p=fract(uv)*sourceDim-0.5`, `base=floor(p)`, `f=p-base`, four repeat neighbors,
then the two horizontal lerps and vertical lerp. It uses the actual GPU mip2
readback, not a CPU approximation of GPU mip generation. A second CPU reference
uses the actual atlas readback and shader-emitted normalized atlas UV, including
that transform's float rounding. The CPU oracle also passed known 2x2 center,
negative-repeat and fractional interpolation anchors.

The local test shader outputs eight float planes: hardware atlas/source,
manual atlas/source, original UV.xy and normalized atlas UV.xy. Manual sampling
uses canonical source texel coordinates and shared fractional weights; the atlas
uses four `texelFetch` operations in the mapped page's real border. No normalized
atlas-coordinate filtering is used for the manual branch.

Atlas pixels are populated by real GPU mip2 image copies using production
`BuildTerrainHeightAtlasLayout` and `BuildTerrainHeightAtlasCopies`. All stored
page/border texels are independently checked against the GPU mip2 readback.
Production `SampleTerrainHeightAtlas` is called unchanged in the hardware branch.
The production uploader class, Store, Cook and renderer wiring are not exercised.

## The original 173-unit sample

Pixel `(4,16)`, source slice 0:

| Quantity | Value |
| --- | --- |
| Original UV | `(-0.00096893310546875, 1.0)` |
| Source p | `(255.251953125, -0.5)` |
| Source neighbors | `(255,255), (0,255), (255,0), (0,0)` |
| Neighbor R16 values | `6350, 27771, 58117, 14002` |
| Axis weights | `0.251953125, 0.5` |
| Four weights | `0.3740234375, 0.1259765625, 0.3740234375, 0.1259765625` |
| CPU exact source | `29374.587890625` R16 units |
| CPU exact atlas using emitted float UV | `29374.403507711133` R16 units |
| Hardware source | `29223.999382853508` R16 units |
| Hardware atlas | `29397.000649273396` R16 units |
| Hardware source versus CPU source | `150.588507771492` R16 units |
| Hardware atlas versus CPU atlas | `22.597141562263` R16 units |
| Hardware pair difference | `173.001266419888` R16 units |
| Manual GPU source and atlas | both `29374.588881880045` R16 units |
| Manual GPU versus CPU source | `0.000991255045` R16 units |

The actual atlas four neighbor values match the source in order. The exact
CPU transform difference is only 0.184383 units; it cannot account for the
173-unit hardware difference as a misplaced texel or copied border error.

On this RTX 3080, `subTexelPrecisionBits=8`. Hardware source is itself different
from exact bilinear interpolation. With only the test atlas dimensions padded
from 408x408 to 512x512, the hardware pair difference becomes zero at this
sample and all 18,496 test samples. Source values and producer copy/page math
are unchanged.

This localizes the discrepancy to the normalized-coordinate/fixed-function
filtering path rather than an incorrect helper page/texel address or stored
height value. It does **not** prove that a single nearest-8-bit-weight rounding
operation reconstructs every hardware result: the lower/upper weight-grid
diagnostic has residuals up to approximately 128.49 R16 units on the stress
input. The report retains those residuals rather than claiming bit-level
knowledge of the GPU filtering implementation.

## Real source image planes and full seven-layer atlas

All seven real 1024x1024 `layer_*_height16.png` planes are decoded as R16, with the
asset's default `flipY=true`, then uploaded. Two GPU linear blits generate mip1
and mip2; actual mip2=256x256 is read back. PNG, texture descriptor and TR hashes
are recorded. Each pair of real planes is tested separately, then all seven
are tested together in the production 28-page layout (816x680).

Across all 12 dispatches / 221,952 sample points:

- Actual page/border storage mismatches: **0**.
- Canonical atlas versus source four-neighbor-value mismatches: **0**.
- Manual GPU atlas/source float-bit mismatches: **0**.
- Maximum manual GPU versus exact CPU error: **0.004917 R16 units**.
- Device-precision diagnostic-envelope violations: **0**.

The native normalized atlas UV can change the integer neighbor window at an
exact zero-weight boundary: seven samples in the full seven-layer case.
Those raw window changes are retained in the report. The canonical neighbor
window is correct, the corresponding near-zero coefficient is recorded, and
the CPU weighted difference is measured separately; this is not hidden as
an address failure or discarded sample.

Native 816x680 hardware source/atlas differences:

| Layer | Displacement amplitude | Max R16 error | Max displacement error |
| --- | ---: | ---: | ---: |
| Gravel Ground | 0.06m | 9.00377 | 0.008243mm |
| Rock Face | 0.25m | 14.00174 | 0.053413mm |
| River Water | 0m | 0 | 0 |
| Scree Debris | 0.12m | 2.99995 | 0.005493mm |
| River Silt | 0.02m | 12.99980 | 0.003967mm |
| Sea | 0m | 0 | 0 |
| Beach Sand | 0.01m | 36.99944 | 0.005646mm |

Physical errors use `abs(deltaHeight)/65535 * displacementM * 1000`, with global
displacement scale=1, full distance fade and a unit direction. Multiply by the
actual global scale/fade/direction length when evaluating deployed geometry.
The conservative diagnostic bound for Rock Face is 0.808366mm; its measured
pair difference is 0.053413mm. These are sampling results, not a renderer/TES
measurement.

The expanded artificial corner-discontinuity test reaches 286.00150 R16 units,
or 4.364103mm for a hypothetical 1m amplitude. That amplitude is not asserted
to be a real asset setting. The original 173-unit window and both failures
remain recorded.

## Error envelope, not a replacement acceptance threshold

For each sampler's actual four neighbors in R16 units, let:

```text
Lx = max(abs(q10-q00), abs(q11-q01))
Ly = max(abs(q01-q00), abs(q11-q10))
coordinateEnvelope = (Lx+Ly) / 2^subTexelPrecisionBits
samplerEnvelope = coordinateEnvelope + 0.5 R16 rounding cell + 0.05 R16 float allowance
pairEnvelope = sourceEnvelope + atlasEnvelope + abs(CPUatlas-CPUsource)
```

The hardware values in this run are within 0.001953 R16 units of an integer.
Real low-gradient neighborhoods demonstrate the need to retain the half-R16
result-rounding cell in addition to the gradient term; the earlier gradient-only
diagnostic underbounded some samples, preserved in `height-diagnosis-real-v1`.

Every final sample lies within the stated envelope. This is evidence about the
tested device and inputs, not an implementation-independent proof of all texture
filter arithmetic. `diagnosisStatus=complete` means storage/address/manual
checks and diagnostic-envelope checks completed. It does not turn the unchanged
two-R16-unit acceptance gate into a pass: native layouts still fail that gate.

## Minimal production fix recommendation to the main thread

Prefer testing **power-of-two physical height-atlas dimensions** first. The local
counterfactual leaves the production sampling helper unchanged:

1. Pad width to a power of two.
2. Recompute `columns=width/136`, and reflow page-copy destinations using those
   columns; changing width without this step can break helper addressing.
3. Compute required rows and pad height to a power of two.
4. Recompute byte budgets and stored layout metadata/dependency identities.

The two-source test changes 408x408 to 512x512. The full real seven-source layout
changes 816x680 to 1024x1024, with columns 6→7; all sampled hardware source/atlas
differences become **zero** in both cases, including the original extreme input.
Seven-source R16 atlas storage increases from 1,109,760 to 2,097,152 bytes
(+987,392 bytes, excluding the unchanged layer map). This is a tested candidate,
not a production patch made here or a guarantee for arbitrary floating UVs,
all page counts, non-power-of-two source mips or other devices.

If canonical mathematical bilinear values are required instead of agreement
with legacy hardware filtering, the alternative is a shared manual four-fetch
bilinear helper in canonical source texel space. Apply that semantic choice
consistently to atlas and direct LOD2 sampling; changing only atlas to exact
bilinear can still disagree with the legacy approximate source sampler.

After a production choice, retain the original strict gate and rerun raw cases,
vary page counts/source extents/negative and arbitrary fractional UVs, and
perform renderer TES/detail alignment and visual checks when its owner releases
the renderer. No complete S1 or renderer-quality acceptance is claimed here.

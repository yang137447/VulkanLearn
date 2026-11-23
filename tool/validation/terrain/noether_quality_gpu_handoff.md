# Noether QualityGpu — minimum raw capture / adapter interface

2026-11-21. Implementation handoff, not a runtime acceptance receipt.
No Service/Runtime edit requested here. Main can forward this file to Noether.
Full context: `rvt_evidence_protocol.md`; Python boundary: `rvt_sample_evidence.py`.

Update: Noether's `terrainCompositedQualityData.h` ABI v1 is now landed.
`rvt_quality_binary.py` directly decodes exact1606912B, no expanded traversal JSON.
`samplesArtifact.format="terrain-quality-v1"` selects it. The log quality event also
binds ordered surfaceIds,renderExtent,captureSequence,submission/completedEpoch and
numeric uint64 poseId (or decimal string). Raw digest dictionary bytes retain their
original32-byte order. CPU negatives include wrong identity,fence/version/truncation,
missing resolved bit and bias1 raw+2 rejection. Actual runtime capture remains unverified.

## Capture scope and minimum records

`all-fragment-taps` = every **actual anisotropic tap of each captured visible
feedback sample**, not every screen fragment. Domain is one World128×128 feedback
grid (<=16384 positions), depth/visibility tested. Each included terrain sample
has1..8 taps, both low/high virtual mip resolutions. Background is excluded; failed
terrain samples cannot be excluded. Report actual visibility mask/coordinates.

Minimum semantic fields; compact indices into immutable dictionaries are preferred:

| Record | Fields |
| --- | --- |
| Capture header | `protocol/abiVersion`, `frame`, `poseId`, `worldGeneration`, `drawSubmissionEpoch`, `completedEpoch`, `samplingDomain="feedback-visible-terrain"`, `feedbackExtent=[128,128]`, `renderExtent`, `captureSequence`, `sampleCapacity=16384`, `sampleCount`, `terrainVisibleCount`, `overflowCount`, `droppedTerrainSamples`, `uninitializedDraws` |
| Per captured sample | `feedbackPixel(x,y)`, `surfaceIndex`, `targetMip` **before budget bias**, `effectiveMipBias`, `tapCount`, `expectedSurfaceRevision`; UV/footprint/majorUv for optional address/reference diagnostics |
| Per tap, low and high | `requestedMip` (or derive unambiguously from target+bias), `resolvedMip`, resolved `pageX,pageY`, raw page-table `entry` (`0` absent, otherwise `slot+1`); optional route/anchored address diagnostic flags |
| Committed slot owner dictionary | `slot`, `surfaceIndex`, `mip,x,y`, `contentIdentity` (full256bit value or immutable identity-table index), `revision`, `worldGeneration`; **state captured for that draw epoch** |
| Surface dictionary | stable `surfaceId`, `width,height`, `contentIdentity`, expected revision, generation; mapped to verified V2 publication / actual active refresh identity |
| Capture accounting | actual Vulkan buffer allocation sizes GPU/host, Workspace/CPU owner high-water, capture GPU timestamp, requested/completed/deferred capture counts and bytes |

`entry=0` must be explicit and retains the unresolved sample in the denominator.
Both branches are exported even when requestedMip is the same at maxMip.
Resolved x/y refer to the actual virtual ancestor page selected by shader, including
AddressAnchored behavior; slot owner must identify that exact key.
Missing/unknown owner is reported, never replaced with expected identity.
No `valid=true`, `qualityPass=true` or `identityMatches=true` as substitute records.

Expected identity comes from verified publication/active revision at draw binding,
actual identity comes from the epoch-bound committed owner metadata. Do not take
both values from the latest CPU state after readback. GPU capture must bind the
same page-table view/version and slot owner generation as the sampled draw.
Completion fence must cover capture and the referenced draw epoch; simply labeling
a later unrelated feedback pass with the draw frame is insufficient.

## Minimum producer/consumer API boundary

Noether can use existing C++ conventions for exact method/type names. Provide:

1. Immutable CaptureRequest with draw/page-table/owner bindings, surface dictionary,
   actual render footprint data, epoch, camera/pose/frame and budgets/capacity.
2. `Record(command, request, slot)` records bounded work; does not submit/waitIdle,
   mutate residency, or acknowledge readiness.
3. Submission notification binds the actual fence/epoch. `Poll(completedEpoch)`
   returns Pending/Complete/Failed with an immutable **bounded binary readback**;
   no output while pending, and no slot reuse before completion/retirement.
4. ABI document or shared header: magic/version/endian/struct strides, fixed
   capacities/offsets, integer packing, overflow semantics and digest/dictionary
   encoding. Include total buffer requirements and precise valid byte range.
5. Return capture metadata and actual counters. Service later owns quality/loading
   decisions; P5 adapter independently derives counts and checks metadata.

P5 adapter will expose a decoder from the agreed binary ABI to the semantics above,
then call `quality_counts(raw,event,verifiedSurfaces)`. **Do not expand16384×8×2
traversal arrays into per-frame JSON.** Current expanded JSON compatibility path
requires traversal; P5 must remove that requirement for the binary final-result
mode rather than asking Noether to emit fabricated traversal. Bounded witnesses
can preserve actual walk details for selected failures only.

Raw artifacts are `{path,bytes,sha256}` under the external run evidence root.
Runtime log `quality` links the artifact and reports recomputable current counts.
Python recomputes visible/validAtTargetPlusBias/unresolved/incorrectIdentity/
rootOnlySamples/effectiveMipBias from **all included samples and all their taps**.
It verifies frame/pose/epoch/slot owner and counts, then applies unchanged99%,
bias<=1,zero unresolved/incorrect identity/no root-only-ready thresholds.
Require sampleCount=terrainVisibleCount, zero overflow/drop; capture failure is
missing evidence and cannot release loading.

## Bounded implementation suggestion, not additional budget

One capture globally in flight, <=4Hz, fence-polled. Candidate output2MiB:
sample headers524288B, packed tap pairs1048576B,512×64B owners32768B,
optional bounded128KiB witnesses, remainder for header/counters.
One output+one host readback proposes4MiB payload. Actual Vulkan requirements,
all metadata and retirement overlap must pass the existing32MiB Workspace cap
and CPU reservations; no free extra diagnostic allocation. Reject if insufficient.
Bounded output queue/run byte cap; never build unbounded per-frame history.

This proposal is adjustable to a smaller ABI within the same quality domain,
but no capacity overflow, dropped samples/taps, or quality threshold relaxation
can be used to claim success. No capture currently counts as implemented evidence.

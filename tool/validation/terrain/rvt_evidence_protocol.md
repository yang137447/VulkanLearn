# RVT native evidence protocol v1 — Euclid / P5 handoff

Implementation handoff, not evidence of runtime completion. Consumer:
`rvt_native_adapter.py::verify_case`. Emit one compact UTF-8 JSON object per line,
prefixed **`[RVT_EVIDENCE] `**. Every event carries:
`protocol:1,event:string,runId:string,sequence:uint64,monotonicMs:number,frame:uint64,
worldGeneration:uint64`. Time is process-relative steady-clock milliseconds.
Sequence increases in actual observation order, including fence retirement/readback.
Do not emit booleans that claim a test passed.

## Required events and exact payload fields

The following fields are in addition to the envelope. Missing evidence blocks its case.
Numbers must be finite; counts/epochs are nonnegative integers. Identity digests are
64-character lowercase hex. `surfaceId` is `terrain|Landscape_XX`.

| event | Payload fields | Real observation / consumer |
| --- | --- | --- |
| `identity` | `scene,worker,weightSource,contentIdentity,shaderSha256,frozenSha256` | Once after inputs verified. worker is actual launch mode, weightSource is `CoverageAtlas`/`LocalLayerArray` from restored closure. contentIdentity is **SHA256 of canonical full V2 manifest JSON**, sorted keys, separators `,`/`:`, UTF-8 with non-ASCII escaped (Python canonical_hash). shaderSha256 is actual package `shader/runtime-shader.manifest.json` bytes SHA256. frozenSha256 is actual loaded config bytes. |
| `cache` | `mode,osCacheCleared,contentIdentity,initialFineBytes,peakBytes,deletedBytes,rejectedCorruptEntries` | Actual optional disk-cache owner/accounting. mode=`disabled`/`bounded`; osCacheCleared=false. Disabled has all byte/count fields zero; memory residency still starts empty in fresh process. |
| `pose` | `name,category,position,lookAt,previousVisits,poseId` | When actual camera snapshot receives pose. category=`center`/`edge`/`junction`/`outer`/`overview`/`teleport`; count actual visits, not scheduled argv. |
| `crossing` | `distanceCm,intermediateSamples,fromPoseId,toPoseId` | Actual camera snapshots traversed; at least 24 intermediate samples for continuous route. |
| `quality` | `phase,reason,poseId,elapsedSincePoseMs,visibleSamples,validAtTargetPlusBias,effectiveMipBias,unresolved,incorrectIdentity,rootOnlySamples,uninitializedDraws,samplesArtifact` | phase=`loading`/`ready`; reason=`initial`/`pose`/`teleport`; GPU resolved sampling described below, independently recomputed by adapter. Counts are current observation, never cumulative. |
| `loading` | `reason,surfaceId` | Actual draw/loading gate held; reason=`root-not-ready` for damaged root recovery. Must precede GPU publication and root-ready. |
| `root-ready` | `surfaceId,mip,x,y,contentIdentity,revision` | Actual validated root page-table publication, after fence/status success, not queued work. |
| `publication` | `surfaceId,mip,x,y,contentIdentity,revision,source,status,writeEpoch,fenceCompletedEpoch,publishedEpoch,slot` | source=`gpu`/`package`/`cache`; real producer status and submission/fence/page-table commit. GPU success: status=0, fenceCompletedEpoch>=writeEpoch, publishedEpoch>writeEpoch. |
| `integrity` | `kind,surfaceId,mip,x,y,path,expectedDigest,observedDigest` | Actual failed payload verification. kind=`root`/`coarse`/`input`. Root key must match damaged payload. |
| `rejection` | `kind,path,candidateGeneration,activeGeneration` | Real failed input/manifest/config validation before candidate commit. kind=`input-digest`/`v1`/`legacy-switch`; input damage must exit1 and clean teardown. |
| `production-failure` | `faultId,surfaceId,mip,x,y,status,submitResult,publishedChannels,attempts,maxAttempts,backoffTicks,slot,writeEpoch` | Actual injected nonzero GPU status or submission return; publishedChannels=0 on failure; attempts/backoff from real retry owner. Emit distinct faultIds for both failure modes. |
| `slot-retirement` | `faultId,slot,completedWriteEpoch,reuseEpoch` | Observed reuse after failed write retirement; reuseEpoch>completedWriteEpoch. Never synthesize epochs. |
| `admission` | `frozenSha256,peakTerrainGpuBytes,peakRvtGpuBytes,peakTerrainCpuBytes,oldestWaitMs,candidateResult` | Actual cross-World Active+Candidate+Retiring owner, allocation requirements and leases; overload may reject candidate, never raise caps. |
| `world` | `scene,generation,stalePublications,peakTerrainGpuBytes,candidateResult,activeGenerationBefore,activeGenerationAfter` | Actual successful commit/rejection. Need A→B→A and failed candidate retains old. Rejected candidate is an additional event; don't infer roundtrip from command names. |
| `tes` | `surfaceId,weightSource,distanceCm,activeRadiusCm,displacementCm,heightInputsValidated,heightInputsExpected,heightErrorR16,baseFineResidentPages,samplesArtifact` | Real TES output/height inputs and independent height comparison at active radius, nonzero displacement. Capture base fine residency zero separately. No isolated TES GPU timing claim. |
| `comparison` | `poseId,mip,crossBoundarySamples,distinctLayerMaps,albedoMaxByteError,normalMaxByteError,ormMaxByteError,roiMaeBytes,roiRmseBytes,roiP99ErrorBytes,artifacts` | Actual GPU output readback vs independent evaluator plus locked lit image ROI. artifacts lists raw image/channel/reference/mask identities; error numbers independently recomputed offline. |
| `editor` | `reviewer,implementer,reviewReceiptSha256,inflightTasksAtEdit,stalePublications,artifacts` | P4 independent receipt plus observed edits; runtime does not invent reviewer: runner binds verified P4 external receipt. |
| `file-access` | `path,class,operation` | Observed resolved file opens. class=`package`/`system`/`runtime-cache`; actual paths checked independently. Include failed workspace attempts, not just successful opens. |
| `installation-mode` | `installWriteAttempts,writeProbeExit,cacheRoot` | Runner-owned real ACL write probe under same account; not an in-process assumption. cacheRoot outside install, installWriteAttempts from file access. |
| `installation` | `tool,exitCode,argv,logArtifact` | Runner-owned actual native `vl_export_game --verify-package` outcome (including corrupted coarse/root rejection). Never emit expected outcome as observed. |

Artifact references are `{path,bytes,sha256}` relative to the run evidence directory;
paths reject absolute/traversal/symlinks. `samplesArtifact` is raw GPU readback JSON.
Runner must include it in the case's hash-bound artifacts. `installation`, external P4
and ACL probe evidence belong to separate runner artifacts; don't contaminate runtime
stdout with synthetic events. Adapter integration must read both sources explicitly.

## Bounded capture domain and budget — Euclid implementation guidance

`all-fragment-taps` means **all actual taps for each captured sample**, not every
full-screen fragment. The denominator is visible terrain in the **128×128 feedback
domain**, at most16384 positions across the whole World, not per surface. Report
that domain, render dimensions, depth/visibility mask and camera explicitly.
Use existing feedback visibility and full-render derivative scaling. Include all
1..8 actual anisotropic taps and both requested mips for each captured terrain sample.
Exclude background, never selectively drop failed terrain. The99%, bias<=1,
zero unresolved/incorrect identity and root-only thresholds remain unchanged.

Proposed implementable bounded capture, subject to actual budget admission:

- Capture at most4Hz (250ms), including requested loading-release/arrival observations.
  One capture in flight globally. If busy defer, record gap; no waitIdle or per-frame
  full readback. Use the actual qualifying capture's monotonic time for convergence;
  never interpolate or backdate readiness. Failed capture cannot release loading.
- Fixed binary records, no per-sample JSON in the render loop. Candidate2MiB GPU
  output layout:16384×32B headers=524288B;16384×8×2×4B resolved tap results=1048576B;
  512×64B epoch-bound owner dictionary=32768B; bounded128KiB traversal witnesses;
  header/counters in the remaining space. Every sample header includes feedback
  pixel/surface/target mip/UV/footprint/tapCount/valid mask. A32-bit result can pack
  slot+1(10bits,0 absent),actualMip(4bits),x/y(6bits each),remaining flags.
  Specify exact ABI/version before implementation. Full identity/page key/revision/
  generation is held once per committed slot in the epoch-bound dictionary.
- One2MiB output plus one2MiB host-visible readback, no extra frame ring, is a
  **4MiB payload proposal**, not guaranteed allocation. Query both Vulkan requirements,
  include parameters/descriptors/counters/dictionary copies and reserve Workspace
  costs before allocation. Count mapped host bytes in CPU readback reservations.
  Account old in-flight/retiring buffers during World replacement. Frozen32MiB cap
  stays unchanged; P0's historical ~4.81MB remaining does not prove current fit.
  If admission fails, reject diagnostic capture and report missing evidence; never
  enlarge caps, hide costs, silently truncate or lower quality.
- Full traversal lists are only a bounded diagnostic witness subset (e.g.128KiB),
  not required for every sample. All sampled terrain pixels still export every
  tap's final result produced by the actual GPU Address/AddressAnchored walk.
  CPU independently recomputes99% and identities using results+owner dictionary.
  CPU-only reconstructed traversal and cumulative histograms are insufficient.
- Fence-completed binary readback persists through a bounded queue, no expanding
  per-frame history. Set run capture-count and output-byte limits; queue/disk failure
  stops capture with explicit incomplete evidence. Record requested/captured/dropped.
  Export selected evidence windows, not all frames of a long tour.
- Record actual capture GPU timestamp, bytes/dispatch, allocation requirements and
  Workspace/CPU high-water. Separate instrumented correctness and uninstrumented
  formal performance runs, bind equal package/input/camera conditions, report overhead.
  No estimated overhead subtraction; both runs respect the original frozen caps.

Quality artifact metadata required:
`samplingDomain:"feedback-visible-terrain",feedbackExtent:[128,128],renderExtent:[W,H],
sampleCapacity:16384,sampleCount,terrainVisibleCount,overflowCount,droppedTerrainSamples,
captureIntervalMs,captureSequence,captureScope:"all-fragment-taps",
captureGpuAllocationBytes,captureHostAllocationBytes,workspacePeakBytes,cpuPeakBytes`.
Qualifying observations require sampleCount=terrainVisibleCount<=capacity,
overflowCount=0,droppedTerrainSamples=0. Coordinates/mask show actual domain coverage.
An arbitrary smaller subset cannot claim complete feedback-grid sampling.
The16384 domain and4Hz are capture settings, not relaxed quality thresholds.

The authoritative compact ABI is now `source/render/resource/terrainCompositedQualityData.h`,
magic0x31515654,version1,exactly1606912B. `rvt_quality_binary.py` decodes this layout
directly; `samplesArtifact.format="terrain-quality-v1"` selects it. Log event also
provides ordered `surfaceIds`, numeric uint64 `poseId` (or decimal string),
submissionEpoch/completedEpoch,renderExtent,captureSequence. Identity dictionary
bytes are raw32-byte digest order, not a hexadecimal string's ASCII bytes.
Requested pair is derived from target+bias; actual mip/page/entry is decoded from
GPU packed words. Bit26=resolved; bit27=anchored; remaining high4bits reserved.
Header/sample/owner dictionaries stay bound to the actual draw epoch.
CPU decoder negatives do not establish runtime capture or budget viability.

## quality samplesArtifact: actual sampled page-table/identity resolution

JSON root (`captureScope:"all-fragment-taps"` is mandatory for quality):
`{protocol:1,source:"gpu-page-table-readback",frame,submissionEpoch,completedEpoch,
worldGeneration,poseId,captureScope:"all-fragment-taps",uninitializedDraws,samples:[...]}`.
completedEpoch>=submissionEpoch; frame/generation/pose match the quality event.
Readback is fence-completed **from the draw/feedback submission that used this page table**.
Every captured visible feedback terrain sample has (expanded diagnostic form):

```json
{
  "surfaceId":"terrain|Landscape_00", "uv":[0.5,0.5],
  "feedbackPixel":[64,64], "tapCount":1,
  "targetMip":2.25, "effectiveMipBias":0, "maxMip":13,
  "expectedContentIdentity":"<surface contentHash from V2 surfaces[]>",
  "expectedRevision":1,
  "taps":[{"low":{"requestedMip":2,"resolvedMip":3,"x":4,"y":4,"entry":18,
    "slotOwner":{"slot":17,"surfaceId":"terrain|Landscape_00","mip":3,"x":4,"y":4,
      "contentIdentity":"<same surface contentHash>","revision":1,"worldGeneration":1},
    "traversal":[{"mip":2,"x":8,"y":8,"entry":0},{"mip":3,"x":4,"y":4,"entry":18}]},
  "high":{"requestedMip":3,"resolvedMip":3,"x":4,"y":4,"entry":18,
    "slotOwner":{"slot":17,"surfaceId":"terrain|Landscape_00","mip":3,"x":4,"y":4,
      "contentIdentity":"<same surface contentHash>","revision":1,"worldGeneration":1},
    "traversal":[{"mip":3,"x":4,"y":4,"entry":18}]}}]
}
```

GPU capture uses the actual `TerrainCompositedAddressAnchored`/Address traversal,
including center/tap addressing when anisotropic taps are enabled. `entry=0` is absent;
entry>0 means physical slot=`entry-1`. Slot owner metadata must be the committed
generation/revision/content/page key associated with that GPU epoch, not latest CPU
residency estimated after the fact. Capture all actual taps or explicitly label a
center-only diagnostic as insufficient for full-fragment quality.

Adapter derives requested pair from footprint target and effective bias<=1.
**Total coarse tolerance is the raw, pre-bias target pair plus1**, not biased
requested pair plus1: `goodMip <= min(maxMip, requestedMip - effectiveBias + 1)`
(Noether Recompute's exact boundary). Bias1 does not permit raw+2. Every actual
tap's low/high branch must satisfy this bound; one failure makes the sample not-good.
Missing/unknown owner is unresolved,
wrong surface/page/revision/content is incorrectIdentity. Root-only means both resolve
to maxMip when target<12. At least 99% current visible samples meet target+1; unresolved
and incorrect identity zero. Root-only cannot release loading. Do not use cumulative
feedback histograms or queue-empty as this measurement.

## P4 ticket trace

`editor` events additionally carry `ticketId,editSeq,sourceRevision,expectedRevision,
publishedRevision,surfaceId,ticketState,diagnostic`. These are copied from the actual
QueueRefresh/PollRefresh DTO (`artifacts/p4-editor-handoff.md`), not generated by
P5. ticketState is `Queued/Prepared/Submitted/Published/Failed/Stale`.
Only exact current ticket/world/editSeq/source revision Published acknowledges dirty;
Published must advance expectedRevision. Retain all transitions for stale and failed
requests and bound retries. Include actual in-flight count at edit.

## Raw timing / faults / applicability

- Retain `[GPU_RAW] {"frame":N,"passes":[{"name":"geometry","ms":...},...]}` and
  `[CPU_RAW] {"renderFrameIntervalMs":...}` per rendered frame. GPU timestamps only
  after query completion; include query period/validBits metadata in run identity.
- `geometry_including_tes`: entire geometry draw timestamp, p99<=**3.754 ms**,
  >=1000 raw frames. It includes other geometry; never separate TES GPU cost.
  Retain instrumented pass sum, production, copy, same-task production+copy and
  per-frame production+copy timestamps. P0 JSON retains all thresholds.
- Root/coarse corruption: installation rejects; runtime input-valid rebuilds; damaged
  root holds loading until root-ready, never uninitialized draw. Input corruption:
  reject candidate/World, no fallback. Record real injected fault target and returned state.
- Disabled optional disk fine cache: cache_deleted/cache_corrupt **not-applicable**,
  independently reviewed exclusion with zero disk counters; not skipped-as-pass.
  Cold fresh process/residency, unvisited teleport and real GPU production still required.
- Existing runtime does not yet implement all these events. Missing telemetry is
  incomplete evidence; this protocol is not permission to replace observation with literals.

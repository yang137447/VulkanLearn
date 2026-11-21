# Noether TVQ1 quality capture ABI v1

Implementation handoff, not a GPU validation receipt. All offsets below are bytes,
little-endian; no native pointers, padding, or traversal JSON. Frozen quality
thresholds are unchanged. GPU execution needs an explicit main-thread window.

## Fixed layout

Exactly **1,606,912 bytes**, both output and readback. The two payloads total
**3,213,824 bytes** before Vulkan requirements/alignment and metadata. One capture
globally in flight; at most one attempt recorded per 250 ms; default run limit 256
captures and 411,369,472 raw bytes. Aborted recordings consume an attempt. No frame
wait for a busy/not-ready slot. Keeping a readback alive blocks reuse of its memory.
The in-flight token,250ms admission clock and run capture counter are shared across
all module instances, so Active/Candidate/Retiring World owners cannot create a
second concurrent capture or reset the run cap during World replacement.

| Region | Byte offset | Count | Stride |
| --- | ---: | ---: | ---: |
| Header | 0 | 1 | 256 |
| Samples, fixed feedback pixel index | 256 | 16384 | 32 |
| Resolved results, 8 taps × 2 branches per pixel | 524544 | 16384 | 64 |
| Epoch-bound physical owner dictionary | 1573120 | 512 | 64 |
| Expected surface dictionary | 1605888 | 16 | 64 |

Only `surfaceCount` surface rows are meaningful. Unused samples/results/owners are
zeroed before capture. Pixel index = `y*128+x`, for one World-wide feedback grid.
Background has `claims=0`; visible terrain has exactly one claim. Multiple equal-depth
terrain fragments at one pixel fail the observation rather than racing/choosing one.

Header is 64 uint32 words:

| Word | Value |
| --- | --- |
| 0,1,2 | magic `0x31515654` (file bytes `54 56 51 31`, TVQ1), ABI 1, capacity 16384 |
| 3 | actual unique visible-terrain claims |
| 4,5 | duplicate count, invalid-input/result count |
| 6,7 | actually resolved sample count, renderer-observed uninitialized draws |
| 8..9,10..11,12..13 | uint64 frame, poseId, World generation, low word first |
| 14..15 | uint64 actual draw/capture submission epoch |
| 16,17,18 | render width, render height, surface count (1..16) |
| 19..20 | uint64 capture sequence |
| 21,22 | dropped terrain samples, overflow count |
| 23..63 | reserved zero |

Sample at `256+pixel*32`: six float32 values `(uv.x,uv.y,majorUv.x,majorUv.y,
targetMipBeforeBias,effectiveMipBias)`, then uint32 `surfaceIndex | tapCount<<16`,
then uint32 atomic claim count. `surfaceIndex<surfaceCount`, `1<=tapCount<=8`,
finite fields, bias exactly 0 or 1. Current material uses the unmodified footprint,
so the capture shader writes its actual bias 0. Future effective bias must change
the real material sampling and capture together; it cannot relax target+1.
Runtime queue-budget mip bias is not a quality sample input. In the current
implementation it changes queue policy only; report that policy in a separate
event. It must never replace the captured shader bias0, suppress a real fine0
hit, or permit coarse2 for a raw target below1.

Result at `524544+pixel*64+(tap*2+branch)*4`, low branch=0, high=1:

| Bits | Value |
| --- | --- |
| 0..9 | actual page-table entry; 0 absent, 1..512 slot+1 |
| 10..13 | actual resolved virtual mip |
| 14..19,20..25 | actual resolved page X,Y |
| 26 | GPU wrote final resolution, including absent-after-root |
| 27 | selected center-anchored page, otherwise tap-addressed page |
| 28..31 | reserved zero |

Requested low = `min(maxMip,floor(targetMipBeforeBias)+bias)`, high =
`min(maxMip,low+1)`. Both records are required even at the root. The compute
walker performs each mip's anchored-border attempt followed by the direct tap
lookup, matching `TerrainCompositedAddressAnchored`; it exports actual selected
entry/key. It does not reconstruct current residency on CPU. No full traversal is
claimed. Capture rejects layouts larger than 8192 on either axis, maxMip>13,
entries>512, page coordinates>63, or missing tap results.

Owner row at `1573120+(entry-1)*64`: 16 uint32 words:
`valid,surfaceIndex,mip,(x | y<<16),generationLo,generationHi,revisionLo,revisionHi,
identity[8]`. Identity is 32 raw digest bytes, not numeric text; hex conversion
preserves byte order. Owner rows are the immutable committed pool snapshot plus
that recording's pending removals/uploads overlay, supplied by Euclid. Invalid
owner is unknown; mismatched page/surface/generation/revision/full identity is
incorrect. Never replace unknown owner with expected surface metadata.

Surface row at `1605888+surfaceIndex*64`: 16 uint32 words:
`index,width,height,(maxMip | repeat<<8),generationLo,generationHi,
expectedRevisionLo,expectedRevisionHi,expectedIdentity[8]`.
Stable surfaceId strings are carried in the hash-bound metadata `surfaceIds[]`.
Pascal must additionally compare this expected identity/layout to the verified
V2 publication or independently verified active refresh revision.

## GPU/renderer integration

Existing graph `terrainFeedback.input` is exactly ordered worldTexture bindings
0 page table, 1 layer catalog, 2 layer IDs, 3 layer weights, 4 surface map.
Existing SPIR-V was disassembled to verify this interface. The normal compiled
shader may optimize unused catalog/ID/weight bindings away; physical layout is
still graph-owned 0..4.

The authorized existing shader now declares macro `VL_TERRAIN_QUALITY_CAPTURE=0`
in `pass/M_terrainFeedback.json`. With value 1, `pass/terrainFeedback.frag`
includes `terrainCompositedQualityVisible.glsl` and calls
`TerrainQualityVisibleSample(inMaterialUv,compositedFootprint,surfaceIndex,0)`.
Full-render derivative scaling remains in the existing feedback footprint helper.
Set 3 binding 5 is the capture SSBO; binding 6 is final feedback depth sampler.
After storage side effects the capture variant discards the fragment, leaving the
normal feedback color attachments unchanged. An invalid surface-map sample is
captured with invalid surface index65535 rather than silently dropped; its
visible/resolved count mismatch makes the observation incomplete.
No new resources are declared in the graph by this worker.

**Implemented replay uses the existing PipelineFactory layout override path.**
No general pipeline schema, graph input type, or descriptor-writer extension is
needed. `terrainCompositedQualityReplay.*` prepares the actual full shader-pair
variant with macro1, retains the regular pipeline's complete physical sets0/1/2,
retains all five graph inputs at set3, and adds reflected storage5/sampler6 through
`GraphicsPipelineLayoutDesc`. `GraphicsPipeline` already applies these overrides;
`PipelineLayoutBuilder` already consumes `ShaderBinding.type/descriptorCount`.

`TerrainCompositedQualityGpu::CreateVisibleReplay` is the admitted creation entry:
construct the quality owner with its actual allocation quote/metadata reservation
first, then call this method. The returned alias retains the Workspace lease for
the replay pipeline's lifetime. One replay pipeline is admitted per quality owner;
recreate/retire that owner at a safe point if the replay renderpass contract changes.
Bind rejects a pipeline not prepared by that owner. The module owns a17-set pool (16 compute +1 replay),
and `BindVisibleReplay(command,handle,pipeline,feedbackInputs[5])` explicitly writes
set3 bindings0..4 from the actual strong Texture inputs,5 from its capture buffer,
6 from the final-depth snapshot, then binds the replay pipeline and set3.
It accepts one bind per handle, retains pipeline/input owners until fence-safe,
and rejects a regular/noncapture layout. Euclid retains/binds the normal global and
material sets0/1 and reuses normal terrain object set2 during draw replay.

The earlier Material wrapper restriction is local to
`Material::Material -> BuildMaterialSurfacePipelineLayout`, which enforces graph
sampler-only set3. Replay deliberately calls PipelineFactory directly and never
routes through that wrapper or the ordinary pass descriptor writer. Regular
M_terrainFeedback macro0 has no storage5/depth6 requirement.

`M_terrainFeedback.json.macros` is a real material macro declaration, not a
parameter. `MaterialInstanceResolver::ResolveWithDefinition` merges its numeric
override, `MaterialInstanceValidator::BuildLoadPlan` produces the ShaderVariantKey,
`ShaderCompiler::CreateGraphicsVariantBuildRequest` passes it to shaderc's
`AddMacroDefinition`. This asset has no shaderEvaluation and therefore correctly
uses the full `pass/terrainFeedback.vert/frag` graphics-pair path. A separate
M_terrainFeedbackQuality is unnecessary (and its filename would infer another
shader pair unless supporting sources were added).

Remaining frame integration, owned by Euclid/main:

- `source/render/frame/renderFrameRecorder.cpp`, after the feedback pass ends and
  before queue submission: transition final128² feedback depth from attachment
  writes to sampled read, replay the identical terrain draw/camera/viewport into
  a pass that does not attach/write this sampled depth, then resolve/copy.
  Reusing the same depth as sampled input and active writable attachment is invalid.
  A depth copy is optional; if introduced, query/charge its complete allocation.
- Create the replay Vulkan renderpass/framebuffer separately using existing backend
  APIs and its actual compatible PassPipelineContractKey. The replay helper disables
  depth/stencil tests and depth writes; final-depth equality is the shader mask.
  An independently owned replay does not need to be registered as a graph
  TerrainFeedback pass and therefore does not need that graph validator changed.
- `source/render/backend/rendererDrawExecutor.cpp::DrawTerrainFeedbackScene`
  already filters eligible terrain and binds object set2 for replay. Global,
  material and augmented pass sets must match the replay pipeline layout.

Compute set0: binding0 output storage buffer, binding1 current surface's actual
page-table combined sampler2DArray, binding2 four-byte surface selector storage.
Sixteen distinct descriptor sets/selectors avoid descriptor updates overwriting
earlier recorded dispatches. Dispatch each surface over the fixed World domain.

API order:

1. `QueryAllocation(backend,limits)` quotes every actual Vulkan buffer requirement:
   output, host readback, and16 selectors. `metadataOverhead` explicitly covers
   descriptors/query/pipeline/opaque bookkeeping and CPU objects/snapshot/export
   metadata. Constructor repeats queries and reserves the total with the shared
   World BudgetOwner in Workspace before allocating. New/old/retiring owners
   remain charged. Query existence is not proof of current32MiB budget fit.
2. Construct immutable `TerrainCompositedQualitySnapshot`: frame, poseId, generation,
   reserved drawSubmissionEpoch, render extent, actual uninitializedDraws counter,
   actual camera pose, expected surface dictionary and page-table Texture strong
   references, committed+pending owner overlay, borrowed final feedback depth
   binding, and strong `drawPageTableOwner`. Same queue ordering must prevent later table mutations overtaking
   compute. Do not use a new latest Residency lookup when consuming.
3. `Record(command,snapshot,monotonicMs)` returns an optional handle; null means
   deferred (busy, interval, run cap). It clears/uploads the fixed buffer and
   records transfer→shader visibility. Begin the independent final-depth-sampled
   replay renderpass, retain/bind regular global/material sets, call
   `BindVisibleReplay(command,handle,replayPipeline,actualFeedbackTextures)`,
   and draw the original eligible terrain with its unchanged camera/object set2.
   Then end the replay renderpass and call
   `RecordResolveAndCopy(command,handle)` outside the renderpass.
4. Submit in the same draw epoch. `Submitted(handle,actualEpoch)` requires an
   exact epoch match and returns a **strong retirement lease**; put it into the
   renderer's existing GPU epoch retirement owner until its fence completes.
   This keeps buffers/queries/descriptors/snapshot alive across World removal.
   For an unsubmitted/discarded command, `Abort(handle)` releases the snapshot.
5. `TryConsume(completedEpoch,actualObservationMonotonicMs)` is nonblocking and
   returns nothing before epoch/query completion. Its immutable mapped readback
   retains storage/snapshot and prevents slot reuse until released. Readiness
   convergence uses completedMonotonicMs, never backdates to Record time.

No waitIdle, no capture ring, no pending-slot overwrite. Caller thread serializes
Record/Submitted/Abort/TryConsume. Fragment stores/atomics must be enabled on the
logical device before creating the capture graphics pipeline. Current
`vulkanManager.cpp` already requires/enables this feature for particle diagnostics.

Depth API update for Euclid: `snapshot.finalFeedbackDepth` is
`TerrainCompositedQualityDepthBinding`, **not** `shared_ptr<Texture>`.
Fill its `descriptor = vk::DescriptorImageInfo(resource.sampler,
resource.GetShaderDescriptorView(),actualSampledLayout)`,
`graphDepthOwner = sourceGraphPackage` (the strong owner that actually retains
those native GPU resources), `image = resource.image`,
`extent = {resource.width,resource.height}`, `format = resource.format`,
`usage = actualCreateInfo.usage`, and `samples = e1`.
`VisibilityDepth(handle)` returns the stored borrowed descriptor by const reference.
The module never destroys these image/view/sampler handles; the strong graph owner
stays in the immutable snapshot through submission retirement and readback.
Do not construct an owning Texture from graph-native handles.

The frozen feedback domain currently requires single-sample128² D32_SFLOAT.
Record validates that format/extent, nonnull borrowed handles/graph owner, sampled
usage, and shader-read-only/depth-stencil-read-only descriptor layout. Native graph
resource `terrainFeedbackDepth` currently declares only depthStencilAttachment at
`config/renderGraphConfig.json` resource entry (around line381); its image creation
in `source/renderGraphPassResources.cpp` uses exactly the configured usage.
Euclid must add sampled to this existing resource before creation, query/admit its
actual requirements, and transition attachment writes to sampled reads after the
feedback pass. Layout transition alone cannot add image usage.
No private depth copy or extra owned depth allocation is introduced by this module.

## Pascal decoder and evidence

`noether_quality_decode.py::Capture(payload,metadata,verified_surfaces=None)` validates
the fixed ABI/epochs. `samples()` streams all actual tap pairs; `counts()` independently
recomputes quality without materializing per-frame traversal arrays. It raises
ValueError on incomplete/malformed capture. Missing entries/unknown owners remain
in the denominator; wrong actual identity increments incorrectIdentity. Zero visible
samples cannot release loading. No independent GPU verification has run yet.

Metadata must be hash-bound beside the raw binary by the runner:
`protocol:1,source:"gpu-page-table-readback",frame,poseId,worldGeneration,
submissionEpoch,completedEpoch,surfaceIds:[...],cameraPosition,cameraLookAt,
captureIntervalMs:250,capturedMonotonicMs,completedMonotonicMs,
captureGpuAllocationBytes,captureHostAllocationBytes,workspacePeakBytes,cpuPeakBytes,
captureGpuMilliseconds,requestedCaptures,completedCaptures,deferredCaptures`.
Binary ABI does not claim to authenticate an arbitrary caller's fence number;
native fence retirement and hash-bound runner provenance establish that origin.

`WriteReadback(readback,binaryStream,metadataStream)` writes the bounded binary and
small metadata directly; no sample-array JSON is created in the engine. Export
failure throws and disables further recordings on that owner. Caller must surface
incomplete evidence and stop its export queue. Metadata overhead CPU reservation
must cover at least Impl + Snapshot +64KiB (and any larger actual integration
costs); the constructor checks this floor. `workspacePeakBytes` is explicitly the
maximum shared-owner Workspace charge observed at allocation/Record/TryConsume,
with `workspacePeakStatistic` recording that sampling scope. It is not claimed
to be continuous per-class telemetry; CPU peak comes from the owner's actual peak.

`document_fields()` emits domain/counter metadata; optional offline
`--expanded-output` streams diagnostic JSON with `binaryAbi:"TVQ1-final-results-v1"`,
without fabricated traversals. Pascal owns adapter support for that mode.
Existing `rvt_sample_evidence.py` expanded-traversal mode is not changed here.
Service uses the native counts only after actual roots+TES closure readiness;
the module's threshold predicate alone does not acknowledge initial loading.

No-visible-terrain liveness boundary: `Counts::MeetsQualityThreshold` intentionally
requires nonempty samples; an empty/cleared capture is never an automatic pass.
For sky/sea/off-island views the Service may release loading only via a distinct
explicit `no visible terrain` readiness event backed by independent evidence that
the actual capture draw/visibility domain contains zero terrain, with valid
production inputs, roots and TES height closure. A CPU visible-terrain candidate
with zero captured samples remains incomplete. This additional empty-domain proof
and Service branch are not yet implemented by this module and must be included in
arbitrary unvisited-position/teleport validation; they cannot be hidden as a passed
zero-denominator99% observation. Full16's nonempty initial far view uses the
unchanged nonempty99%/identity/root-only gate.

Run CPU-only verification:
`py -3 tool/validation/terrain/noether_quality_cpu_check.py`.
It compiles the module, runs focused GTests and Python negatives, links both
vertex/fragment variants, validates SPIR-V, and checks actual descriptor decorations.
All outputs stay under `artifacts/terrain-rvt-production/p3-noether-cpu`; it never
launches a Vulkan application or writes shared `shader/spv`/build outputs.

`py -3 tool/validation/terrain/noether_quality_variant_check.py` additionally
fresh-compiles the production Material resolver/validator, include generator,
ShaderCompiler and reflection implementation, then runs GoogleTests through
their real CPU candidate-build entry points for both macro values. It supplies
the real generated include in the candidate's in-memory overlay and never
commits generated includes/SPIR-V/cache outputs. The same compiled reflection
tests the explicit replay layout and regular-set compatibility. Results and
source/object/support hashes stay in `artifacts/terrain-rvt-production/p3-noether-variant`.
This verifies actual material compilation/CPU reflection; creation/use of the
Vulkan graphics pipeline still awaits the assigned GPU window.

# Formal performance runner conditions and remaining evidence

`run_rvt_case.py --profile formal-performance --quality-mode capture-4hz` locks
worker2,1280×720,MSAA2,UI disabled in installed config,immediate present,30Hz and
`--no-validation`. Correctness uses separate `--profile correctness` with validation.
No package config mutation occurs. Mismatching config is rejected before launch.

Child environment is explicit: inherited `VL_*` and `VULKANLEARN_*` variables
are cleared, then `VL_GPU_TIMING_RAW=1` and
`VULKANLEARN_RVT_QUALITY_DIR=<external fresh evidence>/quality` are set.
Both CPU_RAW and GPU_RAW use the former flag in current native code. This is
not a new timestamp implementation. No quality output is directed into readonly
install; ACL backup,probe,logs and screenshots also live outside install.
Record all environment override values and cleared variable names,not unrelated
environment secrets. Package binary,shader cache,shader manifest,frozen config,
capture plan,scenario and decoder files are hash bound before/after execution.

Named compiler inventory starts before process launch at500ms and spans teardown.
Any busy/error/missing audit rejects formal scope. Like P0,this sampling cannot
prove every unrelated background workload absent or detect all sub500ms work.
Record hardware/driver conditions separately; do not infer them from P0.

## Quality and timing scope

Initial implementation keeps native bounded quality capture enabled at<=4Hz,
with actual allocation/IO/timestamp overhead included in reported raw frame data.
Do not subtract assumed overhead. Compare against P0's instrumented timing scope
and explicitly label the additional quality replay/readback/IO work.
Capture256/run and byte caps still apply. Deferred/exhausted capture or absent
quality proof is incomplete,not ready. Capture status and phase coverage must
be verified from actual native metadata. 4Hz policy is not proof it was recorded.

`paired-steady` is a reserved **rejected** mode until runtime implements a real,
observable capture-phase control. It cannot silently unset quality env. Future
paired evidence must first prove each pose's99%/identity/TES quality,then freeze
and identify that same camera/input/residency state for steady timestamps;
capture-on comparison and actual capture-on→steady transitions are required.
Every different pose,streaming miss,generation/input change invalidates that proof
until reobserved. Quality-off whole-route “pass” is not accepted.

## Required actual phases

Native `measurement-phase` events need
`phase=loading|streaming|warm-ready`, `frame`, `poseId`, `worldGeneration`,
`monotonicMs`, `qualityCaptureMode`, `qualityProofArtifact`, `reason`.
Only observed native state transitions are evidence; scheduled command frames
are not phase markers. Raw GPU frame IDs and capture/draw frames must have an
explicit mapping,CPU_RAW currently lacks frame IDs so do not pair by guessed index.
That runtime mapping remains necessary for phase-specific formal statistics.

Analyzer retains **whole raw streams** and reports all slow/loading/capture frames.
No fastest contiguous subset,no screenshot-near window,no filtering null queries.
Require all GPU frame IDs unique/contiguous and every pass query complete; report
any startup/teardown query omission explicitly before formal comparison.
At least1000 raw frames are required **within each prespecified formal phase/window**
used for a percentile,not1000 arbitrary samples pooled across loading/streaming.
Entire predetermined quiet-final nine-section warm route is the reference condition;
reload route/overlap peaks are separate. Current analyzer's whole-run1000 check is
only completeness preflight; it deliberately never outputs formalPerformancePassed.

P0 CPU reference1081/GPU1080 rows and timing scope remain historical finite
observations. Do not fake equal counts or drop one CPU row to align arrays.
Geometry p99 includes TES and other geometry; no independent TES GPU timing.

## Production, caches and offline execution

Separate finite production suite requires **10936 unique samples**:
5468/mode,real tile00,8192,mip0..13,heightBlend3. No dropped/duplicated page keys,
missing/null query or faster-window substitution. Same-page production+copy is
computed per row,not sum of independent maxima. Actual V2 GPU→atlas copy is
identified separately from P0 synchronous output→host reference.

CPU/WDDM peaks use full process including loading/reload/candidate overlap;
record sample gaps/unavailable tail and original limits. Raw pass sum covers
instrumented passes only,not uninstrumented environment or pure terrain work.
Input/physical/workspace/CPU cap admission remains the runtime owner responsibility.

OS file/standby cache is uncontrolled,not purged. Shader cache must be warm and
any actual compile/cache miss invalidates formal run. Fine disk cache disabled
is an observed feature scope,not a demonstrated warm-cache gain; mutation cases
remain explicitly not-applicable with independent scope review.
Fresh process/physical residency and unvisited GPU production remain mandatory.

Readonly NTFS run is separately recorded on a newly exported owned package with
real write probes,Game success,zero shader misses and finally ACL restoration.
Project-owned file-open tracing+closure/static root validation is scoped honestly;
no OS-wide process file monitoring is claimed. Screenshot user-output is external.

## Current status

Runner can launch under these conditions,collect raw samples,quality artifacts and
host audit,but **formal phase mapping,full frozen metric evaluation and paired
quality-overhead comparison still require native/runtime integration and actual runs**.
Missing measurements result incomplete; code existence is not performance approval.
GPU execution remains controlled by main/Euclid allocation. No run was started here.

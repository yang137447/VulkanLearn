"""Sign measured P0 references and immutable numeric RVT production targets."""

import argparse
import hashlib
import json
import math
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def rounded_up(value):
    return math.ceil(value * 1000) / 1000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    root = options.evidence.resolve()
    quiet = read(root / "quiet-final/raw-analysis.json")
    audit = read(root / "quiet-final.host-audit-summary.json")
    if not audit["quietAtObservedSamples"] or audit["driverExitCode"]:
        raise ValueError("Formal quiet reference is not valid")
    if quiet["gpuRawFrames"] < 1000 or quiet["cpuRawFrames"] < 1000:
        raise ValueError("Missing raw frame reference")
    if any(quiet["rawParseErrors"].values()):
        raise ValueError("Raw timing parse errors")
    producers = [read(root / name / "report.json") for name in
                 ("producer-coverage-full", "producer-local-full")]
    for mode, report in enumerate(producers):
        samples = report["samples"]
        if not report["passed"] or len(samples) != 5468 or {sample["mip"] for sample in samples} != set(range(14)):
            raise ValueError("Incomplete finite pressure collection")
        if any(sample["weightSource"] != mode or sample["heightBlend"] != 3 for sample in samples):
            raise ValueError("Wrong production workload")
    references = {name: read(root / name / "report.json") for name in
                  ("cold", "raw-cold", "raw-warm-r2", "quiet-final")}
    if any(report["status"] != "passed" for report in references.values()):
        raise ValueError("Baseline run failed")
    allocation = read(root / "allocation-requirements-final.json")
    images = {item["name"]: item for item in allocation["images"]}
    aggregate = lambda items: sum(item["allocationBytes"] * item["multiplicity"] for item in items)
    pool = aggregate(item for name, item in images.items() if name.startswith("shared-material-pool"))
    input_bytes = aggregate(item for name, item in images.items() if not any(
        text in name for text in ("shared-material-pool", "old-private", "surface-page-table")))
    if pool > 224 * 1024**2 or input_bytes > 768 * 1024**2:
        raise ValueError("Measured Full16 resource envelope exceeds the proposed P0 caps")
    ledger = read(root / "ledger-final.json")
    samples = [sample for report in producers for sample in report["samples"]]
    gpu_p99 = quiet["gpuInstrumentedPassSumMs"]["p99"]
    cpu_p99 = quiet["cpuRenderFrameIntervalMs"]["p99"]
    world_reference = min(report["worldCommitElapsedSeconds"][0] for report in references.values()) - 0.250
    private_reference = min(references[name]["memory"]["peakPrivateBytes"]
                            for name in ("cold", "raw-cold", "raw-warm-r2"))
    wddm_reference = min(references[name]["memory"]["peakSimultaneousDedicatedPlusSharedBytes"]
                         for name in ("cold", "raw-cold", "raw-warm-r2"))
    mib = 1024**2
    result = {
        "schemaVersion": 1, "status": "frozen-targets-independent-review-required",
        "dateAsiaShanghai": "2026-11-21",
        "frozen": True, "independentReview": "pending",
        "validRunIds": {"formalCpuAndGpu": ["quiet-final"],
            "fullRouteMemoryAndReload": ["cold", "raw-cold", "raw-warm-r2"],
            "finiteProduction": ["producer-coverage-full", "producer-local-full"]},
        "excludedRunIds": {"quiet": "Observed compiler processes; not a formal timing denominator",
            "raw-warm": "Rejected zero console-command delay; no renderer measurement",
            "producer-coverage": "Rejected redundant default material parameter",
            "producer-local": "Old R32_FLOAT upload unsupported; negative evidence only",
            "raw-warm-r2": "Valid functional/memory/reload evidence, but not the formal CPU timing denominator"},
        "noRelaxation": "P1-P5 and optional P6 must meet these targets; failure is reported, never fixed by raising budgets, lowering quality or changing benchmark conditions.",
        "conditions": {"gpu": "NVIDIA GeForce RTX 3080", "driver": "621.01",
            "cpu": "AMD Ryzen 9 3950X", "resolution": [1280, 720], "msaa": 2,
            "build": "Release MinGW C++17", "validation": False, "presentMode": "immediate",
            "displayScope": "1280x720 SDL/Vulkan render area; physical display refresh and continuous foreground state were not measured",
            "frameLimitHz": 30, "uiEnabled": False, "referenceWorkerThreads": 2,
            "requiredCorrectnessWorkerModes": [1, 2],
            "route": "quiet-final/command.json: far, mid, near, tile00_near, cross_tile_corner, interpolated movement, teleport, near, final_far",
            "formalCpuReference": "quiet-final; 370 observed compiler-inventory samples, zero busy/error samples; absence of every unrelated workload is not proven"},
        "cacheConditions": {
            "baselineApplication": "fresh process, fresh physical residency; V1 immutable full page packages remain present",
            "baselineShader": "warm SPV/build cache retained; no shader compile in runs",
            "baselineOs": "OS file/standby cache not cleared or claimed cold",
            "requiredV2Cold": "fresh process; delete all optional fine-page cache payloads and cache index under the configured private runtime cache root; preserve immutable V2 production inputs, roots/coarse pages and SPV",
            "requiredV2Warm": "same private package, same inputs/camera/render settings, revisit trace; retain optional bounded cache",
            "noFallback": "V1 files, authored direct evaluator and retired material switches must be rejected"},
        "measured": {
            "quietRawGpuFrames": quiet["gpuRawFrames"], "quietRawCpuFrames": quiet["cpuRawFrames"],
            "quietGpuInstrumentedPassSumMs": quiet["gpuInstrumentedPassSumMs"],
            "quietGeometryIncludingTesMs": quiet["gpuPassesMs"]["geometry"],
            "quietUnclampedRenderFrameIntervalMs": quiet["cpuRenderFrameIntervalMs"],
            "worldCommitReferenceLowerBoundSeconds": world_reference,
            "fullRoutePrivatePeakReferenceBytes": private_reference,
            "fullRouteWddmPeakReferenceBytes": wddm_reference,
            "finiteProductionPages": len(samples), "finiteMipRange": [0, 13],
            "finiteWeightModes": ["CoverageAtlas", "LocalLayerArray"],
            "finiteHeightBlend": 3,
            "singlePageProductionMaximumObservedMs": max(sample["productionMs"] for sample in samples),
            "singlePageReadbackCopyMaximumObservedMs": max(sample["readbackCopyMs"] for sample in samples),
            "singlePageProductionPlusReadbackMaximumObservedMs": max(sample["productionMs"] + sample["readbackCopyMs"] for sample in samples),
            "sharedPoolMemoryRequirementsBytes": pool, "sharedInputMemoryRequirementsEnvelopeBytes": input_bytes,
            "allResidentGeometryPayloadEnvelopeBytes": ledger["geometryAllResidentPayloadBytes"],
            "legacyKnownImagePayloadBytes": ledger["baselineKnownImagePayloadBytes"],
            "legacyFullMaterialDataBytes": ledger["materialPayloadDataBytes"],
            "legacyFullMaterialHeadersBytes": ledger["materialPayloadHeaderBytes"]},
        "steadyRvtCaps": {"physicalPoolGpuBytes": 224*mib, "inputGpuBytes": 768*mib,
            "workspaceGpuBytes": 32*mib, "totalGpuBytes": 1024*mib,
            "totalCpuBytes": 512*mib, "physicalSlots": 512, "productionTasks": 4},
        "peakRvtOwnerCaps": {"physicalPoolGpuBytes": 448*mib, "inputGpuBytes": 1536*mib,
            "workspaceGpuBytes": 64*mib, "totalGpuBytes": 2048*mib,
            "totalCpuBytes": 1024*mib, "physicalSlots": 1024, "productionTasks": 4},
        "terrainTotalCaps": {"steadyGpuBytes": 2048*mib, "candidateActiveRetiringGpuBytes": 3072*mib,
            "geometryGpuBytesPerWorld": 1024*mib, "steadyCpuBytes": 2048*mib,
            "candidateActiveRetiringCpuBytes": 3072*mib, "geometryCpuBytesPerWorld": 1024*mib},
        "workspaceReserves": {"descriptorDriverMetadataGpuBytesPerWorld": 4*mib,
            "taskParameterGpuBytesPerWorld": 256*1024, "uploadRingBytesPerSlot": 4*mib,
            "uploadRingSlots": 2, "productionOutputBytesPerTask": 221956,
            "productionTasksGlobal": 4, "queueEntriesGlobal": 4096,
            "queueMetadataCpuBytes": 16*mib, "indexMetadataCpuBytes": 32*mib,
            "otherHostMetadataCpuBytes": 16*mib, "uploadAndReadbackCpuBytes": 64*mib,
            "optionalCpuPageCacheBytes": 128*mib, "decodedInputCpuWorkspaceBytes": 256*mib,
            "cpuClassReserveSumBytes": 512*mib},
        "accounting": {
            "physicalPool": "Three actual image memoryRequirements capacities; roots/reserved/retiring slots included.",
            "input": "Source arrays, global maps, shared coverage once, frame inputs, height atlas/LUTs, source maps/catalog and both local-weight representations; exact immutable resource identity deduplicates, never World count.",
            "workspace": "Every per-surface page table, GPU feedback targets/readback, scratch/status/parameters, shared upload ring, descriptors and explicit GPU metadata reserve.",
            "cpu": "One aggregate cap plus bounded queues, metadata, cache, input decode and host-visible ring reservations. Reserve components sum to steady total; payload allocations cannot bypass it.",
            "gpuAdmission": "Actual Vulkan image/buffer memoryRequirements.size before allocation, including allocator alignment/padding; descriptor/opaque driver GPU bookkeeping uses explicit reserve. WDDM is a separate observed process criterion.",
            "outerTerrainOwner": "RVT costs plus geometry and terrain host metadata; enforce both RVT caps and outer terrain caps over Candidate+Active+Retiring including old frames and task leases.",
            "overlap": "peakRvtOwnerCaps maps to TerrainRvtBudgetCaps for renderer-wide owner; steadyRvtCaps also enforced for one active World. Outer 3GiB terrain peak may reject candidates even if each subcap separately fits. Reject and retain old World, or verified staged reuse; never silently exceed.",
            "hostAndDriverLimits": "WDDM is a sampled allocation/residency approximation, not exact Vulkan high-water; unobservable opaque host bookkeeping remains subject to whole-process peak thresholds."},
        "targets": {
            "singlePageProductionHardMs": 4.0, "singlePageReadbackCopyHardMs": 4.0,
            "singlePageProductionAndCopyHardMs": 6.0, "perFrameProductionAndCopyHardMs": 6.0,
            "productionPagesPerFrame": 1, "productionBytesPerFrame": 221956,
            "uploadPagesPerFrame": 16, "uploadBytesPerFrame": 4*mib,
            "maxGlobalInFlightProductionTasks": 4,
            "maxSteadyGpuPassSumP99Ms": rounded_up(gpu_p99*1.10),
            "maxStreamingGpuPassSumP99Ms": rounded_up(gpu_p99 + 6.0),
            "maxSteadyGeometryIncludingTesP99Ms": rounded_up(quiet["gpuPassesMs"]["geometry"]["p99"]*1.10),
            "maxRawRenderFrameIntervalP95Ms": rounded_up(quiet["cpuRenderFrameIntervalMs"]["p95"]*1.10),
            "maxRawRenderFrameIntervalP99Ms": rounded_up(cpu_p99*1.10),
            "minimumRawFrameSamples": 1000, "maxWorldCommitSeconds": math.floor(world_reference*0.5*1000)/1000,
            "maxProcessToInteractiveSeconds": math.floor(world_reference*0.5*1000)/1000+10,
            "maxPoseOrTeleportConvergenceSeconds": 10.0,
            "maxWholeProcessPrivatePeakBytes": math.floor(private_reference*0.8),
            "maxWholeProcessWddmPeakBytes": math.floor(wddm_reference*0.8),
            "minPrivatePeakImprovementFraction": 0.20, "minWddmPeakImprovementFraction": 0.20,
            "minStartupImprovementFraction": 0.50, "maxWddmSampleGapSeconds": 2.5,
            "minimumValidWddmSamples": 20, "maxInteriorUnavailableWddmSamples": 0,
            "maxTeardownUnavailableWddmSamples": 1, "maxOptionalFineDiskCacheBytes": 256*mib,
            "maxOfflineGamePackageBytes": 1536*mib, "maxPublishedCoarsePagesFull16": 448,
            "finestPublishedCoarseMip": 4, "requiredPinnedRoots": 16},
        "qualityTargets": {
            "maxEffectiveMipBias": 1, "minReadyVisibleSampleFractionAtTargetPlusBias": 0.99,
            "maxUnresolvedVisibleSamplesAtReady": 0, "maxIncorrectIdentitySamples": 0,
            "rootOnlyIsReady": False, "rootOnlyAllowedAfterInteractive": False,
            "basePageMaxAlbedoChannelByteError": 1, "basePageMaxNormalChannelByteError": 1,
            "basePageMaxOrmChannelByteError": 1, "litTerrainRoiMaxMaeBytes": 2.0,
            "litTerrainRoiMaxRmseBytes": 3.0, "litTerrainRoiMaxP99ErrorBytes": 8.0,
            "maxSilhouetteXorFraction": 0.001, "tesMaxHeightErrorR16Units": 2.0,
            "maxValidationErrors": 0, "maxStalePublications": 0,
            "loadingRelease": "All 16 roots and independent TES height closure valid, zero unknown/stale identities, >=99% visible valid feedback samples resolve no coarser than target+1, no root-only result when target<12; then expose interactive game. Validate actual sampled-page identities and images, not queue-empty or cumulative estimated histograms.",
            "convergence": "Same readiness within 10s at each fixed pose/teleport; bias never exceeds 1. If visible working set cannot meet it with 512 slots, report budget insufficient.",
            "visualComparison": "Lock inputs/camera/render parameters and terrain ROI. Compare baked base channels to independent current evaluator, then lit images plus active TES/dynamic items separately. Diagnose authored discontinuity without excusing addressing seams."},
        "limitations": [
            "P0 freezes targets for future stages; it does not claim V2 runtime, package, convergence or quality targets have already passed.",
            "Finite production collection is real tile00 data, all 5468 pages/mode at 8192, not a proof over every input, driver or GPU preemption.",
            "Local production uses the approved isolated R32_FLOAT->eR32Sfloat upload-format correction; original unsupported-format failure remains recorded.",
            "Synchronous copy queries measure output-to-host readback. V1 Game service queries cover real atlas uploads as a batch; future V2 GPU copy must be timestamped and checked against these hard targets.",
            "GPU pass sum includes full geometry with TES and other objects, shadow and feedback, but excludes uninstrumented environment work; neither pure terrain time nor separate TES time is claimed.",
            "CPU reference is unclamped rendered-frame wall interval including pacing/capture, not CPU work. World reload is validated in full runs and measured separately from quiet steady reference.",
            "250ms milestone observer gives upper-bound observation times; first screenshot includes scheduled waits and is not earliest clear readiness. V2 must emit its own monotonic readiness event.",
            "WDDM helper exit=1/terminal unavailable rows are explicitly retained; requested 500ms becomes approximately 1.54s actual cadence. No exact GPU transient high-water claim.",
            "quiet observed compiler activity; raw-warm-r2 lacked a compiler-free audit. quiet-final is the sole formal CPU reference. Shorter-than-audit compiler processes and unrelated background activity cannot be ruled out."] }
    evidence_paths = [
        "quiet-final/report.json", "quiet-final/raw-analysis.json", "quiet-final/command.json",
        "quiet-final/runtime.log", "quiet-final.host-audit-summary.json", "quiet-final.host-audit.jsonl",
        "quiet-final/gpu-raw.jsonl", "quiet-final/cpu-raw.jsonl",
        "quiet-final/runtime.cpu-memory.jsonl", "quiet-final/runtime.gpu-memory.jsonl",
        "cold/report.json", "raw-cold/report.json", "raw-warm-r2/report.json",
        "producer-coverage-full/report.json", "producer-local-full/report.json",
        "producer-coverage-full/bake-raw.jsonl", "producer-local-full/bake-raw.jsonl",
        "allocation-requirements-final.json", "ledger-final.json",
        "numeric-oracle/report.json", "raw-cpu-stage/p0-input-hashes.json",
        "raw-cpu-link/build.json", "local-producer-link-r2/build.json", "local-producer-link-r2/mapping.patch"]
    evidence_paths += ["hardware-cpu.json", "hardware-os.json", "hardware-gpu.csv"]
    result["evidenceSha256"] = {path: hashlib.sha256((root/path).read_bytes()).hexdigest()
                                for path in evidence_paths}
    result["evidenceRoot"] = str(root)
    result["evidenceFiles"] = {path: {"path": str(root/path), "bytes": (root/path).stat().st_size,
        "sha256": result["evidenceSha256"][path]} for path in evidence_paths}
    result["acceptanceMeasurements"] = {
        "steadyGpu": {"maximum": result["targets"]["maxSteadyGpuPassSumP99Ms"], "unit": "ms",
            "statistic": "p99 of raw per-rendered-frame instrumented GPU pass sum",
            "minSamples": 1000, "sampling": "every completed timestamp-query frame, no rolling mean",
            "cases": ["fixed nine-section warmed camera trace, worker2, 30Hz"],
            "comparison": "at most 10% above quiet-final raw p99"},
        "steadyCpu": {"maximum": result["targets"]["maxRawRenderFrameIntervalP99Ms"], "unit": "ms",
            "statistic": "p99 of unclamped rendered-frame wall intervals including pacing/capture",
            "minSamples": 1000, "sampling": "steady_clock every rendered frame; no exposure clamp",
            "cases": ["same fixed nine-section warmed camera trace, without World reload"],
            "comparison": "at most 10% above quiet-final raw p99"},
        "singlePageProduction": {"maximum": 4.0, "unit": "ms", "statistic": "observed maximum",
            "minSamples": 10936, "sampling": "GPU timestamps around every page production dispatch/initialization",
            "cases": ["CoverageAtlas and LocalLayerArray", "real tile00, 8192, mip0..13, heightBlend=3"],
            "comparison": "hard target, finite pressure scope; not a hardware WCET guarantee"},
        "singlePageCopy": {"maximum": 4.0, "unit": "ms", "statistic": "observed maximum",
            "minSamples": 10936, "sampling": "GPU timestamp pair per page",
            "cases": ["synchronous output-to-host copy finite reference; V2 buffer-to-atlas copy must be measured separately"],
            "comparison": "independent copy hard target; production+copy and per-frame sum must also stay <=6ms"},
        "wddmPeak": {"maximum": result["targets"]["maxWholeProcessWddmPeakBytes"], "unit": "bytes",
            "statistic": "maximum simultaneous PID dedicated+shared sample",
            "minSamples": 20, "sampling": "requested 500ms, actual cadence recorded, gap <=2.5s; only one terminal unavailable allowed",
            "cases": ["same Full16 full route including candidate/old World reload overlap"],
            "comparison": "at least 20% below minimum of this session's three matching full-route peaks"},
        "privatePeak": {"maximum": result["targets"]["maxWholeProcessPrivatePeakBytes"], "unit": "bytes",
            "statistic": "maximum sampled PID private bytes; OS peakPagefile/working-set retained separately",
            "minSamples": 20, "sampling": "GetProcessMemoryInfo, requested 500ms",
            "cases": ["same Full16 full route including reload overlap"],
            "comparison": "at least 20% below minimum of this session's three full-route private peaks"},
        "startup": {"maximum": result["targets"]["maxWorldCommitSeconds"], "unit": "seconds",
            "statistic": "process-launch to first World commit; compare monotonic event if available",
            "minSamples": 2, "sampling": "one fresh process per cold/warm case; observer uncertainty explicit",
            "cases": ["V2 fine-cache empty/SPV warm/OS uncontrolled", "same inputs with bounded warm fine cache"],
            "comparison": "at least 50% faster than current-session minimum observed startup minus 250ms observer uncertainty"}}
    output = options.output.resolve()
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    checksum = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(checksum + "  " + output.name + "\n", encoding="utf-8")
    print(json.dumps({"path": str(output), "sha256": checksum, "targets": result["targets"]}, indent=2))


if __name__ == "__main__":
    main()

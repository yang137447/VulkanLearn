"""Adapt the real frozen P0 and native V2 validator; never runs a renderer."""

import argparse
import hashlib
import json
import math
import os
import re
from pathlib import Path
import subprocess

from build_composited_fixture import published_world_files, read_json, reference_path
from rvt_delivery import IncompleteEvidence, canonical_hash, require, artifact
from rvt_p0_analyze import distribution
from rvt_p0_analyze import extract

APPROVED_P0_SHA256 = "43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd"


class FaultEvidence:
    """Read the actual RuntimeEvidence envelope; stdout never replaces this sink."""

    def __init__(self, path):
        self.path = Path(path)
        self.records = []
        self.offset = 0
        self.pending = b""
        self.bytes = 0
        self.footer = None

    def poll(self, final=False):
        from rvt_full_tour import strict_json_bytes
        if self.path.exists():
            require(not self.path.is_symlink(), "Fault sink must be a regular private output")
            size = self.path.stat().st_size
            require(self.offset <= size <= 128 * 1024 * 1024, "Fault sink truncated/over capacity")
            with self.path.open("rb") as stream:
                stream.seek(self.offset)
                while block := stream.read(1024 * 1024):
                    self.offset += len(block)
                    lines = (self.pending + block).split(b"\n")
                    self.pending = lines.pop()
                    require(len(self.pending) <= 65536, "Fault sink line exceeds native bound")
                    for line in lines:
                        encoded = line + b"\n"
                        require(len(encoded) <= 65536 and len(self.records) < 1000000,
                                "Fault sink exceeds native line/record bound")
                        require(self.footer is None, "Record after runtimeEvidenceEnd")
                        row = strict_json_bytes(encoded)
                        require(type(row) is dict and type(row.get("sequence")) is int and
                                row["sequence"] == len(self.records) + 1, "Fault sink sequence differs")
                        require(type(row.get("steady_ns")) is int and row["steady_ns"] > 0 and
                                type(row.get("threadTag")) is int and row["threadTag"] > 0 and
                                row.get("outputCategory") == "explicitoutput" and
                                type(row.get("eventClass")) is str and row["eventClass"] and
                                type(row.get("payload")) is dict, "Fault sink envelope invalid")
                        require(not self.records or row["steady_ns"] >= self.records[-1]["steady_ns"],
                                "Fault sink clock regressed")
                        if row["eventClass"] == "runtimeEvidenceEnd":
                            payload = row["payload"]
                            require(payload.get("incomplete") is False and
                                    type(payload.get("recordsBeforeTerminal")) is int and
                                    payload["recordsBeforeTerminal"] == len(self.records) and
                                    type(payload.get("bytesBeforeTerminal")) is int and
                                    payload["bytesBeforeTerminal"] == self.bytes,
                                    "Fault sink terminal incomplete/count/byte mismatch")
                            self.footer = row
                        self.records.append(row)
                        self.bytes += len(encoded)
        if final:
            require(not self.pending and self.footer is not None,
                    "Fault sink missing complete runtimeEvidenceEnd")
            require(self.path.stat().st_size == self.bytes, "Fault sink terminal size changed")
        return self.records

    def find(self, category=None, event=None, fault_id=None):
        return [row for row in self.records if
                (category is None or row["eventClass"] == category) and
                (event is None or row["payload"].get("event") == event) and
                (fault_id is None or row["payload"].get("faultId") == fault_id)]


def fault_frame_witness(records, world, after_epoch=0, after_sequence=0, exact_epoch=None):
    cpu = {}
    gpu_seen = set()
    for row in records:
        if row["eventClass"] == "GPU_RAW":
            raw_key = tuple(row["payload"].get(field) for field in ("renderFrame", "worldGeneration", "submitEpoch"))
            require(row["payload"].get("identityVersion") == 2 and
                    all(type(value) is int and value >= 0 for value in raw_key),
                    "GPU raw lacks actual R9 recording identity")
            require(raw_key not in gpu_seen, "Duplicate GPU frame identity")
            gpu_seen.add(raw_key)
    for row in records:
        if row["eventClass"] != "CPU_RAW":
            continue
        payload = row["payload"]
        if payload.get("event") != "frame-timing" or payload.get("acquiredReady") is not True:
            continue
        key = tuple(payload.get(field) for field in ("renderFrame", "worldGeneration", "submitEpoch"))
        if payload.get("identityVersion") == 2 and all(type(value) is int for value in key):
            require(key not in cpu, "Duplicate CPU frame identity")
            cpu[key] = row
    for row in records:
        if row["eventClass"] != "GPU_RAW" or row["sequence"] <= after_sequence:
            continue
        payload = row["payload"]
        key = tuple(payload.get(field) for field in ("renderFrame", "worldGeneration", "submitEpoch"))
        if (payload.get("identityVersion") == 2 and all(type(value) is int for value in key) and
                key in cpu and cpu[key]["sequence"] > after_sequence and
                key[1] == world and key[2] > after_epoch and
                (exact_epoch is None or key[2] == exact_epoch) and
                type(payload.get("fenceCompletedEpoch")) is int and
                payload["fenceCompletedEpoch"] >= key[2]):
            return {"cpu": cpu[key], "gpu": row}
    return None


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]


def audit_p0(path):
    path = path.resolve(strict=True)
    document = read_json(path)
    require(document["schemaVersion"] == 1 and document["frozen"] is True, "Unsupported/unfrozen P0")
    expected = Path(str(path) + ".sha256").read_text(encoding="utf-8").split()[0]
    require(expected == APPROVED_P0_SHA256 and sha256(path) == APPROVED_P0_SHA256,
            "Frozen P0 SHA256 differs from independently approved original")
    review_path = Path(str(path) + ".review.json")
    review = read_json(review_path) if review_path.is_file() else None
    accepted = document["independentReview"] == "accepted"
    if review:
        require(review["frozenSha256"] == expected and review["status"] == "passed"
                and review["authority"] == "independent-subagent-audit"
                and review["reviewer"]["name"] == "Boole",
                "P0 review does not bind the frozen configuration")
        audit_entry = review["audit"]
        audit_path = reference_path(path.parent.parent, audit_entry["path"])
        require(sha256(audit_path) == audit_entry["sha256"] and
                audit_path.stat().st_size == audit_entry["bytes"],
                "Independent P0 audit bytes changed")
        audit_text = audit_path.read_text(encoding="utf-8-sig")
        require(APPROVED_P0_SHA256 in audit_text and "**最终判定：PASS。**" in audit_text,
                "Independent P0 audit does not accept the approved frozen configuration")
        accepted = True
    root = Path(document["evidenceRoot"]).resolve(strict=True)
    checked = []
    require(set(document["evidenceSha256"]) == set(document["evidenceFiles"]), "P0 evidence inventory differs")
    for name, digest in document["evidenceSha256"].items():
        source = reference_path(root, name)
        entry = document["evidenceFiles"][name]
        require(Path(entry["path"]).resolve() == source, f"P0 evidence path differs: {name}")
        require(entry["sha256"] == digest and source.stat().st_size == entry["bytes"]
                and sha256(source) == digest, f"P0 evidence changed: {name}")
        checked.append(name)
    require(document["validRunIds"]["formalCpuAndGpu"] == ["quiet-final"], "P0 formal baseline changed")
    host = read_json(root / "quiet-final.host-audit-summary.json")
    require(host["quietAtObservedSamples"] is True and not host["busySamples"] and not host["errors"],
            "P0 baseline compiler interference")
    gpu = rows(root / "quiet-final/gpu-raw.jsonl")
    cpu = rows(root / "quiet-final/cpu-raw.jsonl")
    require(len({row["frame"] for row in gpu}) == len(gpu), "Duplicate baseline GPU frames")
    values = {
        "quietGpuInstrumentedPassSumMs": [sum(item["ms"] for item in row["passes"]) for row in gpu],
        "quietGeometryIncludingTesMs": [
            next(item["ms"] for item in row["passes"] if item["name"] == "geometry") for row in gpu],
        "quietUnclampedRenderFrameIntervalMs": [row["renderFrameIntervalMs"] for row in cpu],
    }
    for name, samples in values.items():
        require(all(type(value) in (int, float) and math.isfinite(value) and value >= 0
                    for value in samples), f"Invalid P0 raw timing: {name}")
        actual = distribution(samples)
        for field, value in actual.items():
            require(math.isclose(value, document["measured"][name][field], rel_tol=1e-10, abs_tol=1e-10),
                    f"P0 raw distribution differs: {name}.{field}")
    require(len(gpu) == document["measured"]["quietRawGpuFrames"] and
            len(cpu) == document["measured"]["quietRawCpuFrames"], "P0 raw sample counts differ")
    require(sum(document["workspaceReserves"][key] for key in (
        "queueMetadataCpuBytes", "indexMetadataCpuBytes", "otherHostMetadataCpuBytes",
        "uploadAndReadbackCpuBytes", "optionalCpuPageCacheBytes", "decodedInputCpuWorkspaceBytes"))
        == document["steadyRvtCaps"]["totalCpuBytes"], "P0 CPU reserve sum differs")
    return {"status": "audited" if accepted else "audited-review-pending",
            "independentReview": "accepted" if accepted else "pending", "frozen_sha256": sha256(path),
            "review_receipt": artifact(review_path) if review else None,
            "independent_audit": artifact(audit_path) if review else None,
            "evidence_files_checked": len(checked), "raw_distributions_recomputed": list(values),
            "renderer_started": False, "acceptance_passed": False, "document": document}


def normalize_p0(path):
    audit = audit_p0(path)
    require(audit["independentReview"] == "accepted",
            "P0 independentReview is pending; independent reviewer must resolve it before delivery")
    document = audit["document"]
    root = Path(document["evidenceRoot"])
    targets = document["targets"]
    rules = {}
    for name, maximum, statistic, sampling, unit, cases in (
            ("instrumented_pass_sum_gpu_ms", targets["maxSteadyGpuPassSumP99Ms"], "p99",
             "gpu-timestamp", "ms", ["hot"]),
            ("streaming_pass_sum_gpu_ms", targets["maxStreamingGpuPassSumP99Ms"], "p99",
             "gpu-timestamp", "ms", ["cold", "tour", "teleport"]),
            ("geometry_including_tes", targets["maxSteadyGeometryIncludingTesP99Ms"], "p99",
             "gpu-timestamp", "ms", ["hot", "tes"]),
            ("render_interval_p95_ms", targets["maxRawRenderFrameIntervalP95Ms"], "p95",
             "per-frame", "ms", ["hot", "tour"]),
            ("render_interval_p99_ms", targets["maxRawRenderFrameIntervalP99Ms"], "p99",
             "per-frame", "ms", ["hot", "tour"])):
        rules[name] = {"maximum": maximum, "min_samples": targets["minimumRawFrameSamples"],
                       "statistic": statistic, "sampling": sampling, "unit": unit, "cases": cases}
    rules["geometry_including_tes"]["scope"] = (
        "Complete geometry draw timestamp including terrain TES and other geometry; "
        "neither isolated terrain shading nor a separate TES GPU stage.")
    for name, key, sampling, unit, cases in (
            ("process_to_interactive_ms", "maxProcessToInteractiveSeconds", "per-run", "ms", ["cold"]),
            ("world_commit_ms", "maxWorldCommitSeconds", "per-run", "ms", ["cold", "world_replace"]),
            ("convergence_ms", "maxPoseOrTeleportConvergenceSeconds", "per-run", "ms",
             ["cold", "hot", "tour", "teleport"]),
            ("production_gpu_ms", "singlePageProductionHardMs", "gpu-timestamp", "ms", ["cold", "tour"]),
            ("copy_gpu_ms", "singlePageReadbackCopyHardMs", "gpu-timestamp", "ms", ["cold", "tour"]),
            ("production_and_copy_gpu_ms", "singlePageProductionAndCopyHardMs", "gpu-timestamp", "ms",
             ["cold", "tour"]),
            ("frame_production_and_copy_gpu_ms", "perFrameProductionAndCopyHardMs", "gpu-timestamp", "ms",
             ["cold", "tour"]),
            ("process_private_bytes", "maxWholeProcessPrivatePeakBytes", "pid-memory", "bytes",
             ["cold", "hot", "tour", "world_replace"]),
            ("wddm_dedicated_shared_bytes", "maxWholeProcessWddmPeakBytes", "pid-memory", "bytes",
             ["cold", "hot", "tour", "world_replace"])):
        rules[name] = {"maximum": targets[key] * (1000 if key.endswith("Seconds") else 1),
                       "min_samples": targets["minimumValidWddmSamples"] if sampling == "pid-memory" else 1,
                       "statistic": "max", "sampling": sampling, "unit": unit, "cases": cases}
    steady = document["steadyRvtCaps"]
    budgets = {
        "cache_workspace_bytes": steady["physicalPoolGpuBytes"] + steady["workspaceGpuBytes"],
        "shared_inputs_bytes": steady["inputGpuBytes"],
        "terrain_total_bytes": document["terrainTotalCaps"]["steadyGpuBytes"],
        "replacement_peak_bytes": document["terrainTotalCaps"]["candidateActiveRetiringGpuBytes"],
        "cpu_total_bytes": document["terrainTotalCaps"]["steadyCpuBytes"],
        "pages_per_frame": targets["productionPagesPerFrame"],
        "bytes_per_frame": targets["productionBytesPerFrame"],
        "inflight_tasks": targets["maxGlobalInFlightProductionTasks"],
    }
    return {"status": "frozen", "baseline_valid": True, "measurement_run_id": "quiet-final",
            "native_p0": artifact(path), "native_review": audit["review_receipt"], "native_limits": document,
            "hardware": document["conditions"], "display": {"resolution": document["conditions"]["resolution"]},
            "build": {"mode": document["conditions"]["build"]}, "cache_scope": document["cacheConditions"],
            "quality": document["qualityTargets"], "budgets": budgets, "performance": rules,
            "baseline_receipt": artifact(root / "quiet-final/report.json"),
            "measurement_artifacts": [artifact(root / name) for name in document["evidenceFiles"]],
            "comparison_requirements": {key: value for key, value in targets.items() if "Improvement" in key}}


def inspect_package(package, scene, weight_source):
    package = package.resolve(strict=True)
    installed_frozen = package / "config/terrainRvtP0Frozen.json"
    require(sha256(installed_frozen) == APPROVED_P0_SHA256 and
            (package / "config/terrainRvtP0Frozen.json.sha256").read_text().split()[0] == APPROVED_P0_SHA256,
            "Offline Game frozen config/sidecar differs from approved original")
    export_receipt = read_json(package / "game-export-manifest.json")
    require(export_receipt.get("terrainRvtFrozenSha256") == APPROVED_P0_SHA256,
            "Delivery manifest did not lock approved P0 SHA256")
    validator_name = os.environ.get("VULKANLEARN_RVT_VALIDATOR")
    require(validator_name, "Set VULKANLEARN_RVT_VALIDATOR to the frozen native vl_export_game.exe")
    validator = Path(validator_name).resolve(strict=True)
    require(validator.name.lower() == "vl_export_game.exe", "Native exporter validator required")
    before = sha256(validator)
    result = subprocess.run([str(validator), "--verify-package", str(package)],
                            cwd=package, capture_output=True, timeout=300, check=False,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    require(result.returncode == 0, "Native Store/exporter rejected installed closure: " +
            (result.stdout + result.stderr).decode("utf-8", errors="replace"))
    require(sha256(validator) == before, "Validator changed while inspecting package")
    config = read_json(package / "config/config.json")
    require(config["resourcePath"] == "resources", "Game must use package-local resources")
    resources = (package / "resources").resolve(strict=True)
    document = read_json(reference_path(resources, scene))
    placements = [item for item in document["objects"] if item.get("type") == "terrain"]
    ids = sorted("terrain|" + item["name"] for item in placements)
    require(len(ids) == 16 and len(set(ids)) == 16, "Full16 placement identity required")
    manifests = set()
    for item in placements:
        terrain = read_json(reference_path(resources, item["terrainPath"]))
        settings = terrain["compositedVT"]
        require(not {"residentPages", "heightBudgetBytes"} & settings.keys(), "Retired TR budget fields")
        manifests.add(settings["packagePath"])
    require(len(manifests) == 1, "Full16 must share one World V2 manifest")
    manifest_name = manifests.pop()
    manifest = read_json(reference_path(resources, manifest_name))
    require(manifest["version"] == 2, "V1 has no runtime fallback")
    modes = {"CoverageAtlas": 0, "LocalLayerArray": 1}
    require(weight_source in modes and all(item["weightSource"] == modes[weight_source]
            for item in manifest["production"]["surfaces"]), "Installed weight source differs")
    files = published_world_files(resources, manifest_name, ids)
    closure = {name: {"bytes": path.stat().st_size, "sha256": sha256(path)}
               for name, path in sorted(files.items())}
    return {"version": 2, "surface_ids": ids, "weight_source": weight_source,
            "content_identity": canonical_hash(manifest),
            "closure_sha256": canonical_hash(closure), "validator_sha256": before,
            "frozen_sha256": APPROVED_P0_SHA256,
            "package": str(package), "scene": scene,
            "binary_sha256": sha256(package / "main_game.exe"),
            "shader_sha256": sha256(package / "shader/runtime-shader.manifest.json"),
            "closure_files": closure, "manifest": manifest_name}


def verify_case(case, evidence, frozen, package_summary):
    entries = evidence.get("artifacts", [])
    process_entries = [entry for entry in entries if entry.get("role") == "process"]
    require(len(process_entries) == 1, "Case needs one observed process artifact")
    base = Path(evidence["artifact_root"]).resolve(strict=True)

    def checked(entry):
        path = reference_path(base, entry["path"])
        logical = Path(entry["path"])
        ancestor = base
        for component in logical.parts:
            ancestor /= component
            require(not ancestor.is_symlink(), "Native evidence artifact cannot use a symlink")
        require(sha256(path) == entry["sha256"] and path.stat().st_size == entry["bytes"],
                "Native case artifact identity differs")
        return path

    process = read_json(checked(process_entries[0]))
    log_entries = [entry for entry in entries if entry.get("role") == "runtime_log"]
    require(len(log_entries) == 1, "Case needs one raw runtime log")
    log = checked(log_entries[0])
    require(process["logSha256"] == sha256(log), "Observed process log differs")
    argv = process["argv"]
    executable = Path(argv[0]).resolve(strict=True)
    require(sha256(executable) == package_summary["binary_sha256"], "Executed binary differs from package")
    require(argv[argv.index("--worker-thread-count") + 1] == str(case["worker"]), "Observed worker differs")
    require(Path(process["cwd"]).resolve() == Path(package_summary["package"]).resolve(),
            "Observed process ran outside independent package")
    text = log.read_text(encoding="utf-8", errors="replace")
    require(f"[TerrainRvtFrozen] sha256={APPROVED_P0_SHA256}" in text,
            "Game startup did not report reading the approved frozen configuration")
    events, parse_errors = extract(text, "[RVT_EVIDENCE] ")
    require(not parse_errors, "Malformed native RVT evidence events")
    for entry in entries:
        if entry.get("role") != "runner_event":
            continue
        external = read_json(checked(entry))
        require(external.get("event") == "installation" and external.get("tool") == "vl_export_game",
                "Unsupported runner event authority")
        checked(external["logArtifact"])
        require(external["argv"][1] == "--verify-package" and
                Path(external["argv"][2]).resolve() == Path(package_summary["package"]).resolve(),
                "Installation event verified a different package")
        events.append(external)
    identity = [event for event in events if event.get("event") == "identity"]
    identity_ok = len(identity) == 1 and all(
        identity[0].get(field) == expected for field, expected in (
            ("scene", package_summary["scene"]), ("worker", case["worker"]),
            ("weightSource", case["weight_source"]), ("contentIdentity", package_summary["content_identity"]),
            ("shaderSha256", package_summary["shader_sha256"])))
    checks = {"observed_run_identity": identity_ok}
    quality = [event for event in events if event.get("event") == "quality"]
    if quality:
        from rvt_sample_evidence import quality_counts
        from rvt_artifact_binding import load_quality_json
        from rvt_artifact_binding import read_bound_bytes
        from rvt_quality_binary import quality_binary_counts, decode_bound_capture
        manifest = read_json(reference_path(
            Path(package_summary["package"]) / "resources", package_summary["manifest"]))
        surfaces = {row["surfaceId"]: row for row in manifest["surfaces"]}
        for observation in quality:
            entry = observation["samplesArtifact"]
            require(any(item["path"] == entry["path"] and item["sha256"] == entry["sha256"]
                        for item in entries), "GPU readback is not a case-bound raw artifact")
            if entry.get("format") == "terrain-quality-v1":
                data = read_bound_bytes(base, entry)
                if "metadataArtifact" in observation:
                    metadata_entry = observation["metadataArtifact"]
                    require(any(item["path"] == metadata_entry["path"] and
                                item["sha256"] == metadata_entry["sha256"] for item in entries),
                            "Quality metadata is not bound to case artifacts")
                    metadata = json.loads(read_bound_bytes(base, metadata_entry, maximum=65536).decode("utf-8"))
                    decode_bound_capture(data, metadata, observation, surfaces)
                else:
                    quality_binary_counts(data, observation, surfaces)
            else:
                quality_counts(load_quality_json(base, entry), observation, surfaces)
    limits = frozen["native_limits"]
    target = limits["qualityTargets"]
    ready = [row for row in quality if row.get("phase") == "ready"]
    quality_ok = bool(ready) and all(
        row["visibleSamples"] > 0 and row["validAtTargetPlusBias"] / row["visibleSamples"] >=
        target["minReadyVisibleSampleFractionAtTargetPlusBias"] and
        row["effectiveMipBias"] <= target["maxEffectiveMipBias"] and
        row["unresolved"] == 0 and row["incorrectIdentity"] == 0 and row["rootOnlySamples"] == 0
        for row in ready)
    convergence = quality_ok and all(row["elapsedSincePoseMs"] <=
        limits["targets"]["maxPoseOrTeleportConvergenceSeconds"] * 1000 for row in ready)
    production = [row for row in events if row.get("event") == "publication"]
    produced = any(row.get("source") == "gpu" and row.get("status") == 0 and
                   row.get("fenceCompletedEpoch", -1) >= row.get("writeEpoch", 0) and
                   row.get("publishedEpoch", -1) > row.get("writeEpoch", 0) for row in production)
    clean = process["exitCode"] == 0 and "EngineLoop teardown complete" in text
    validation_clean = not re.search(r"VUID-|Validation Error|THREADING", text)
    no_compile = not re.search(r"Shader cache miss:|compiled=[1-9]|shaderc=[1-9]", text)
    name = case["case"]
    if name not in ("input_corrupt", "v1_reject", "legacy_switch_reject"):
        require(clean and validation_clean and not process.get("timeout", False),
                "Game case failed, timed out, or lacked clean validation/teardown")

    def event(kind):
        matches = [row for row in events if row.get("event") == kind]
        if not matches:
            raise IncompleteEvidence(f"{case['id']}: missing native {kind} telemetry; no inferred pass")
        return matches

    if name == "closure":
        actual = inspect_package(Path(package_summary["package"]), package_summary["scene"], case["weight_source"])
        same = actual == package_summary
        checks.update(dict.fromkeys(("v2_full16", "complete_inputs", "runtime_compute", "offline_paths"), same))
    elif name in ("cold", "hot", "cache_deleted", "cache_corrupt"):
        caches = event("cache")
        cache = caches[-1]
        disabled = cache["mode"] == "disabled"
        require(cache["osCacheCleared"] is False, "This protocol does not clear or claim cold OS cache")
        checks["converged"] = convergence
        if name == "cold":
            checks.update(empty_fine_cache=disabled or cache["initialFineBytes"] == 0,
                          initial_quality=quality_ok, produced_pages=produced)
        elif name == "hot":
            checks.update(same_cache_identity=cache["contentIdentity"] == package_summary["content_identity"],
                          cache_scope_recorded=cache["mode"] in ("disabled", "bounded"))
        elif disabled:
            require(identity_ok and evidence.get("status") == "not-applicable"
                    and cache["initialFineBytes"] == 0 and cache["peakBytes"] == 0,
                    "Disabled disk cache needs observed identity and zero disk payload evidence")
            return {"status": "not-applicable", "reason": "optional-disk-cache-disabled",
                    "scope": "No cache deletion/corruption exercised; native cache mode is disabled"}
        elif name == "cache_deleted":
            checks.update(fine_cache_deleted=cache["deletedBytes"] > 0 and cache["initialFineBytes"] == 0,
                          produced_pages=produced)
        else:
            checks.update(corruption_observed=cache["rejectedCorruptEntries"] > 0,
                          bounded_rebuild=produced and cache["peakBytes"] <=
                          limits["targets"]["maxOptionalFineDiskCacheBytes"])
    elif name in ("tour", "teleport", "seams"):
        poses = event("pose")
        if name == "tour":
            centers = {row["name"] for row in poses if row.get("category") == "center"}
            crossings = event("crossing")
            checks.update(all_tile_centers=len(centers) == 16,
                          continuous_crossings=any(row["distanceCm"] > 0 and row["intermediateSamples"] >= 24
                                                   for row in crossings), converged=convergence)
        elif name == "teleport":
            arrivals = [row for row in ready if row.get("reason") == "teleport"]
            checks.update(unvisited_destinations=any(row["previousVisits"] == 0 for row in poses),
                          each_arrival_quality=quality_ok and len(arrivals) == len(poses),
                          each_arrival_converged=convergence and len(arrivals) == len(poses))
        else:
            counts = {category: len({row["name"] for row in poses if row.get("category") == category})
                      for category in ("edge", "junction", "outer")}
            comparisons = event("comparison")
            good = all(row["albedoMaxByteError"] <= target["basePageMaxAlbedoChannelByteError"] and
                       row["normalMaxByteError"] <= target["basePageMaxNormalChannelByteError"] and
                       row["ormMaxByteError"] <= target["basePageMaxOrmChannelByteError"] and
                       row["roiMaeBytes"] <= target["litTerrainRoiMaxMaeBytes"] and
                       row["roiRmseBytes"] <= target["litTerrainRoiMaxRmseBytes"] and
                       row["roiP99ErrorBytes"] <= target["litTerrainRoiMaxP99ErrorBytes"]
                       for row in comparisons)
            checks.update(all_24_edges=counts["edge"] == 24 and good,
                          all_9_junctions=counts["junction"] == 9 and good,
                          outer_4_corners=counts["outer"] == 4 and good,
                          coarse_footprints=good and any(row["mip"] > 0 and row["crossBoundarySamples"] > 0
                                                       for row in comparisons),
                          mixed_layer_mapping=good and any(row["distinctLayerMaps"] > 1 for row in comparisons))
    elif name == "tes":
        samples = event("tes")
        checks.update(active_distance=all(row["distanceCm"] < row["activeRadiusCm"] for row in samples),
                      nonzero_displacement=any(abs(row["displacementCm"]) > 0 for row in samples),
                      height_ready=all(row["heightInputsValidated"] == row["heightInputsExpected"] > 0
                                       for row in samples),
                      correct_weight_source=all(row["weightSource"] == case["weight_source"] and
                                               row["heightErrorR16"] <= target["tesMaxHeightErrorR16Units"]
                                               for row in samples),
                      independent_of_base_residency=any(row["baseFineResidentPages"] == 0 and
                                                       abs(row["displacementCm"]) > 0 for row in samples))
    elif name in ("input_corrupt", "v1_reject", "legacy_switch_reject"):
        rejection = event("rejection")
        rejected = process["exitCode"] == 1 and "World/graph transaction committed:" not in text
        teardown = "EngineLoop teardown complete" in text
        if name == "input_corrupt":
            checks.update(input_integrity_rejected=rejected and any(row["kind"] == "input-digest"
                                                                    for row in rejection),
                          no_candidate_commit=rejected, clean_teardown=teardown)
        else:
            checks.update(runtime_rejected=rejected, no_fallback=rejected,
                          no_crash=teardown and process["exitCode"] == 1)
            if name == "v1_reject":
                checks["installation_rejected"] = any(
                    row["tool"] == "vl_export_game" and row["exitCode"] != 0 for row in event("installation"))
    elif name == "root_corrupt":
        from accept_game_delivery import validate_recovery
        integrity = event("integrity")
        damaged = next((row for row in integrity if row["kind"] == "root"), None)
        require(damaged is not None, "No observed corrupt root key")
        validate_recovery(text, damaged, True)
        checks.update(root_rebuilt=produced,
                      no_uninitialized_draw=bool(ready) and all(row["uninitializedDraws"] == 0 for row in quality),
                      installation_verify_rejects=any(row["exitCode"] != 0 for row in event("installation")))
    elif name == "production_failure":
        failures = event("production-failure")
        slots = event("slot-retirement")
        checks.update(nonzero_status=any(row["status"] != 0 for row in failures),
                      submission_failure=any(row["submitResult"] != 0 for row in failures),
                      no_partial_publish=all(row["publishedChannels"] == 0 for row in failures),
                      bounded_retry=all(0 < row["attempts"] <= row["maxAttempts"] and row["backoffTicks"] > 0
                                        for row in failures),
                      slot_retirement=all(row["reuseEpoch"] > row["completedWriteEpoch"] for row in slots))
    elif name == "over_budget":
        accounts = event("admission")
        checks.update(frozen_caps=all(row["frozenSha256"] == frozen["native_p0"]["sha256"] for row in accounts),
                      no_peak_overrun=all(row["peakTerrainGpuBytes"] <=
                        limits["terrainTotalCaps"]["candidateActiveRetiringGpuBytes"] for row in accounts),
                      quality_floor=quality_ok,
                      no_starvation=all(row["oldestWaitMs"] <=
                        limits["targets"]["maxPoseOrTeleportConvergenceSeconds"] * 1000 for row in accounts))
    elif name == "world_replace":
        worlds = event("world")
        checks.update(different_world_roundtrip=len({row["scene"] for row in worlds}) >= 2 and
                      worlds[0]["scene"] == worlds[-1]["scene"],
                      generation_isolation=len({row["generation"] for row in worlds}) >= 3 and
                      all(row["stalePublications"] == 0 for row in worlds),
                      overlap_accounted=all(row["peakTerrainGpuBytes"] <=
                        limits["terrainTotalCaps"]["candidateActiveRetiringGpuBytes"] for row in worlds),
                      failed_candidate_preserves_old=any(row["candidateResult"] == "rejected" and
                        row["activeGenerationAfter"] == row["activeGenerationBefore"] for row in worlds))
    elif name == "editor_refresh":
        edits = event("editor")
        checks.update(independent_p4_receipt=all(row["reviewer"] != row["implementer"] and
                      row["reviewReceiptSha256"] for row in edits),
                      inflight_edits=any(row["inflightTasksAtEdit"] > 0 for row in edits),
                      no_stale_publish=all(row["stalePublications"] == 0 for row in edits))
    elif name == "offline_game":
        paths = event("file-access")
        install = event("installation-mode")
        scope = event("file-access-scope")
        require(all(row.get("scope") == "engine-controlled-asset-opens" for row in scope),
                "Only project-owned asset-open tracing is supported; do not claim OS-wide monitoring")
        root = Path(package_summary["package"]).resolve()
        bound = {entry["path"]: entry for entry in entries}
        required_acl = ("readonly-probe.json", "acl-protect.stdout", "acl-restore.stdout",
                        "package-acl-backup.json", "acl-command.json")
        require(all(item in bound for item in required_acl), "Missing actual NTFS protect/probe/restore evidence")
        probe = read_json(checked(bound["readonly-probe.json"]))
        observed_probe = json.loads(checked(bound["acl-protect.stdout"]).read_text(encoding="utf-8-sig"))
        restore = json.loads(checked(bound["acl-restore.stdout"]).read_text(encoding="utf-8-sig"))
        backup = read_json(checked(bound["package-acl-backup.json"]))
        acl_command = read_json(checked(bound["acl-command.json"]))
        require(probe == observed_probe and Path(probe["package"]).resolve() == root and
                probe["sid"] == backup["sid"] == restore["sid"] and
                restore["exactSddlRestored"] is True and restore["status"] == "restored" and
                artifact(Path(__file__).with_name("readonly_game_acl.ps1")) == acl_command["script"],
                "NTFS ACL script/probe/backup/restore identity differs")
        writes = sum(row.get("operation") in ("write", "create", "append", "delete") and
                     Path(row["path"]).resolve().is_relative_to(root) for row in paths)
        closure_valid = inspect_package(root, package_summary["scene"], case["weight_source"]) == package_summary
        cache_roots = [Path(row["cacheRoot"]).resolve() for row in install]
        system_root = Path(os.environ.get("SystemRoot", "C:/Windows")).resolve()
        checks.update(workspace_independent=closure_valid and all(
                      Path(row["path"]).resolve().is_relative_to(root) or
                      (row["class"] == "system" and Path(row["path"]).resolve().is_relative_to(system_root)) or
                      (row["class"] == "runtime-cache" and any(
                          Path(row["path"]).resolve().is_relative_to(cache_root) for cache_root in cache_roots))
                      or (row["class"] == "user-output" and Path(row["path"]).resolve().is_relative_to(base))
                      for row in paths),
                      read_only_install=probe["createFileDenied"] is True and
                      probe["existingFileWriteDenied"] is True and probe["writeProbeExit"] != 0 and
                      writes == 0 and all(row["installWriteAttempts"] == writes for row in install),
                      separate_writable_cache=all(not Path(row["cacheRoot"]).resolve().is_relative_to(root)
                                                  for row in install),
                      no_shader_compile=no_compile, validation_clean=validation_clean, clean_teardown=clean)
    missing = [gate for gate in case["gates"] if checks.get(gate) is not True]
    if missing:
        raise IncompleteEvidence(f"{case['id']}: unproven native gates: {', '.join(missing)}")
    return {"status": "passed", "checks": checks, "scope":
            ("Native closure validation plus project-owned asset-open trace and actual NTFS probes; "
             "no OS-wide process file monitoring. External screenshots are explicit user-output."
             if name == "offline_game" else "Computed from raw native events and observed process")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit-p0", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    audit = audit_p0(options.audit_p0)
    audit.pop("document")
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()

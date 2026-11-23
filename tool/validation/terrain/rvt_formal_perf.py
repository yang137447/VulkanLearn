"""CPU preparation/replay for frozen RVT performance; renderer launch is opt-in.

Only this file owns these tests (--self-test). Existing tour/case runners are
unchanged. Exit 2 means an observed frozen-limit failure; 3 means incomplete.
Neither a prepared plan nor metric replay signs off P2/P3/P5.

The existing dedicated sink envelope is sequence/steady_ns/eventClass/payload,
terminated by runtimeEvidenceEnd. GPU_RAW/CPU_RAW use that sink if present,
otherwise strictly complete stdout lines. identityVersion=2 is mandatory for
new runs; historical frame is a readback sequence, never a render frame.

Existing RVT_QUALITY_CLOCK/STATE/PRESENT/EVENT, RVT_CANDIDATE_STATE,
RVT_WORLD_CANDIDATE and RVT_PRODUCTION_RAW are the native facts. Normalized
report labels never require new engine event names. Runner command/camera and
bound capture evidence supply phase/pose alignment. No line-offset pairing.
"""

import argparse
from collections import defaultdict
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import ExitStack

from rvt_p0_analyze import distribution


ROOT = Path(__file__).resolve().parents[3]
FROZEN_SHA = "43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd"
RAW_CLASSES = ("GPU_RAW", "CPU_RAW")
NATIVE_CLASSES = ("RVT_QUALITY_CLOCK", "RVT_QUALITY_STATE", "RVT_QUALITY_PRESENT",
                  "RVT_QUALITY_EVENT", "RVT_CANDIDATE_STATE", "RVT_WORLD_CANDIDATE",
                  "RVT_PRODUCTION_RAW")
SECTIONS = ("far", "mid", "near", "tile00_near", "cross_tile_corner",
            "move_across_island", "teleport", "reload", "final_far")
GPU_CLASS = {"geometry": "geometry", "physical": "physical", "input": "input",
             "source": "input", "workspace": "workspace", "feedback": "workspace",
             "quality": "workspace", "taskslots": "workspace", "metadata": "workspace",
             "driver-reserve": "workspace"}
HOOKS = {
    "implemented-await-bound-output": [
        "identityVersion2/renderFrame/worldGeneration/submitEpoch in CPU_RAW and GPU_RAW; "
        "GPU readbackSequence is separately named and never used for ctxframe",
        "terrainQualityAfterFeedback query in each terrainFeedback frame; include it in ALL-pass sum",
        "RuntimeEvidence sequence/steady_ns/threadTag/eventClass/payload and clean runtimeEvidenceEnd",
        "Service RVT_QUALITY_CLOCK/STATE, candidate identity and quality event forwarding"],
    "not-yet-proven-by-package": [
        "Runner work, not a new engine API: actual command-send/camera-state + existing quality capture "
        "frame/pose/epoch alignment for original nine-section window; >=1000 complete raw frames/window",
        "Receipt/runner work: source/bin/kernel/newfeedbackUV/QualityTimer attribution and same-session "
        "device conditions. Existing RVT_PRODUCTION_RAW supplies timestampValidBits/period",
        "World commit exact native monotonic stamp not currently emitted; existing commit diagnostic "
        "can instead use P0-style measured observer interval, conservatively, without a new event name",
        "Decoder/runner work: existing RVT_QUALITY_STATE.elapsedMs supplies pose convergence; "
        "CLOCK+PRESENT supply interactive time; existing raw taps supply fallback displacement "
        "sample population. Missing binary samples stay missing, never zero-filled",
        "actual geometry/RVT/source/feedback/quality/task slots/metadata allocation capacities, "
        "explicit driver reserve, shared VkImage identity, Candidate+Active+Retiring overlap telemetry",
        "same-session hardware/driver/host audit, shader warm/no-miss and actual private fine-cache mode/root",
        "R9 final export receipt, package-local shared production input closure, cross-World scene pair",
        "original nine-section timing scene object closure (not the dynamic53 near-tour scene)"]
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def artifact(path):
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}


def strict_json(text):
    def duplicate_free(pairs):
        result = {}
        for name, value in pairs:
            require(name not in result, f"Duplicate JSON key: {name}")
            result[name] = value
        return result

    def invalid_constant(value):
        raise ValueError(f"Nonfinite JSON constant: {value}")

    def finite_float(value):
        result = float(value)
        require(math.isfinite(result), "Nonfinite JSON numeric exponent")
        return result

    value = json.loads(text, object_pairs_hook=duplicate_free, parse_constant=invalid_constant,
                       parse_float=finite_float)
    require(isinstance(value, dict), "JSON evidence row must be an object")
    return value


def read(path):
    return strict_json(Path(path).read_text(encoding="utf-8"))


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
                          encoding="utf-8")


def jsonl(path):
    text = Path(path).read_text(encoding="utf-8", errors="strict")
    require(not text or text.endswith("\n"), f"Truncated final JSONL line: {path}")
    return [strict_json(line) for line in text.splitlines() if line.strip()]


def number(value, positive=False):
    return type(value) in (int, float) and math.isfinite(value) and (
        value > 0 if positive else value >= 0)


def frozen(path):
    require(digest(path) == FROZEN_SHA, "P0Frozen differs from independently approved bytes")
    document = read(path)
    require(document["validRunIds"]["formalCpuAndGpu"] == ["quiet-final"],
            "Cannot substitute another formal denominator")
    return document


def stdout_records(text):
    records, errors = [], []
    marker = re.compile(r"\[(GPU_RAW|CPU_RAW|RVT_[A-Z_]+)\]")
    for line_number, line in enumerate(text.splitlines(keepends=True), 1):
        found = marker.search(line)
        if not found:
            continue
        prefix = found.group(1)
        tail = line[found.end():].strip()
        if prefix not in RAW_CLASSES and not tail.startswith("{"):
            continue
        try:
            require(found.start() == 0 and line.endswith("\n"),
                    "Raw stdout must be one complete unprefixed line")
            records.append({"eventClass": prefix, "payload": strict_json(tail),
                            "stdoutLine": line_number})
        except ValueError as error:
            errors.append({"line": line_number, "reason": str(error), "raw": line.rstrip("\n")})
    return records, errors


def dedicated_records(path):
    records = jsonl(path)
    require(records, "Empty dedicated evidence stream")
    require([row.get("sequence") for row in records] == list(range(1, len(records) + 1)),
            "Dedicated sink sequence missing/duplicated/reordered")
    require(all(type(row.get("steady_ns")) is int and row["steady_ns"] > 0
                and isinstance(row.get("payload"), dict)
                and row.get("outputCategory") == "explicitoutput" for row in records),
            "Incomplete dedicated envelope")
    require(all(second["steady_ns"] >= first["steady_ns"]
                for first, second in zip(records, records[1:])), "Sink clock went backwards")
    terminal = records[-1]
    require(terminal["eventClass"] == "runtimeEvidenceEnd"
            and terminal["payload"].get("incomplete") is False
            and terminal["payload"].get("recordsBeforeTerminal") == len(records) - 1,
            "Dedicated sink lacks normal complete terminal event")
    require(not any(row.get("eventClass") == "runtimeEvidenceEnd" for row in records[:-1]),
            "Premature dedicated terminal")
    return records


def load_native(directory):
    log = next((directory / name for name in ("runtime.log", "stdout.log")
                if (directory / name).is_file()), None)
    console, errors = stdout_records(log.read_text(encoding="utf-8", errors="strict")) if log else ([], [])
    sink = directory / "runtime-events.jsonl"
    native = []
    if sink.exists():
        try:
            native = dedicated_records(sink)
        except ValueError as error:
            errors.append({"source": str(sink), "reason": str(error)})
    sources = {}
    rows = {}
    for category in RAW_CLASSES:
        preferred = [row["payload"] for row in native if row["eventClass"] == category]
        fallback = [row["payload"] for row in console if row["eventClass"] == category]
        rows[category] = preferred if preferred else fallback
        sources[category] = "dedicated-jsonl" if preferred else "strict-stdout-fallback"
        if preferred and fallback and preferred != fallback:
            errors.append({"source": category, "reason": "Dedicated/stdout raw streams differ"})
    native_classes = {row["eventClass"] for row in native}
    combined = native + [row for row in console if row["eventClass"] not in native_classes] if native else console
    return rows, combined, errors, sources


def timing(gpu, cpu, minimum, required_passes, modern=True):
    problems, totals, geometry, by_pass = [], [], [], defaultdict(list)
    for index, row in enumerate(gpu):
        passes = row.get("passes")
        if not isinstance(passes, list) or not passes:
            problems.append(f"GPU row {index}: missing passes")
            continue
        names = [item.get("name") for item in passes if isinstance(item, dict)]
        valid = (len(names) == len(passes) and all(isinstance(name, str) for name in names)
                 and len(set(names)) == len(names) and set(required_passes) <= set(names)
                 and all(number(item.get("ms")) for item in passes))
        if not valid:
            problems.append(f"GPU row {index}: null/missing/duplicate query or required pass")
            continue
        totals.append(sum(item["ms"] for item in passes))
        for item in passes:
            by_pass[item["name"]].append(item["ms"])
            if item["name"] == "geometry":
                geometry.append(item["ms"])
    intervals = []
    for index, row in enumerate(cpu):
        if number(row.get("renderFrameIntervalMs")):
            intervals.append(row["renderFrameIntervalMs"])
        else:
            problems.append(f"CPU row {index}: missing/null wall interval; retained, never filtered for pass")
    if len(gpu) < minimum or len(cpu) < minimum:
        problems.append(f"Require >= {minimum} raw CPU and GPU frames in the declared window")
    identities = {}
    if modern:
        for label, rows in (("GPU", gpu), ("CPU", cpu)):
            keys = []
            for index, row in enumerate(rows):
                values = tuple(row.get(field) for field in ("renderFrame", "worldGeneration", "submitEpoch"))
                if (row.get("identityVersion") != 2 or any(type(value) is not int or value < 0 for value in values)
                        or any(value <= 0 for value in values[1:])
                        or type(row.get("frame")) is not int or row["frame"] != values[0]):
                    problems.append(f"{label} row {index}: native frame identity unavailable")
                    continue
                keys.append(values)
                if label == "GPU":
                    if (type(row.get("readbackSequence")) is not int
                            or not number(row.get("readbackMonotonicNs"), positive=True)):
                        problems.append(f"GPU row {index}: missing actual readback metadata")
                elif not number(row.get("sampleMonotonicNs"), positive=True):
                    problems.append(f"CPU row {index}: missing native sample clock")
            frames = [key[0] for key in keys]
            if len(keys) != len(rows) or len(set(keys)) != len(keys) or (
                    frames and frames != list(range(frames[0], frames[0] + len(frames)))):
                problems.append(f"{label}: incomplete/noncontiguous rendered-frame identities")
            identities[label] = keys
        if set(identities["GPU"]) != set(identities["CPU"]):
            problems.append("CPU/GPU native identity sets differ; no offset pairing or tail deletion")
        readbacks = [row.get("readbackSequence") for row in gpu]
        if (readbacks and all(type(value) is int for value in readbacks)
                and readbacks != list(range(readbacks[0], readbacks[0] + len(readbacks)))):
            problems.append("GPU readback sequence missing/duplicate; independent from render frame IDs")
    return {"gpuFrames": len(gpu), "cpuFrames": len(cpu), "problems": problems,
            "gpuPassSumMs": distribution(totals), "geometryIncludingTesMs": distribution(geometry),
            "rawCpuIntervalMs": distribution(intervals),
            "byPassMs": {name: distribution(values) for name, values in by_pass.items()},
            "statisticsEligible": not problems,
            "scope": "Sum ALL queries per frame, then percentile. Invalid rows retained as incompleteness; "
                     "CPU includes pacing/capture. Not pure TES or uninstrumented GPU time."}


def moment(row, domain):
    if domain == "gpu":
        value = datetime.fromisoformat(row["utc"].replace("Z", "+00:00"))
        require(value.tzinfo is not None, "WDDM UTC lacks timezone")
        return value.timestamp()
    if number(row.get("elapsedMs")):
        return row["elapsedMs"] / 1000
    require(number(row.get("observerMonotonic")), "CPU sample lacks actual observer clock")
    return row["observerMonotonic"]


def memory(rows, pid, domain, targets, normal_exit):
    problems, valid, unavailable, times = [], [], [], []
    fields = ("dedicatedBytes", "sharedBytes") if domain == "gpu" else ("privateBytes", "workingBytes")
    for index, row in enumerate(rows):
        try:
            require(row.get("pid") == pid, "Wrong/missing PID")
            require(type(row.get("available")) is bool, "Missing availability")
            if row["available"] is False:
                unavailable.append(index)
                continue
            require(all(number(row.get(field)) for field in fields), "Missing/nonfinite memory value")
            require(row[fields[0]] > 0, "Zero dedicated/private usage is not a valid displacement metric")
            if domain == "gpu":
                require(all(type(row.get(field)) is int and row[field] > 0
                            for field in ("dedicatedInstances", "sharedInstances")),
                        "No actual target-PID Dedicated/Shared counter instances")
            times.append(moment(row, domain))
            valid.append(row)
        except (ValueError, KeyError, TypeError) as error:
            problems.append(f"{domain} memory row {index}: {error}")
    terminal_allowed = unavailable == [len(rows) - 1] and normal_exit
    if unavailable and not terminal_allowed:
        problems.append(f"{domain}: interior/unproven terminal unavailable rows {unavailable}")
    if len(valid) < targets["minimumValidWddmSamples"]:
        problems.append(f"{domain}: fewer than 20 actual valid PID samples")
    gaps = [second - first for first, second in zip(times, times[1:])]
    if any(gap <= 0 or gap > targets["maxWddmSampleGapSeconds"] for gap in gaps):
        problems.append(f"{domain}: nonmonotonic sampling or gap >2.5s")
    values = [row["dedicatedBytes"] + row["sharedBytes"] if domain == "gpu"
              else row["privateBytes"] for row in valid]
    return {"validSamples": len(valid), "unavailableRows": unavailable,
            "allowedNormalTeardownUnavailable": terminal_allowed,
            "peakBytes": max(values, default=None), "actualCadenceSeconds": distribution(gaps),
            "osPeakPagefileBytes": max((row.get("peakPagefileBytes", 0) for row in valid), default=None)
            if domain == "cpu" else None, "problems": problems,
            "scope": "Actual sample cadence, not requested500ms. Simultaneous dedicated+shared "
                     "includes driver/source; sampled peaks are not exact transient high-water."}


def ledger(snapshot, config):
    allocations = snapshot.get("allocations")
    require(isinstance(allocations, list) and allocations, "Missing actual memory ledger allocations")
    require(snapshot.get("observedRuntime") is True, "Static arithmetic is not runtime peak telemetry")
    seen, totals, active, geometry_by_world = {}, defaultdict(int), defaultdict(int), defaultdict(int)
    for row in allocations:
        domain, category = row.get("memoryDomain"), row.get("class")
        require(domain in ("gpu", "cpu") and category in GPU_CLASS, "Unclassified allocation")
        require(not (domain == "gpu" and category == "source") or row.get("kind") == "image",
                "GPU source must identify the actual shared image, not a content hash or CPU payload")
        require(number(row.get("bytes")), "Allocation/capacity missing")
        require(row.get("basis") in ("memoryRequirements", "container-capacity", "explicit-reserve"),
                "Payload sizes do not prove allocation requirements or metadata capacity")
        if domain == "gpu" and row.get("kind") == "image":
            identity = row.get("imageIdentity")
            require(isinstance(identity, str) and identity, "Source image lacks actual shared image identity")
        else:
            identity = row.get("allocationIdentity")
            require(isinstance(identity, str) and identity, "Missing allocation identity")
        require(not identity.startswith(("sha256:", "hash:")), "Hash deduplication is forbidden")
        owners = row.get("owners")
        require(isinstance(owners, list) and owners and all(
            owner.get("state") in ("Candidate", "Active", "Retiring")
            and type(owner.get("worldGeneration")) is int for owner in owners),
            "Missing actual Candidate/Active/Retiring ownership")
        key = (domain, identity)
        signature = (category, row["bytes"], row["basis"])
        if key in seen:
            require(seen[key] == signature, "Conflicting shared allocation identity")
            raise ValueError("Ledger must emit each shared allocation once with all actual owners")
        seen[key] = signature
        totals[(domain, GPU_CLASS[category])] += row["bytes"]
        if any(owner["state"] == "Active" for owner in owners):
            active[(domain, GPU_CLASS[category])] += row["bytes"]
        if category == "geometry":
            for world in {owner["worldGeneration"] for owner in owners}:
                geometry_by_world[(domain, world)] += row["bytes"]
    require(any(row["class"] == "driver-reserve" and row["basis"] == "explicit-reserve"
                for row in allocations), "Missing explicit driver/descriptor metadata reserve")
    require(any(row["memoryDomain"] == "cpu" and row["basis"] == "container-capacity"
                and row["class"] == "metadata" for row in allocations), "Missing CPU metadata containers")
    require({"geometry", "source", "physical", "feedback", "quality", "taskslots"} <= {
        row["class"] for row in allocations}, "Incomplete geometry/RVT/source/feedback/quality/taskslots ledger")
    require(type(snapshot.get("globalInFlightTasks")) is int and type(snapshot.get("physicalSlots")) is int,
            "Missing actual task/slot occupancy")
    failures = []
    for label, values, caps, outer in (
            ("steady", active, config["steadyRvtCaps"], config["terrainTotalCaps"]),
            ("peak", totals, config["peakRvtOwnerCaps"], config["terrainTotalCaps"])):
        limits = {"physical": caps["physicalPoolGpuBytes"], "input": caps["inputGpuBytes"],
                  "workspace": caps["workspaceGpuBytes"]}
        for category, maximum in limits.items():
            if values[("gpu", category)] > maximum:
                failures.append(f"{label} GPU {category} budget exceeded")
        for domain, suffix in (("gpu", "Gpu"), ("cpu", "Cpu")):
            rvt = sum(value for (kind, category), value in values.items()
                      if kind == domain and category != "geometry")
            terrain = rvt + values[(domain, "geometry")]
            maximum = outer["steady" + suffix + "Bytes"] if label == "steady" else (
                outer["candidateActiveRetiring" + suffix + "Bytes"])
            if rvt > caps["total" + suffix + "Bytes"] or terrain > maximum:
                failures.append(f"{label} {domain} RVT/outer terrain budget exceeded")
    if snapshot["globalInFlightTasks"] > config["targets"]["maxGlobalInFlightProductionTasks"]:
        failures.append("Global task cap exceeded")
    if snapshot["physicalSlots"] > config["peakRvtOwnerCaps"]["physicalSlots"]:
        failures.append("Global physical-slot cap exceeded")
    for (domain, world), value in geometry_by_world.items():
        maximum = config["terrainTotalCaps"]["geometry" + domain.title() + "BytesPerWorld"]
        if value > maximum:
            failures.append(f"World {world} {domain} geometry cap exceeded")
    return {"totals": {f"{domain}:{category}": value for (domain, category), value in totals.items()},
            "failures": failures, "ownershipStates": sorted({owner["state"] for row in allocations
                                                            for owner in row["owners"]})}


def compare(measurements, targets):
    failures = []
    for name, value in measurements.items():
        require(name in targets, f"Unknown frozen metric {name}")
        if number(value) and value > targets[name]:
            failures.append({"metric": name, "observed": value, "maximum": targets[name]})
    return failures


def existing_native_facts(events):
    grouped = {name: [] for name in NATIVE_CLASSES}
    for row in events:
        if row.get("eventClass") in grouped:
            grouped[row["eventClass"]].append(row["payload"])
    clocks = [row for row in grouped["RVT_QUALITY_CLOCK"] if row.get("initialWorld") is True
              and number(row.get("processStartedMonotonicMs"))]
    interactive = []
    for row in grouped["RVT_QUALITY_PRESENT"]:
        if row.get("event") != "loading_released" or row.get("ready") is not True:
            continue
        matching = [clock for clock in clocks if clock.get("worldGeneration") == row.get("worldGeneration")]
        if matching and number(row.get("monotonicMs")):
            elapsed = (row["monotonicMs"] - matching[0]["processStartedMonotonicMs"]) / 1000
            require(number(elapsed), "Existing native CLOCK/PRESENT clock mismatch")
            interactive.append({"seconds": elapsed, "worldGeneration": row["worldGeneration"],
                                "frame": row.get("frame"), "submissionEpoch": row.get("submissionEpoch"),
                                "source": "RVT_QUALITY_CLOCK + RVT_QUALITY_PRESENT.loading_released"})
    convergence = []
    for row in grouped["RVT_QUALITY_STATE"]:
        if row.get("interactive") is True and number(row.get("elapsedMs")):
            convergence.append({"seconds": row["elapsedMs"] / 1000,
                                "worldGeneration": row.get("worldGeneration"), "poseId": row.get("poseId"),
                                "submissionEpoch": row.get("submissionEpoch"),
                                "source": "RVT_QUALITY_STATE.elapsedMs; exact capture camera still required"})
    calibration = [row for row in grouped["RVT_PRODUCTION_RAW"] if
                   type(row.get("timestampValidBits")) is int and 0 < row["timestampValidBits"] <= 64
                   and number(row.get("timestampPeriodNanoseconds"), positive=True)]
    return {"counts": {name: len(rows) for name, rows in grouped.items()},
            "interactive": interactive, "poseConvergenceObservations": convergence,
            "timestampCalibrationObservations": len(calibration),
            "phaseMapping": {"loading": "RVT_QUALITY_PRESENT.loading_black_drawn",
                             "interactive": "RVT_QUALITY_PRESENT.loading_released + initial RVT_QUALITY_CLOCK",
                             "pose-ready": "RVT_QUALITY_STATE + exact capture_completed/metadata/raw/frame identity",
                             "streaming-window": "Runner actual command-send/camera observations + native pose changes",
                             "steady-window": "Original prespecified nine route; actual camera/raw quality proof. "
                                              "No inferred frame offsets or fastest-window selection"},
            "memoryMapping": "Existing RVT_CANDIDATE_STATE.candidateOwnerGpuBytes and TerrainVT stats "
                             "are partial measured facts, not a complete classified overlap ledger",
            "worldCommitMapping": "World/graph transaction committed diagnostic is the activation commit. "
                                  "RVT_WORLD_CANDIDATE.jointSwitch is an input/producer switch, not World commit",
            "newNativeEventNamesRequired": []}


def recompute_p0(config):
    root = Path(config["evidenceRoot"])
    verified = []
    for name, entry in config["evidenceFiles"].items():
        require(entry["sha256"] == config["evidenceSha256"][name], f"P0 inventory conflict: {name}")
        require(artifact(entry["path"]) == entry, f"P0 evidence bytes changed: {name}")
        verified.append(name)
    gpu, cpu = jsonl(root / "quiet-final/gpu-raw.jsonl"), jsonl(root / "quiet-final/cpu-raw.jsonl")
    result = timing(gpu, cpu, 1000, ("geometry",), modern=False)
    expected = config["measured"]
    for actual_name, expected_name in (
            ("gpuPassSumMs", "quietGpuInstrumentedPassSumMs"),
            ("geometryIncludingTesMs", "quietGeometryIncludingTesMs"),
            ("rawCpuIntervalMs", "quietUnclampedRenderFrameIntervalMs")):
        for key, value in result[actual_name].items():
            require(math.isclose(value, expected[expected_name][key], rel_tol=1e-10, abs_tol=1e-8),
                    f"P0 raw recomputation differs: {actual_name}.{key}")
    runs = {}
    for name in (*config["validRunIds"]["fullRouteMemoryAndReload"], "quiet-final"):
        report = read(root / name / "report.json")
        process = report["process"]
        require(digest(root / name / "runtime.log") == process["logSha256"], "P0 runtime log changed")
        runs[name] = {domain: memory(jsonl(root / name / f"runtime.{domain}-memory.jsonl"),
                                    process["pid"], domain, config["targets"], process["exitCode"] == 0)
                      for domain in ("cpu", "gpu")}
        require(not any(item["problems"] for item in runs[name].values()), f"P0 memory incomplete: {name}")
    full = config["validRunIds"]["fullRouteMemoryAndReload"]
    require(min(runs[name]["cpu"]["peakBytes"] for name in full) == expected["fullRoutePrivatePeakReferenceBytes"],
            "P0 full-route private denominator differs")
    require(min(runs[name]["gpu"]["peakBytes"] for name in full) == expected["fullRouteWddmPeakReferenceBytes"],
            "P0 full-route WDDM denominator differs")
    return {"status": "historical-recomputed", "verifiedEvidenceFiles": len(verified),
            "timing": result, "memory": runs, "formalPerformancePassed": False,
            "identityScope": "1081 CPU/1080 GPU retained independently. Historical frame=readbackSequence, "
                             "not ctxframe; this recomputation does not pair or certify modern phases."}


def bound_inventory(root, prefixes):
    entries = []
    for prefix in prefixes:
        path = root / prefix
        candidates = sorted(path.rglob("*")) if path.is_dir() else [path]
        for candidate in candidates:
            if candidate.is_file() and "__pycache__" not in candidate.parts:
                entries.append({**artifact(candidate), "relative": candidate.relative_to(root).as_posix()})
    return entries


def handoff_text(root, output, plan=None, plan_name="plan.json"):
    tool = str(root / "tool/validation/terrain/rvt_formal_perf.py")
    binding = (plan or {}).get("packageBinding")
    if binding:
        return f"""Actual package-bound CPU checkpoint; NO GPU launched.
State: {plan['status']}
Package: {plan['package']}
Mode: {plan['weightSource']}
Game: {binding['actualGame']['path']}
Game SHA256: {binding['actualGame']['sha256']}
Core: {binding['coreVersion']}
Checkpoint SHA256: {binding['coreCheckpoint']['sha256']}
Frozen source/CMake: {binding['frozenSourceCount']}/{binding['frozenCmakeCount']}, before=after.
Package files/bytes: {binding['files']}/{binding['physicalBytes']}
Actual native CPU Verify: exit{binding['installationVerify']['exitCode']}
Bound plan: {output / plan_name}

CPU tests:
py -3 "{tool}" --self-test
CPU replay of actual terminated evidence (no renderer):
py -3 "{tool}" --analyze "$CompletedRun" --plan "{output / plan_name}" --output "$ReplayJson"

Exact executable command AFTER fixed-core rebind + main GPU window:
py -3 "{tool}" --execute --plan "{output / plan_name}" --gpu-window "$Window" --case warm-nine-section --output "$RunOutput"
Other separately authorized cases: cold-full-reload, warm-full-reload.
Window binds authority=main, exclusiveGpu=true, planSha256,
coreCheckpointSha256, ownedPackageReceiptSha256, case, notBeforeUtc, expiresUtc.
These are scheduling authorizations, never metric pass gates.
Current R10 known-failure binding REFUSES all formal launches before creating a
renderer: center_0_2/center_1_2 Coverage worker2 failed10s with fresh0/14of16384.
No Local GPU failure is inferred. Nine trace does not replace the failed centers.
New core/bin/kernel needs actual Export/Verify and a fresh binding, not a plan rename.

Actual original P0 route:
quiet-final nine sections,35 exact camera actions,922 commands,185-command tail.
Native camera acknowledgements must match all35 in order. Queue/send is recorded
separately from actual applied camera. No command index is treated as ctxframe.
Window endpoints come from existing loading_released frame/world/submission and
last completed GPU query; every inside raw CPU/GPU identity/query is checked.
Require>=1000 frames inside, retain all outside startup/teardown separately.
The first boundary rule is fixed in the tool, never picked for a fast percentile.
Cold/warm full reload keeps raw-cold ordering/delay and maps only loadworld path
to this installed scene: {binding['scene']}
This is same-asset/new-World-generation reload; a different B scene is not in the
installation. Do not invent a B path or claim an A-B-A scene test.

Inputs/readonly:
Actual package inventory includes DLLs,config,shader/cache/manifest,UI,resources.
All shared production objects resolve INSIDE this installation and are hash bound.
Hashes identify files, NOT shared GPU images/allocations.
P0 Frozen SHA256={FROZEN_SHA}; all27 original evidence files and original
quiet-final command/report/raw paths remain reference dependencies.
Private cache root is external/shared across cold/warm for this plan; no OS cache
purge and no automatic file deletion. Native fineDiskCache=disabled means N/A,
not a proven warm fine-cache speedup. Actual native mode must be recorded.
Owned export receipt: {binding['readonlyOwnership']['path']}
No NTFS protection/write probe/ACL restore has been executed in this CPU stage.
Later execute uses existing readonly_package Protect/probe/finally Restore only
inside the explicit window. All logs/captures/cache/ACL backup remain external.
Worker2/1280x720/MSAA2/UIoff/immediate30Hz/validationoff,quality capture ON.

Existing native consumers:
CLOCK+PRESENT -> initial interactive elapsed; STATE.elapsedMs+exact capture
frame/pose/epoch -> convergence. Existing decoder verifies raw identity/counts.
Actual raw taps -> fallback displacement mip levels, positive sample population.
GPU_RAW -> true renderFrame/world/submitEpoch and ALL-pass sum including
terrainQualityAfterFeedback; readbackSequence stays independent.
Existing allocationQuery/capture requirements/native owner stats are retained
and normalized without requiring any new event label or adding repeated images.
TerrainVT/scalar owner reports are consumed as actual bounded facts, not summed
across16 surface lines when they repeat a shared owner.
World commit existing diagnostic uses actual runner observation, not
RVT_WORLD_CANDIDATE producer/input jointSwitch.

Minimum native facts still needed IF absent from actual supplied ledger:
1. Renderer-wide simultaneous cost-by-class/phase current+peak and owner identity,
   including actual Candidate/Active/Retiring World overlap.
2. Actual shared image/allocation identities and requirements/capacity for source,
   geometry,feedback/quality/task slots; CPU metadata container capacities and
   explicit driver reserve. Existing registry reports can supply them.
No new event names required. A scalar candidateOwnerGpuBytes or allocationQuery
only proves its stated subset; private/WDDM remain separate actual PID samples.
Per-frame pose attribution need not be a new engine event: command acknowledgements
and existing raw capture camera/frame identities provide runner alignment.
Exact native World-commit monotonic timestamp is optional: a measured observer
upper bound is conservative; do not call an uncertain over-limit observation a
proven native-time failure.

Status is CPU-bound/checkpoint ONLY. P2/P3/P5 and formal performance NOT accepted.
Preserve original two-point failures; optimize source, never expected/caps/quality.
"""
    return f"""Unbound CPU preparation; NO GPU launched.
Source root: {root}
Unbound plan: {output / 'plan.json'}

Runnable now (CPU only):
py -3 "{tool}" --self-test
py -3 "{tool}" --p0-recompute --output "{output / 'p0-recomputed.json'}"

After main provides actual coherent package/export receipt:
Set $Package, $Receipt, $BoundPlanRoot to absolute paths supplied by main.
py -3 "{tool}" --prepare-only --package "$Package" --owned-package-receipt "$Receipt" --output "$BoundPlanRoot"

Only after E004 and main's explicit exclusive GPU window:
Set $Window and $RunOutput to main's authorization JSON and fresh external output.
py -3 "{tool}" --execute --plan "$BoundPlanRoot\\plan.json" --gpu-window "$Window" --case warm-nine-section --output "$RunOutput"
Repeat with separately authorized cold-full-reload and warm-full-reload cases.
Each authorization must bind authority=main, exclusiveGpu=true, coreCheckpointSha256,
planSha256, ownedPackageReceiptSha256, case, notBeforeUtc, expiresUtc (timezone explicit).
These fields authorize scheduling, NOT metric/quality pass gates.

Replay completed samples, CPU only:
py -3 "{tool}" --analyze "$RunOutput" --output "$RunOutput.formal-replay.json"

Input dependencies:
P0Frozen + SHA sidecar + all27 evidenceFiles; quiet-final command/report/GPU/CPU
and full-route cold/raw-cold/raw-warm-r2 memory streams; original P0 input-hashes.
Source/bin/shader-cache/manifest/coherent-core receipt and package-local immutable
shared-input closure. Private fine-cache empty/retained state must be observed;
OS file/standby cache is uncontrolled and not cleared. No cache deletion here.
Quality capture stays on, worker2/1280x720/MSAA2/UIoff/immediate30Hz/validationoff.
No53-pose scene substitution; original nine-section mapping is in plan.json.

Existing RuntimeEvidence API:
VULKANLEARN_RUNTIME_EVIDENCE=<external fresh output>/runtime-events.jsonl
sequence, steady_ns, threadTag, outputCategory=explicitoutput, eventClass, payload.
runtimeEvidenceEnd must be last, incomplete=false, clean child exit.
Raw classes prefer dedicated sink; absent raw classes use full-line stdout only.
Old frame readback counter never establishes ctxframe or CPU/GPU association.

Remaining package/runtime integration requirements:
{chr(10).join('- ' + hook for hook in HOOKS['not-yet-proven-by-package'])}

Always preserve incomplete/errors and whole raw streams. Whole-process timing is
diagnostic, not a warmed-window percentile. Observed bound failures must go to
main for source optimization; no expected/cap/quality/resolution edits.
No P2/P3/P5 acceptance is claimed.
"""


def route_argv(reference, package, output, scene=None):
    original = read(reference)["argv"]
    result = [str(package / "main_game.exe")]
    index = 1
    while index < len(original):
        argument = original[index]
        if argument == "--perf-jsonl":
            result.extend([argument, str(output / "runtime.perf.jsonl")])
            index += 2
        elif argument == "--console-command":
            command = original[index + 1]
            if command.startswith("screenshot "):
                command = "screenshot " + (output / command.replace("\\", "/").rsplit("/", 1)[-1]).as_posix()
            elif command.startswith("loadworld ") and scene:
                command = "loadworld " + scene
            result.extend([argument, command])
            index += 2
        else:
            result.append(argument)
            index += 1
    require(result[result.index("--console-command-delay") + 1] == "120", "Original route delay changed")
    require(result[result.index("--worker-thread-count") + 1] == "2", "Original worker changed")
    return result


def json_value(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def verify_bound_artifact(entry):
    require(artifact(entry["path"]) == {name: entry[name] for name in ("path", "bytes", "sha256")},
            f"Bound artifact changed: {entry['path']}")


def bind_package(package, receipt_path, mode, output):
    receipt = read(receipt_path)
    require(Path(receipt["package"]).resolve() == package, "Receipt belongs to another installation")
    require(receipt["exportExitCode"] == receipt["verifyExitCode"] == 0, "Actual native export/verify missing")
    for name in ("coreCheckpoint", "sourceManifest", "publicationReceipt", "exporter"):
        verify_bound_artifact(receipt[name])
    require(digest(package / "main_game.exe") == receipt["gameSha256"], "Installed Game differs from receipt")
    checkpoint = read(receipt["coreCheckpoint"]["path"])
    archive = Path(receipt["sourceManifest"]["path"]).parent
    before, after = read(archive / "source-before.json"), read(archive / "source-after.json")
    cmake_before, cmake_after = read(archive / "build-input-before.json"), read(archive / "build-input-after.json")
    require(before == after and len(before) == receipt["frozenSourceCount"] == checkpoint["sourceFileCount"]
            and checkpoint["sourceBeforeAfterUnchanged"] is True, "Frozen core source lock differs")
    # 冻结核心归档有两种记录口径：R9/R10 的 build-input-*.json 只保存
    # additionalCmakeInputCount 个增量 CMake 输入；R13/R14 保存全部
    # buildInputCount 个构建输入（含 CMake 与源码列表）。两者都必须 before==after，
    # 因此按 checkpoint 实际声明的 total 校验，不能把 120 项误判成缺 79 项。
    expected_build_inputs = checkpoint.get("buildInputCount") or checkpoint["additionalCmakeInputCount"]
    require(cmake_before == cmake_after and len(cmake_before) == expected_build_inputs
            and checkpoint["additionalCmakeInputsBeforeAfterUnchanged"] is True, "Frozen CMake lock differs")
    inventory_path = receipt_path.parent / "inventory.json"
    expected = json_value(inventory_path)
    canonical_identity = hashlib.sha256(json.dumps(
        expected, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    # R14 收据把 inventory 对象的规范化哈希写进 inventorySha256；R15 收据显式声明
    # “sha256 of the receipt-side inventory.json file bytes”。两种口径都必须只改变
    # “如何给同一份 inventory 取哈希”，不得改写收据/inventory，也不放宽任何门槛：
    # 文件闭包、逐文件大小与 sha256、总量、发布清单与 shader cache 仍全部逐项校验。
    if receipt.get("inventorySha256Algorithm") == "sha256 of the receipt-side inventory.json file bytes":
        inventory_identity = digest(inventory_path)
    else:
        inventory_identity = canonical_identity
    require(inventory_identity == receipt["inventorySha256"], "Package canonical inventory identity changed")
    actual_paths = []
    for path in sorted(package.rglob("*")):
        require(not path.is_symlink() and path.resolve().is_relative_to(package), "Package link/path escaped")
        if path.is_file():
            actual_paths.append(path.relative_to(package).as_posix())
    require(set(actual_paths) == {entry["path"] for entry in expected}, "Package file closure differs")
    require(len(actual_paths) == receipt["files"]
            and sum(entry["bytes"] for entry in expected) == receipt["physicalBytes"], "Package totals differ")
    for entry in expected:
        path = package / entry["path"]
        require(path.stat().st_size == entry["bytes"] and digest(path) == entry["sha256"],
                f"Installed bytes differ: {entry['path']}")
    from rvt_full_tour import package_inputs
    installed, surfaces, manifest_path = package_inputs(package, mode)
    publication = read(manifest_path)
    inputs = []
    for entry in publication["production"]["objects"]:
        path = (manifest_path.parent / entry["path"]).resolve(strict=True)
        require(path.is_relative_to(package / "resources") and path.stat().st_size == entry["bytes"],
                "Shared production input is not inside installed closure")
        inputs.append({"objectId": entry["objectId"], "kind": entry["kind"], **artifact(path),
                       "runtimeIdentity": "Actual resource/image identity still comes from native telemetry; "
                                          "package digest is NOT allocation deduplication"})
    require(digest(manifest_path) == receipt["publicationSha256"], "Published Store manifest differs")
    require(digest(package / "shader/spv/shader-build-cache.json") == receipt["shaderCacheSha256"],
            "Installed shader cache differs")
    ownership = receipt_path.parent / "export/package-ownership.json"
    from rvt_readonly_install import validate_owned_package
    validate_owned_package(package, ownership, output)
    validator = Path(receipt["exporter"]["path"])
    result = subprocess.run([str(validator), "--verify-package", str(package)], cwd=output,
                            capture_output=True, timeout=300, check=False,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    (output / "native-verify.log").write_bytes(result.stdout + result.stderr)
    require(result.returncode == 0, "Actual CPU Verify rejected this installation")
    verify_bound_artifact(receipt["exporter"])
    scene_path = package / "resources" / installed["initScene"]
    scene = read(scene_path)
    failure_path = ROOT / "artifacts/terrain-rvt-production/p5/R10-twofail-fresh-cpu-audit/summary.json"
    failure = read(failure_path) if failure_path.is_file() else None
    blocked = bool(failure and failure.get("gameSha256") == receipt["gameSha256"]
                   and failure.get("acceptancePassed") is False)
    failure_artifacts = ([artifact(failure_path)] + [
        artifact(failure_path.parent / name) for name in failure["audits"]]) if blocked else []
    return {
        "receipt": artifact(receipt_path), "coreCheckpoint": receipt["coreCheckpoint"],
        "coreVersion": checkpoint["version"], "frozenSourceCount": len(before),
        "frozenCmakeCount": len(cmake_before), "frozenSourceManifest": receipt["sourceManifest"],
        "frozenBeforeAfterArtifacts": [artifact(archive / name) for name in (
            "source-before.json", "source-after.json", "build-input-before.json", "build-input-after.json")],
        "liveSourceScope": "R11 may be under construction. Archived core receipt binds R10; "
                           "current live source is NOT attributed to this binary.",
        "actualGame": artifact(package / "main_game.exe"), "inventory": artifact(inventory_path),
        "files": receipt["files"], "physicalBytes": receipt["physicalBytes"],
        "installationVerify": {"exitCode": result.returncode, "validator": receipt["exporter"],
                               "log": artifact(output / "native-verify.log")},
        "readonlyOwnership": artifact(ownership), "sourceResourceRoot": receipt["sourceResourceRoot"],
        "readonlyScope": "Ownership validated CPU-only; WRITE-deny/probes/Game/ACL restore occur only "
                         "inside a later main-authorized execution. No readonly execution claimed.",
        "mode": mode, "scene": installed["initScene"], "sceneArtifact": artifact(scene_path),
        "sceneObjectClosure": [{"type": row["type"], "name": row.get("name")} for row in scene["objects"]],
        "sharedInputClosure": inputs, "publication": artifact(manifest_path),
        "surfaceIds": sorted(surfaces), "shaderCache": artifact(package / "shader/spv/shader-build-cache.json"),
        "sameAssetWorldReload": "loadworld maps to this installed scene; require actual new World generation "
                                "and old/new overlap. No different B scene exists in this package.",
        "formalExecutionBlocked": blocked, "knownFailureArtifacts": failure_artifacts,
        "knownFailureScope": "R10 Coverage worker2 center_0_2/center_1_2 native10s failures. "
                             "This is NOT a claim of an actual Local run failure. Both plans await a fixed/rebound core.",
        "fullP3Scope": "Original nine trace does not replace the53 visual route or these two failed centers",
        "newCorePolicy": "A different Game/core/kernel requires a fresh actual export/verify receipt and preparation"}


def camera_acknowledgements(directory, expected):
    text = "".join((directory / name).read_text(encoding="utf-8") for name in ("runtime.log", "stdout.log")
                   if (directory / name).is_file())
    # 实际引擎日志形如 "position=(x, y, z), forward=(x, y, z)"；旧正则假设无括号且用逗号分隔，
    # 导致 P0/当前构建的真实日志一条都匹配不到。这里同时接受带括号与旧的无括号形式。
    pattern = re.compile(
        r"Camera pose set: position=\(?\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)?"
        r",\s*forward=\(?\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)?")
    observed = [{"cameraPosition": [float(values[0]), float(values[1]), float(values[2])],
                 "cameraLookAt": [float(values[0]) + float(values[3]),
                                  float(values[1]) + float(values[4]),
                                  float(values[2]) + float(values[5])]}
                for values in pattern.findall(text)]
    from rvt_full_tour import camera_matches
    require(len(observed) == len(expected), "Actual camera acknowledgements do not cover entire fixed trace")
    for actual, command in zip(observed, expected):
        coordinates = [float(value) for value in command.split()[2:]]
        require(camera_matches({"position": coordinates[:3], "look_at": coordinates[3:]}, actual),
                "Actual camera differs from original trace (comparison is NOT a raw-frame offset mapping)")
    return {"count": len(observed), "matchesOriginalOrder": True,
            "scope": "Actual applied-camera states, not queued commands; raw frame alignment comes ONLY "
                     "from native frame/pose/epoch and capture metadata"}


def capture_facts(directory, plan, gpu):
    from rvt_full_tour import RuntimeEvidenceStream, CAPTURE_CLASSES, decode_capture, package_inputs
    from rvt_quality_binary import CAPACITY, SAMPLE_OFFSET, RESULT_OFFSET, SURFACE_OFFSET
    import struct
    _, surfaces, _ = package_inputs(Path(plan["package"]), plan["weightSource"])
    stream = RuntimeEvidenceStream(directory)
    stream.poll(final=True)
    keys = {(row.get("frame"), row.get("worldGeneration"), row.get("submitEpoch")) for row in gpu}
    captures = []
    for record in stream.records:
        if record["nativePrefix"] not in CAPTURE_CLASSES:
            continue
        decoded = decode_capture(record, stream, surfaces)
        meta, counts = decoded["metadata"], decoded["counts"]
        require((meta["frame"], meta["worldGeneration"], meta["submissionEpoch"]) in keys,
                "Quality metadata is not paired to an actual GPU frame identity")
        path = Path(decoded["samplesArtifact"]["path"])
        if not path.is_absolute():
            path = directory / path
        data = path.read_bytes()
        delta = []
        for pixel in range(CAPACITY):
            sample = struct.unpack_from("<6f2I", data, SAMPLE_OFFSET + pixel * 32)
            if not sample[7]:
                continue
            surface_index, taps = sample[6] & 65535, sample[6] >> 16
            maximum = struct.unpack_from("<I", data, SURFACE_OFFSET + surface_index * 64 + 12)[0] & 255
            requested_low = min(maximum, int(sample[4]) + int(sample[5]))
            for tap in range(taps):
                for branch in range(2):
                    packed = struct.unpack_from("<I", data, RESULT_OFFSET + (
                        pixel * 16 + tap * 2 + branch) * 4)[0]
                    delta.append((packed >> 10 & 15) - min(maximum, requested_low + branch))
        require(delta, "Missing fallback displacement samples; never substitute zero")
        ready = (decoded["complete"] and decoded["state"]["interactive"] is True and
                 counts["validAtTargetPlusBias"] / counts["visibleSamples"] >= 0.99 and
                 counts["unresolved"] == counts["incorrectIdentity"] == counts["rootOnlySamples"] == 0)
        captures.append({"metadata": meta, "counts": counts, "nativeState": decoded["state"],
                         "ready": ready, "samplesArtifact": decoded["samplesArtifact"],
                         "fallbackDisplacementMipLevels": distribution(delta),
                         "allocationQuery": meta.get("allocationQuery"),
                         "captureGpuAllocationBytes": meta.get("captureGpuAllocationBytes"),
                         "captureHostAllocationBytes": meta.get("captureHostAllocationBytes"),
                         "workspacePeakBytes": meta.get("workspacePeakBytes"),
                         "cpuPeakBytes": meta.get("cpuPeakBytes")})
    return captures


def normalized_accounting(events, captures, config, directory=None):
    observations, full, problems = [], [], []
    for capture in captures:
        query = capture.get("allocationQuery")
        if isinstance(query, dict):
            observations.append({"source": "actual quality WriteReadback.allocationQuery",
                                 "frame": capture["metadata"]["frame"], "query": query,
                                 "captureGpuAllocationBytes": capture["captureGpuAllocationBytes"],
                                 "captureHostAllocationBytes": capture["captureHostAllocationBytes"],
                                 "workspacePeakBytes": capture["workspacePeakBytes"],
                                 "cpuPeakBytes": capture["cpuPeakBytes"],
                                 "scope": "Actual quality suballocation; repeated captures are NOT summed "
                                          "as distinct allocations or whole-RVT high-water"})
    for envelope in events:
        payload = envelope["payload"]
        if "allocations" in payload and isinstance(payload["allocations"], list):
            try:
                full.append(ledger(payload, config))
            except ValueError as error:
                problems.append(str(error))
        if any(name in payload for name in ("byClass", "byPhase", "metadataBytes", "candidateOwnerGpuBytes",
                                           "rendererSlot", "budget", "classes")):
            observations.append({"eventClass": envelope["eventClass"], "payload": payload,
                                 "scope": "Native owner accounting facts; retain field units and lifetime. "
                                          "Not an inferred hash-deduplicated image inventory"})
    if directory:
        import struct
        observed_frames = {capture["metadata"]["frame"] for capture in captures}
        for path in sorted((directory / "quality").glob("capture-*.json")):
            metadata = read(path)
            if metadata.get("frame") in observed_frames:
                continue
            require(metadata.get("source") == "gpu-page-table-readback"
                    and metadata.get("moduleOnly") is False, "Allocation subset must be actual Game readback")
            binary = path.with_suffix(".bin")
            with binary.open("rb") as stream:
                header = stream.read(256)
            require(len(header) == 256, "Actual allocation readback header missing")
            words = struct.unpack("<64I", header)
            require(words[0] == 0x31515654 and words[1] in (1, 2), "Unknown allocation capture ABI")
            for field, offset in (("frame", 8), ("poseId", 10), ("worldGeneration", 12), ("submissionEpoch", 14)):
                require(str(words[offset] | words[offset + 1] << 32) == str(metadata[field]),
                        "Allocation metadata does not identify this actual raw capture")
            query = metadata.get("allocationQuery")
            require(isinstance(query, dict), "Actual quality allocation query absent")
            observations.append({"source": "actual bound binary/header + native allocationQuery",
                                 "frame": metadata["frame"], "query": query, "metadata": artifact(path),
                                 "binary": artifact(binary), "scope": "Quality subset allocation facts only; "
                                 "even a failed run retains these. No repeated-query sum or whole-owner claim"})
        for name in ("runtime.log", "stdout.log"):
            path = directory / name
            if not path.is_file():
                continue
            for index, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if "TerrainVT generation=" not in line:
                    continue
                values = {key: int(value) if value.isdigit() else value for key, value in
                          re.findall(r"(\w+)=([^\s]+)", line)}
                observations.append({"source": name, "line": index, "fields": values,
                    "scope": "Actual Service renderer/source/input/metadata scalar report. "
                             "Shared owner values on surface rows are NOT added sixteen times"})
    if not full:
        problems.append("Actual captured accounting does not yet provide whole-owner classified "
                        "geometry/input/physical/workspace/metadata with resource identity and lifetime. "
                        "Existing native allocationQuery is retained and not rejected by event name")
    return {"nativeObservations": observations, "classifiedSnapshots": full, "problems": problems}


def frame_window(gpu, cpu, start_identity, end_identity, minimum=1000):
    identity = lambda row: (row.get("renderFrame"), row.get("worldGeneration"), row.get("submitEpoch"))
    first, last = identity(start_identity), identity(end_identity)
    require(first in {identity(row) for row in gpu} and last in {identity(row) for row in gpu},
            "Window endpoints must be actual GPU identities, never readback counters")
    require(first[0] <= last[0], "Native frame window is reversed")
    selected_gpu = [row for row in gpu if first[0] <= row.get("renderFrame", -1) <= last[0]]
    selected_cpu = [row for row in cpu if type(row.get("renderFrame")) is int
                    and first[0] <= row["renderFrame"] <= last[0]]
    result = timing(selected_gpu, selected_cpu, minimum, (
        "geometry", "terrainFeedback", "terrainCompositedProductionAndCopy", "terrainQualityAfterFeedback"))
    result["endpoints"] = {"first": start_identity, "last": end_identity}
    result["outsideWindow"] = {"gpu": len(gpu) - len(selected_gpu), "cpu": len(cpu) - len(selected_cpu)}
    result["selection"] = "Prespecified native loading-release through last completed query. "
    result["selection"] += "All frames inside retained, no fastest/screenshot-centered window; "
    result["selection"] += "outside startup/teardown remain in whole-stream diagnostics."
    return result


def fixed_route_window(directory, plan, events, gpu, cpu):
    command = read(directory / "command.json")
    reference = Path(plan["cases"]["warm-nine-section"]["reference"]["path"])
    expected = route_argv(reference, Path(plan["package"]), directory, plan["packageBinding"]["scene"])
    require(command["argv"] == expected, "Actual launched schedule is not the fixed original nine trace")
    poses = [expected[index + 1] for index, value in enumerate(expected)
             if value == "--console-command" and expected[index + 1].startswith("camera pose ")]
    acknowledgements = camera_acknowledgements(directory, poses)
    releases = [row["payload"] for row in events if row["eventClass"] == "RVT_QUALITY_PRESENT"
                and row["payload"].get("event") == "loading_released" and row["payload"].get("ready") is True]
    require(releases and gpu, "No actual native initial loading release for fixed window")
    release = releases[0]
    first = {"renderFrame": release["frame"], "worldGeneration": release["worldGeneration"],
             "submitEpoch": release["submissionEpoch"]}
    last = {field: gpu[-1][field] for field in ("renderFrame", "worldGeneration", "submitEpoch")}
    result = frame_window(gpu, cpu, first, last)
    result["cameraAcknowledgements"] = acknowledgements
    result["routeReference"] = plan["cases"]["warm-nine-section"]["reference"]
    return result


def observe_process_log(path, output, stopped):
    started = time.monotonic_ns()
    position, partial = 0, b""
    with (output / "runner-observations.jsonl").open("w", encoding="utf-8") as destination:
        while True:
            now = time.monotonic_ns()
            if path.is_file():
                with path.open("rb") as source:
                    source.seek(position)
                    data = partial + source.read()
                    position = source.tell()
                lines = data.split(b"\n")
                partial = lines.pop()
                for encoded in lines:
                    line = encoded.decode("utf-8", errors="strict").rstrip("\r")
                    if any(marker in line for marker in (
                            "Camera pose set:", "Camera state:", "World/graph transaction committed:")):
                        destination.write(json.dumps({"observerMonotonicNs": now,
                            "observerElapsedSeconds": (now - started) / 1e9, "line": line,
                            "scope": "Actual process log observation; not a command execution frame "
                                     "and not aligned to GPU by line offsets"}) + "\n")
                        destination.flush()
            if stopped.is_set():
                break
            stopped.wait(0.25)


def trace_mapping(command, report):
    argv = read(command)["argv"]
    commands = [argv[index + 1] for index, argument in enumerate(argv) if argument == "--console-command"]
    sections = read(report)["sections"]
    require(tuple(section["name"] for section in sections) == SECTIONS, "Original nine-section labels differ")
    mapped, begin = [], 0
    for section in sections:
        end = next((index for index in range(begin, len(commands))
                    if commands[index].startswith("screenshot ") and
                    commands[index].replace("\\", "/").endswith("/" + section["name"] + ".bmp")), None)
        require(end is not None, f"Original screenshot delimiter missing: {section['name']}")
        segment = commands[begin:end + 1]
        cameras = [line for line in segment if line.startswith("camera pose ")]
        expected = ["camera pose " + " ".join(format(value, ".9g") for value in
                    pose["position"] + pose["look_at"]) for pose in section["poses"]]
        require(cameras == expected, f"Exact .9g camera sequence differs: {section['name']}")
        require(segment.count("camera get") == 72 and segment.count("terrainvt stats") == 4
                and segment.count("terrainchunk stats") == 1, "Original fixed section dwell differs")
        require(any(line.startswith("loadworld ") for line in segment) == section["reload"],
                "Section name reload must not imply actual reload")
        mapped.append({"name": section["name"], "firstCommandIndex": begin, "lastCommandIndex": end,
                       "cameraCommands": cameras, "cameraGetFrames": 72, "statsCommands": 5,
                       "referenceReloadActuallyRequested": section["reload"],
                       "frameMapping": "These are scheduled command indices, NOT native render frame or readiness IDs"})
        begin = end + 1
    require(commands[begin:] == ["camera get"] * 180 + ["terrainvt stats"] * 4 + ["terrainchunk stats"],
            "Original185-command tail differs")
    require(len(commands) == 922, "Original quiet-final schedule must contain922 commands")
    return {"sections": mapped, "tailCommands": len(commands) - begin, "totalCommands": len(commands),
            "completedTickTarget": int(argv[argv.index("--exit-after-frames") + 1]),
            "historicalRawGpuFrames": 1080, "historicalRawCpuFrames": 1081,
            "scope": "Entire unchanged scheduled trace; no artificial startup/frame pairing or warm-ready assertion."}


def prepare(options, config):
    root, output = options.source_root.resolve(strict=True), options.output.resolve()
    require(not output.exists(), "Preparation output must be fresh")
    p0 = Path(config["evidenceRoot"])
    package = options.package.resolve(strict=True) if options.package else None
    if package:
        require(not output.is_relative_to(package) and not package.is_relative_to(output),
                "Evidence must be outside install")
        installed = read(package / "config/config.json")
        require(installed.get("resourcePath") == "resources", "Game may use only package-local resource closure")
        require(installed.get("windowSize") == [1280, 720] and installed.get("msaa") == 2
                and installed.get("ui", {}).get("enabled") is False, "Installed conditions differ from P0")
        require(options.owned_package_receipt is not None, "Actual export receipt required")
    output.mkdir(parents=True)
    binding = bind_package(package, options.owned_package_receipt.resolve(strict=True),
                           options.weight_source, output) if package else None
    cases = {}
    for name, reference in (("warm-nine-section", "quiet-final"), ("cold-full-reload", "raw-cold"),
                            ("warm-full-reload", "raw-cold")):
        command = p0 / reference / "command.json"
        cases[name] = {"reference": artifact(command),
                       "argv": route_argv(command, package or Path("<MAIN_SUPPLIED_PACKAGE>"),
                                          output / name, binding["scene"] if binding else None),
                       "scope": "timing-denominator" if reference == "quiet-final" else "startup/memory/reload",
                       "referenceRun": reference,
                       "cacheScope": "Private fine-cache empty/retained must be independently observed, "
                                     "not inferred from cold/warm case names"}
    plan = {"schemaVersion": 2, "status": ("bound-known-failure-await-fixed-core" if binding and
            binding["formalExecutionBlocked"] else "prepared-unexecuted") if package else "awaiting-unified-package",
            "packageBinding": binding, "weightSource": options.weight_source,
            "sourceRoot": str(root), "sourceInventory": bound_inventory(
                root, ("source", "shader/glsl", "config", "CMakeLists.txt", "script/CMakeLists.txt")),
            "unattributedCurrentBinary": artifact(root / "build-vt-release/bin/main_game.exe")
            if (root / "build-vt-release/bin/main_game.exe").is_file() else None,
            "package": str(package) if package else None,
            "packageInventory": bound_inventory(package, ("",))
            if package else None,
            "ownedPackageReceipt": artifact(options.owned_package_receipt)
            if options.owned_package_receipt else None,
            "toolInventory": bound_inventory(root, ("tool/validation/terrain",)),
            "frozen": artifact(options.p0_freeze), "conditions": config["conditions"],
            "targets": config["targets"], "sections": SECTIONS, "cases": cases,
            "traceMapping": trace_mapping(p0 / "quiet-final/command.json", p0 / "quiet-final/report.json"),
            "hooks": HOOKS,
            "osCache": "OS file/standby cache uncontrolled, never purged",
            "coldDefinition": config["cacheConditions"]["requiredV2Cold"],
            "warmDefinition": config["cacheConditions"]["requiredV2Warm"],
            "privateCache": "Require native mode/root/payload/index observation; disabled is N/A, not cold/warm gain. "
                            "This tool never deletes caches or immutable package inputs.",
            "requiredReceiptFacts": ["same source/bin/kernel/newfeedbackUV/QualityTimer",
                                     "source inventory corresponds to exported binary (not inferred from mtime)",
                                     "TES enabled", "package-local shared input object closure",
                                     "actual private fine-cache mode/root and cold/warm state",
                                     "same-session hardware/driver and compiler audit",
                                     "observed original nine-section warmed window, >=1000 raw frames",
                                     "cross-World Candidate+Active+Retiring actual memory telemetry"],
            "visual53Scope": "Separate rvt_full_tour coverage; never substitutes fixed P0 timing denominator",
            "formalPerformancePassed": False, "gpuStarted": False}
    save(output / "plan.json", plan)
    (output / "handoff.txt").write_text(handoff_text(root, output, plan), encoding="utf-8")
    if binding:
        tool = root / "tool/validation/terrain/rvt_formal_perf.py"
        commands = (f'py -3 "{tool}" --execute --plan "{output / "plan.json"}" '
                    '--gpu-window "$Window" --case "$Case" --output "$RunOutput"')
        (output / "bound-execute.ps1").write_text(
            'param([Parameter(Mandatory=$true)][ValidateSet("warm-nine-section",'
            '"cold-full-reload","warm-full-reload")][string]$Case,\n'
            '[Parameter(Mandatory=$true)][string]$Window,\n'
            '[Parameter(Mandatory=$true)][string]$RunOutput)\n'
            "$ErrorActionPreference = 'Stop'\n"
            "# Define $Window and $RunOutput from main's explicit exclusive allocation.\n"
            "# Each case needs its own fresh external output and bound authorization.\n"
            "# Current R10 is known-failed; tool refuses launch. Rebind fixed core first.\n"
            + commands + "\n", encoding="utf-8")
    return {"status": plan["status"], "plan": artifact(output / "plan.json"),
            "handoff": artifact(output / "handoff.txt"),
            "actualSourceRoot": str(root), "actualBin": binding["actualGame"] if binding else plan["unattributedCurrentBinary"],
            "packageBinding": {name: binding[name] for name in (
                "coreCheckpoint", "frozenSourceCount", "frozenCmakeCount", "files", "physicalBytes",
                "formalExecutionBlocked")} if binding else None,
            "formalPerformancePassed": False, "gpuStarted": False}


def finalize_plan(options):
    require(options.plan and options.output, "Existing actual bound plan and fresh checkpoint file required")
    options.output = options.output.resolve()
    original = read(options.plan)
    binding = original.get("packageBinding")
    require(binding is not None, "Finalize only an actual package-bound plan, not an unbound template")
    require(not options.output.exists(), "Preserve original checkpoint")
    verify_bound_artifact(binding["actualGame"])
    verify_bound_artifact(binding["receipt"])
    verify_bound_artifact(binding["coreCheckpoint"])
    for entry in binding["frozenBeforeAfterArtifacts"]:
        verify_bound_artifact(entry)
    original["checkpointPredecessor"] = artifact(options.plan)
    original["toolInventory"] = bound_inventory(Path(original["sourceRoot"]), ("tool/validation/terrain",))
    original["privateCacheRoot"] = str(options.output.parent / "private-fine-cache")
    original["checkpointScope"] = "Tool consumer/handoff finalized against existing actual package binding. "
    original["checkpointScope"] += "No duplicate Prepare/Export/Verify, no GPU; original binding evidence retained."
    save(options.output, original)
    handoff = options.output.parent / "final-handoff.txt"
    handoff.write_text(handoff_text(Path(original["sourceRoot"]), options.output.parent,
                                    original, options.output.name), encoding="utf-8")
    return {"status": original["status"], "plan": artifact(options.output), "handoff": artifact(handoff),
            "actualBin": binding["actualGame"], "formalPerformancePassed": False, "gpuStarted": False}


def analyze(options, config):
    directory = options.analyze.resolve(strict=True)
    rows, events, problems, authority = load_native(directory)
    result = timing(rows["GPU_RAW"], rows["CPU_RAW"], config["targets"]["minimumRawFrameSamples"],
                    ("geometry", "terrainFeedback", "terrainCompositedProductionAndCopy",
                     "terrainQualityAfterFeedback"))
    problems.extend(result["problems"])
    facts = existing_native_facts(events)
    process_path = directory / "process.json"
    process = read(process_path) if process_path.is_file() else {}
    normal = process.get("exitCode") == 0 and process.get("timeout") is False
    if not normal:
        problems.append("Missing/unproved clean process exit")
    if process.get("gpuHelperExitCode") not in (0, 1) or process.get("gpuHelperError"):
        problems.append("Actual WDDM helper unavailable/abnormal; never assume requested500ms sampling")
    diagnostic_cpu = {
        "maxRawRenderFrameIntervalP95Ms": result["rawCpuIntervalMs"]["p95"],
        "maxRawRenderFrameIntervalP99Ms": result["rawCpuIntervalMs"]["p99"]}
    measurements = {}
    plan_path = options.plan or (directory / "plan.json")
    plan = read(plan_path) if plan_path.is_file() else None
    captures, window = [], None
    if plan and plan.get("packageBinding"):
        try:
            captures = capture_facts(directory, plan, rows["GPU_RAW"])
        except (ValueError, KeyError, OSError) as error:
            problems.append(f"Existing native/raw quality pairing: {error}")
        command_path = directory / "command.json"
        command_case = read(command_path).get("case") if command_path.is_file() else None
        if command_case in (None, "warm-nine-section"):
            try:
                window = fixed_route_window(directory, plan, events, rows["GPU_RAW"], rows["CPU_RAW"])
                if window["statisticsEligible"]:
                    measurements.update({
                        "maxSteadyGpuPassSumP99Ms": window["gpuPassSumMs"]["p99"],
                        "maxSteadyGeometryIncludingTesP99Ms": window["geometryIncludingTesMs"]["p99"],
                        "maxRawRenderFrameIntervalP95Ms": window["rawCpuIntervalMs"]["p95"],
                        "maxRawRenderFrameIntervalP99Ms": window["rawCpuIntervalMs"]["p99"]})
                else:
                    problems.extend(window["problems"])
            except (ValueError, KeyError, OSError) as error:
                problems.append(f"Prespecified original nine-trace window: {error}")
    else:
        problems.append("No actual package binding supplied for native capture/route replay")
    memory_results = {}
    for domain in ("cpu", "gpu"):
        path = directory / f"runtime.{domain}-memory.jsonl"
        if not path.is_file():
            path = directory / ("private.jsonl" if domain == "cpu" else "wddm.jsonl")
        if path.is_file():
            item = memory(jsonl(path), process.get("pid"), domain, config["targets"], normal)
            memory_results[domain] = item
            problems.extend(item["problems"])
            measurements["maxWholeProcessPrivatePeakBytes" if domain == "cpu"
                         else "maxWholeProcessWddmPeakBytes"] = item["peakBytes"]
        else:
            problems.append(f"Missing actual {domain} memory samples")
    ledgers, ledger_failures = [], []
    for row in events:
        if (row.get("eventClass") == "RVT_QUALITY_FAILURE"
                or row["payload"].get("convergenceFailed") is True):
            ledger_failures.append("Actual native quality/convergence failure")
    if any(process.get("detectedErrors", {}).values()):
        ledger_failures.append("Actual child runtime/validation error")
    accounting = normalized_accounting(events, captures, config, directory)
    problems.extend(accounting["problems"])
    ledgers = accounting["classifiedSnapshots"]
    for item in ledgers:
        ledger_failures.extend(item["failures"])
    if captures:
        for capture in captures:
            if capture["nativeState"].get("convergenceFailed") is True:
                ledger_failures.append("Actual native/raw paired convergence failed")
    else:
        problems.append("Actual bound quality/fallback tap samples absent; not filled with0")
    if facts["interactive"]:
        measurements["maxProcessToInteractiveSeconds"] = min(row["seconds"] for row in facts["interactive"])
    else:
        problems.append("No matched existing initial CLOCK + PRESENT.loading_released interactive milestone")
    failures = compare(measurements, config["targets"])
    # 各子分析器历史上返回字符串或结构体两种 problem；结构体不能直接进 set。
    # 统一序列化为稳定 JSON 文本，既不丢信息也不因 unhashable 崩溃。
    normalized_problems = sorted({
        item if isinstance(item, str) else json.dumps(item, ensure_ascii=False, sort_keys=True)
        for item in problems})
    return {"status": "failed" if failures or ledger_failures else "incomplete",
            "formalPerformancePassed": False, "failures": failures, "budgetFailures": ledger_failures,
            "incomplete": normalized_problems, "timing": result, "rawAuthority": authority,
            "memory": memory_results, "ledgers": ledgers, "existingNativeFacts": facts,
            "nativeAccounting": accounting, "fixedNineTraceWindow": window, "boundCaptures": captures,
            "acceptanceScope": "Metric evidence only. FullP3 includes all53 poses/the two failed centers "
                               "and independent TES/visual proof; nine trace alone cannot pass P3.",
            "wholeRunGpuDiagnostics": compare({
                "maxStreamingGpuPassSumP99Ms": result["gpuPassSumMs"]["p99"]}, config["targets"]),
            "wholeRunCpuDiagnostics": compare(diagnostic_cpu, config["targets"]),
            "rawInputs": [artifact(directory / name) for name in (
                "runtime.log", "stdout.log", "runtime-events.jsonl", "runtime.cpu-memory.jsonl",
                "runtime.gpu-memory.jsonl", "process.json") if (directory / name).is_file()],
            "noProductionSuite": "10936-page pressure scope already independent; not rerun here",
            "action": "Return any formal failure to main for source optimization; never modify expected/caps."}


def execute(options, config):
    require(options.gpu_window and options.plan and options.case,
            "Explicit main-issued GPU window, prepared bound plan and case required")
    window, plan = read(options.gpu_window), read(options.plan)
    binding = plan.get("packageBinding")
    require(binding is not None, "Actual coherent-core package binding required")
    require(binding.get("formalExecutionBlocked") is False,
            "R10 native center_0_2/center_1_2 failures remain. Re-export/rebind fixed core; no formal launch")
    require(window.get("authority") == "main" and window.get("exclusiveGpu") is True,
            "GPU window must explicitly authorize exclusive execution")
    require(window.get("coreCheckpointSha256") == binding["coreCheckpoint"]["sha256"]
            and window.get("ownedPackageReceiptSha256") == plan["ownedPackageReceipt"]["sha256"],
            "GPU window does not bind this actual coherent core and exported package")
    require(window.get("planSha256") == digest(options.plan) and window.get("case") == options.case,
            "GPU authorization does not bind this exact plan/case")
    require(window.get("notBeforeUtc") and window.get("expiresUtc"), "GPU window lacks actual interval")
    now = datetime.now().astimezone()
    require(datetime.fromisoformat(window["notBeforeUtc"]) <= now < datetime.fromisoformat(window["expiresUtc"]),
            "GPU window inactive/expired")
    require(plan.get("package") and plan.get("ownedPackageReceipt"), "Await actual unified package receipt")
    for collection in ("sourceInventory", "packageInventory", "toolInventory"):
        for entry in plan[collection]:
            require(artifact(entry["path"]) == {key: entry[key] for key in ("path", "bytes", "sha256")},
                    f"Source/package changed after preparation: {entry['path']}")
    require(artifact(plan["ownedPackageReceipt"]["path"]) == plan["ownedPackageReceipt"], "Export receipt changed")
    output = options.output.resolve()
    require(not output.exists(), "Execution output must be fresh")
    package = Path(plan["package"])
    require(not output.is_relative_to(package), "No diagnostic output inside install")
    output.mkdir(parents=True)
    (output / "quality").mkdir()
    (output / "present").mkdir()
    save(output / "plan.json", plan)
    # 统一以 plan 中绑定的 case.reference 为唯一路线来源（fixed_route_window 也读这里），
    # 避免 execute 自行按 evidenceRoot 重拼 P0 路径而在延长收尾帧时与窗口校验分叉。
    reference = Path(plan["cases"][options.case]["reference"]["path"])
    argv = route_argv(reference, package, output, binding["scene"])
    from rvt_run_conditions import process_conditions, quiet_host_audit
    from runtime_stress import sampled_process
    environment, conditions = process_conditions(package, output, read(package / "config/config.json"),
                                                 "formal-performance", 2, "capture-4hz")
    environment["VULKANLEARN_RUNTIME_EVIDENCE"] = str(output / "runtime-events.jsonl")
    environment["VULKANLEARN_RVT_QUALITY_EVIDENCE_DIR"] = str(output / "present")
    cache_root = Path(plan.get("privateCacheRoot", Path(options.plan).parent / "private-fine-cache"))
    require(not cache_root.resolve().is_relative_to(package), "Private cache must be outside readonly install")
    if options.case == "cold-full-reload":
        require(not cache_root.exists() or not any(path.is_file() for path in cache_root.rglob("*")),
                "Cold fine-cache is not empty; owner must prepare it explicitly. No automatic deletion")
    cache_root.mkdir(parents=True, exist_ok=True)
    environment["VULKANLEARN_RUNTIME_CACHE_ROOT"] = str(cache_root.resolve())
    conditions["environmentOverrides"]["VULKANLEARN_RUNTIME_EVIDENCE"] = environment["VULKANLEARN_RUNTIME_EVIDENCE"]
    save(output / "conditions.json", conditions)
    conditions["environmentOverrides"].update({name: environment[name] for name in (
        "VULKANLEARN_RVT_QUALITY_EVIDENCE_DIR", "VULKANLEARN_RUNTIME_CACHE_ROOT")})
    save(output / "conditions.json", conditions)
    save(output / "command.json", {"cwd": str(package), "argv": argv, "case": options.case,
          "commandSendScope": "Actual argv queue sent by process creation; camera execution is "
                              "separately observed and matched to native capture, not assumed from queue index",
          "mainGpuWindow": artifact(options.gpu_window)})
    stopped = threading.Event()
    watcher = threading.Thread(target=observe_process_log,
                               args=(output / "runtime.log", output, stopped), daemon=True)
    watcher.start()
    try:
        with ExitStack() as contexts:
            contexts.enter_context(quiet_host_audit(output / "host-audit.jsonl"))
            from rvt_readonly_install import readonly_package
            contexts.enter_context(readonly_package(package, Path(binding["readonlyOwnership"]["path"]),
                output, Path(plan["sourceRoot"]), Path(binding["sourceResourceRoot"])))
            process, _ = sampled_process(argv, package, output / "runtime.log", options.timeout, 500, env=environment)
    finally:
        stopped.set()
        watcher.join(timeout=5)
    save(output / "process.json", process)
    changed = []
    for collection in ("sourceInventory", "packageInventory", "toolInventory"):
        for entry in plan[collection]:
            if not Path(entry["path"]).is_file() or digest(entry["path"]) != entry["sha256"]:
                changed.append(entry["path"])
    save(output / "after-lock.json", {"changed": changed})
    options.analyze = output
    result = analyze(options, config)
    host = read(output / "host-audit.summary.json")
    if host["busy"] or host["errors"] or not host["complete"] or not host["samples"] or changed:
        result["incomplete"].append("Compiler audit or source/package before-after lock failed")
    save(output / "report.json", result)
    return result


class FormalTests(unittest.TestCase):
    def test_strict_complete_stdout(self):
        rows, errors = stdout_records('[GPU_RAW] {"frame":1} junk\n'
                                      '[CPU_RAW] {"renderFrameIntervalMs":1}\n'
                                      '[GPU_RAW] {"frame":2}')
        self.assertEqual(len(rows), 1)
        self.assertEqual(len(errors), 2)

    def test_interleaving_duplicate_and_nonfinite_rejected(self):
        for text in ('prefix [GPU_RAW] {"frame":1}\n',
                     '[GPU_RAW] {"frame":1,"frame":2}\n',
                     '[GPU_RAW] {"ms":NaN}\n', '[GPU_RAW] {"ms":1e400}\n'):
            self.assertTrue(stdout_records(text)[1])

    def test_historical_numbers_and_all_frozen_evidence(self):
        result = recompute_p0(frozen(ROOT / "config/terrainRvtP0Frozen.json"))
        self.assertEqual(result["verifiedEvidenceFiles"], 27)
        self.assertEqual(result["timing"]["gpuFrames"], 1080)
        self.assertEqual(result["timing"]["cpuFrames"], 1081)
        self.assertAlmostEqual(result["timing"]["gpuPassSumMs"]["p99"], 4.198024)
        self.assertAlmostEqual(result["timing"]["geometryIncludingTesMs"]["p99"], 3.41225472)
        self.assertAlmostEqual(result["timing"]["rawCpuIntervalMs"]["p95"], 44.5673)
        self.assertAlmostEqual(result["timing"]["rawCpuIntervalMs"]["p99"], 65.54924)
        self.assertEqual(result["memory"]["raw-cold"]["cpu"]["peakBytes"], 10243166208)
        self.assertEqual(result["memory"]["raw-warm-r2"]["gpu"]["peakBytes"], 8722153472)

    def test_raw_sum_not_sum_of_percentiles_and_null_not_dropped(self):
        gpu = [{"passes": [{"name": "geometry", "ms": 5}, {"name": "quality", "ms": 0}]},
               {"passes": [{"name": "geometry", "ms": 0}, {"name": "quality", "ms": 5}]}]
        cpu = [{"renderFrameIntervalMs": 1}] * 2
        result = timing(gpu, cpu, 2, ("quality", "geometry"), modern=False)
        self.assertEqual(result["gpuPassSumMs"]["p99"], 5)
        gpu[0]["passes"][1]["ms"] = None
        self.assertFalse(timing(gpu, cpu, 2, ("quality",), modern=False)["statisticsEligible"])

    def test_no_line_offset_pairing_or_fake_historical_identity(self):
        historical = [{"frame": 0, "passes": [{"name": "geometry", "ms": 1}]}]
        result = timing(historical, [{"renderFrameIntervalMs": 1}], 1, ("geometry",))
        self.assertTrue(result["problems"])
        self.assertFalse(result["statisticsEligible"])

    def test_quality_query_mandatory_every_frame(self):
        result = timing([{"passes": [{"name": "geometry", "ms": 1}]}],
                        [{"renderFrameIntervalMs": 1}], 1,
                        ("geometry", "terrainQualityAfterFeedback"), modern=False)
        self.assertFalse(result["statisticsEligible"])

    def test_actual_wddm_cadence_terminal_and_missing(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        rows = [{"pid": 1, "available": True, "dedicatedBytes": 100, "sharedBytes": 20,
                 "dedicatedInstances": 1, "sharedInstances": 1,
                 "utc": f"2026-11-21T00:00:{index:02d}+00:00"} for index in range(20)]
        rows.append({"pid": 1, "available": False})
        result = memory(rows, 1, "gpu", config["targets"], True)
        self.assertFalse(result["problems"])
        self.assertEqual(result["peakBytes"], 120)
        self.assertEqual(result["actualCadenceSeconds"]["p50"], 1)
        self.assertTrue(memory(rows, 1, "gpu", config["targets"], False)["problems"])
        rows[10]["available"] = False
        self.assertTrue(memory(rows, 1, "gpu", config["targets"], True)["problems"])
        rows[10]["available"] = True
        del rows[10]["sharedBytes"]
        self.assertTrue(memory(rows, 1, "gpu", config["targets"], True)["problems"])

    def test_gap_zero_and_wrong_pid_reject(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        for row in ({"pid": 2, "available": True}, {"pid": 1, "available": True,
                    "dedicatedBytes": 0, "sharedBytes": 0, "dedicatedInstances": 1, "sharedInstances": 1}):
            self.assertTrue(memory([row], 1, "gpu", config["targets"], True)["problems"])
        rows = [{"pid": 1, "available": True, "privateBytes": 5, "workingBytes": 5,
                 "elapsedMs": index * 3000} for index in range(20)]
        self.assertTrue(memory(rows, 1, "cpu", config["targets"], True)["problems"])

    def test_sink_requires_normal_terminal_and_sequence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            row = {"sequence": 1, "steady_ns": 1, "outputCategory": "explicitoutput",
                   "eventClass": "runtimeEvidenceEnd", "payload": {"incomplete": False, "recordsBeforeTerminal": 0}}
            path.write_text(json.dumps(row) + "\n", encoding="utf-8")
            self.assertEqual(len(dedicated_records(path)), 1)
            row["payload"]["incomplete"] = True
            path.write_text(json.dumps(row) + "\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                dedicated_records(path)

    def test_exact_camera_and_delay_preserved(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        reference = Path(config["evidenceRoot"]) / "quiet-final/command.json"
        original, actual = read(reference)["argv"], route_argv(reference, Path("package"), Path("external"))
        for start in ("camera pose ", "camera get", "terrainvt stats", "terrainchunk stats"):
            self.assertEqual([value for value in original if value.startswith(start)],
                             [value for value in actual if value.startswith(start)])
        self.assertEqual(actual[actual.index("--exit-after-frames") + 1], "1082")

    def test_nine_trace_every_pose_and_section_dwell(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        root = Path(config["evidenceRoot"]) / "quiet-final"
        mapping = trace_mapping(root / "command.json", root / "report.json")
        self.assertEqual([len(section["cameraCommands"]) for section in mapping["sections"]],
                         [1, 1, 1, 1, 1, 24, 4, 1, 1])
        self.assertEqual([section["lastCommandIndex"] - section["firstCommandIndex"] + 1
                          for section in mapping["sections"]], [79, 79, 79, 79, 79, 102, 82, 79, 79])
        self.assertEqual(mapping["tailCommands"], 185)
        self.assertFalse(mapping["sections"][7]["referenceReloadActuallyRequested"])

    def test_1000_native_frames_not_readback_counter(self):
        gpu, cpu = [], []
        for index in range(1000):
            identity = {"identityVersion": 2, "frame": 3000 + index, "renderFrame": 3000 + index,
                        "worldGeneration": 5, "submitEpoch": 3001 + index}
            gpu.append({**identity, "readbackSequence": index, "readbackMonotonicNs": 10000 + index,
                        "passes": [{"name": "geometry", "ms": 1},
                                   {"name": "terrainQualityAfterFeedback", "ms": 0.5}]})
            cpu.append({**identity, "sampleMonotonicNs": 1000 + index, "renderFrameIntervalMs": 33})
        self.assertTrue(timing(gpu, cpu, 1000, ("geometry", "terrainQualityAfterFeedback"))["statisticsEligible"])
        for mutate in ("drop", "counter", "world", "duplicate", "null", "clock"):
            broken = [dict(row) for row in gpu]
            if mutate == "drop":
                del broken[400]
            elif mutate == "counter":
                broken[400]["renderFrame"] = broken[400]["frame"] = broken[400]["readbackSequence"]
            elif mutate == "world":
                broken[400]["worldGeneration"] = 6
            elif mutate == "duplicate":
                broken[400] = dict(broken[399])
            elif mutate == "clock":
                del broken[400]["readbackMonotonicNs"]
            else:
                broken[400]["passes"] = [{"name": "geometry", "ms": 1},
                                         {"name": "terrainQualityAfterFeedback", "ms": None}]
            self.assertFalse(timing(broken, cpu, 1000, ("geometry", "terrainQualityAfterFeedback"))[
                "statisticsEligible"], mutate)

    def test_static_or_hash_only_memory_is_incomplete(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        with self.assertRaises(ValueError):
            ledger({"allocations": []}, config)
        with self.assertRaises(ValueError):
            ledger({"observedRuntime": True, "allocations": [{"memoryDomain": "gpu", "class": "source",
                    "kind": "image", "bytes": 100, "basis": "memoryRequirements", "imageIdentity": "sha256:abc",
                    "owners": [{"state": "Active", "worldGeneration": 1}]}]}, config)

    def test_ledger_counts_actual_distinct_images_even_same_content(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        allocations = []
        for category in ("geometry", "source", "physical", "feedback", "quality", "taskslots",
                         "driver-reserve"):
            allocations.append({"memoryDomain": "gpu", "class": category,
                                "kind": "image" if category == "source" else "buffer", "bytes": 100,
                                "basis": "explicit-reserve" if category == "driver-reserve"
                                else "memoryRequirements", "imageIdentity": "VkImage:actual-1",
                                "allocationIdentity": category,
                                "owners": [{"state": "Active", "worldGeneration": 1},
                                           {"state": "Retiring", "worldGeneration": 0}]})
        allocations.append({**allocations[1], "imageIdentity": "VkImage:actual-2"})
        allocations.append({"memoryDomain": "cpu", "class": "metadata", "kind": "container",
                            "bytes": 200, "basis": "container-capacity", "allocationIdentity": "owner-vector",
                            "owners": [{"state": "Candidate", "worldGeneration": 2}]})
        snapshot = {"observedRuntime": True, "allocations": allocations, "globalInFlightTasks": 4,
                    "physicalSlots": 512}
        result = ledger(snapshot, config)
        self.assertEqual(result["totals"]["gpu:input"], 200)
        self.assertEqual(result["totals"]["cpu:workspace"], 200)
        self.assertFalse(result["failures"])
        allocations.append(dict(allocations[1]))
        with self.assertRaises(ValueError):
            ledger(snapshot, config)

    def test_observed_peak_and_per_world_geometry_cap_reject(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        allocations = [{"memoryDomain": "gpu", "class": category, "kind": "buffer", "bytes": 100,
                        "basis": "memoryRequirements", "allocationIdentity": category,
                        "owners": [{"state": "Active", "worldGeneration": 1}]}
                       for category in ("geometry", "source", "physical", "feedback", "quality", "taskslots")]
        allocations.extend([
            {"memoryDomain": "gpu", "class": "driver-reserve", "bytes": 4194304,
             "basis": "explicit-reserve", "allocationIdentity": "driver",
             "owners": [{"state": "Active", "worldGeneration": 1}]},
            {"memoryDomain": "cpu", "class": "metadata", "bytes": 100, "basis": "container-capacity",
             "allocationIdentity": "host", "owners": [{"state": "Candidate", "worldGeneration": 2}]}])
        allocations[1]["kind"] = "image"
        allocations[1]["imageIdentity"] = "VkImage:source"
        allocations[0]["bytes"] = config["terrainTotalCaps"]["geometryGpuBytesPerWorld"] + 1
        result = ledger({"observedRuntime": True, "allocations": allocations,
                         "globalInFlightTasks": 5, "physicalSlots": 1025}, config)
        self.assertIn("Global task cap exceeded", result["failures"])
        self.assertIn("Global physical-slot cap exceeded", result["failures"])
        self.assertIn("World 1 gpu geometry cap exceeded", result["failures"])

    def test_caps_never_increased(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        expected = {"maxWholeProcessPrivatePeakBytes": 8194532966,
                    "maxWholeProcessWddmPeakBytes": 6977722777,
                    "maxWorldCommitSeconds": 76.748, "maxProcessToInteractiveSeconds": 86.748,
                    "maxSteadyGpuPassSumP99Ms": 4.618, "maxSteadyGeometryIncludingTesP99Ms": 3.754,
                    "maxStreamingGpuPassSumP99Ms": 10.199, "maxRawRenderFrameIntervalP95Ms": 49.025,
                    "maxRawRenderFrameIntervalP99Ms": 72.105}
        self.assertTrue(all(config["targets"][key] == value for key, value in expected.items()))
        self.assertEqual(len(compare({key: value + 1 for key, value in expected.items()}, config["targets"])), 9)

    def test_existing_native_names_need_no_new_semantic_api(self):
        events = [
            {"eventClass": "RVT_QUALITY_CLOCK", "payload": {"initialWorld": True,
             "worldGeneration": 2, "processStartedMonotonicMs": 1000}},
            {"eventClass": "RVT_QUALITY_PRESENT", "payload": {"event": "loading_released",
             "worldGeneration": 2, "ready": True, "monotonicMs": 86000, "frame": 12, "submissionEpoch": 13}},
            {"eventClass": "RVT_QUALITY_STATE", "payload": {"worldGeneration": 2, "poseId": 3,
             "interactive": True, "elapsedMs": 4000, "submissionEpoch": 13}},
            {"eventClass": "RVT_PRODUCTION_RAW", "payload": {
             "timestampValidBits": 64, "timestampPeriodNanoseconds": 1}}]
        facts = existing_native_facts(events)
        self.assertEqual(facts["newNativeEventNamesRequired"], [])
        self.assertEqual(facts["interactive"][0]["seconds"], 85)
        self.assertEqual(facts["poseConvergenceObservations"][0]["seconds"], 4)
        self.assertEqual(facts["timestampCalibrationObservations"], 1)

    def test_package_reload_changes_only_asset_path(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        reference = Path(config["evidenceRoot"]) / "raw-cold/command.json"
        expected = read(reference)["argv"]
        actual = route_argv(reference, Path("installed"), Path("external"), "Maps/new-scene.json")
        self.assertIn("loadworld Maps/new-scene.json", actual)
        self.assertEqual([row for row in actual if row.startswith("camera pose ")],
                         [row for row in expected if row.startswith("camera pose ")])
        self.assertEqual(actual[actual.index("--console-command-delay") + 1], "120")

    def test_window_1000_real_ids_rejects_interior_gap(self):
        gpu, cpu = [], []
        for index in range(1002):
            identity = {"identityVersion": 2, "frame": index + 2000, "renderFrame": index + 2000,
                        "worldGeneration": 1, "submitEpoch": index + 2001}
            gpu.append({**identity, "readbackSequence": index, "readbackMonotonicNs": index + 10000,
                        "passes": [{"name": name, "ms": 0.1} for name in (
                            "geometry", "terrainFeedback", "terrainCompositedProductionAndCopy",
                            "terrainQualityAfterFeedback")]})
            cpu.append({**identity, "sampleMonotonicNs": index + 10000, "renderFrameIntervalMs": 33})
        result = frame_window(gpu, cpu, gpu[1], gpu[-2])
        self.assertTrue(result["statisticsEligible"])
        self.assertEqual(result["gpuFrames"], 1000)
        self.assertEqual(result["outsideWindow"], {"gpu": 2, "cpu": 2})
        del gpu[450]
        self.assertFalse(frame_window(gpu, cpu, gpu[1], gpu[-2])["statisticsEligible"])

    def test_scalar_native_ledger_not_multiplied_per_surface(self):
        config = frozen(ROOT / "config/terrainRvtP0Frozen.json")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "runtime.log").write_text(
                "TerrainVT generation=1 sourceBytes=123 sourceTextures=7 productionSlots=4\n"
                "  terrain|a gpuBytes=999\n  terrain|b gpuBytes=999\n", encoding="utf-8")
            result = normalized_accounting([], [], config, directory)
            self.assertEqual(len(result["nativeObservations"]), 1)
            self.assertEqual(result["nativeObservations"][0]["fields"]["sourceBytes"], 123)
            self.assertTrue(result["problems"])

    def test_r10_failure_prevents_launch_before_any_process(self):
        from types import SimpleNamespace
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            plan, window = directory / "plan.json", directory / "window.json"
            save(plan, {"packageBinding": {"formalExecutionBlocked": True}})
            save(window, {"authority": "main"})
            with patch.object(subprocess, "Popen", side_effect=AssertionError("GPU/process forbidden")):
                with self.assertRaisesRegex(ValueError, "R10 native"):
                    execute(SimpleNamespace(plan=plan, gpu_window=window, case="warm-nine-section"), {})

    def test_actual_r10_failed_capture_allocation_subset_is_retained(self):
        directory = ROOT / "artifacts/terrain-rvt-production/p5/R10-fresh-center_0_2-w2"
        if not directory.is_dir():
            self.skipTest("Actual R10 failed evidence not on this machine")
        result = normalized_accounting([], [], frozen(ROOT / "config/terrainRvtP0Frozen.json"), directory)
        actual = [row for row in result["nativeObservations"] if "query" in row]
        self.assertGreaterEqual(len(actual), 20)
        self.assertTrue(all(row["query"]["outputRequirementBytes"] > 0 for row in actual))
        self.assertTrue(result["problems"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group(required=True)
    actions.add_argument("--self-test", action="store_true")
    actions.add_argument("--p0-recompute", action="store_true")
    actions.add_argument("--prepare-only", action="store_true")
    actions.add_argument("--finalize-plan", action="store_true")
    actions.add_argument("--analyze", type=Path)
    actions.add_argument("--execute", action="store_true")
    parser.add_argument("--p0-freeze", type=Path, default=ROOT / "config/terrainRvtP0Frozen.json")
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--owned-package-receipt", type=Path)
    parser.add_argument("--weight-source", choices=("CoverageAtlas", "LocalLayerArray"), default="CoverageAtlas")
    parser.add_argument("--plan", type=Path)
    parser.add_argument("--gpu-window", type=Path)
    parser.add_argument("--case", choices=("warm-nine-section", "cold-full-reload", "warm-full-reload"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=float, default=1800)
    options = parser.parse_args()
    if options.self_test:
        return 0 if unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(FormalTests)).wasSuccessful() else 2
    try:
        config = frozen(options.p0_freeze)
        if options.finalize_plan:
            result = finalize_plan(options)
        elif options.p0_recompute:
            result = recompute_p0(config)
        elif options.prepare_only:
            require(options.output is not None, "--output required")
            result = prepare(options, config)
        elif options.execute:
            require(options.output is not None and options.timeout > 0, "Fresh output and positive timeout required")
            result = execute(options, config)
        else:
            result = analyze(options, config)
        if options.output and not (options.prepare_only or options.execute or options.finalize_plan):
            require(not options.output.exists(), "Never overwrite original evidence")
            save(options.output, result)
        print(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False))
        return 2 if result["status"] == "failed" else 3 if result["status"] == "incomplete" else 0
    except (ValueError, KeyError, OSError, TypeError) as error:
        print(json.dumps({"status": "incomplete", "formalPerformancePassed": False, "reason": str(error)}))
        return 3


if __name__ == "__main__":
    sys.exit(main())

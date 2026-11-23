"""Exact child-process conditions and complete raw timing selection for P0 comparison."""

from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import threading
import time

from rvt_delivery import require
from rvt_p0_analyze import distribution, extract


def process_conditions(package, output, config, profile, worker, quality_mode):
    require(profile in ("correctness", "formal-performance"), "Unknown measurement profile")
    require(quality_mode in ("capture-4hz", "paired-steady"), "Unknown quality measurement mode")
    if profile == "formal-performance":
        require(worker == 2 and config.get("ui", {}).get("enabled") is False and
                config.get("windowSize") == [1280, 720] and config.get("msaa") == 2,
                "Formal P0 comparison requires worker2/UI off/1280x720/MSAA2")
        require(quality_mode == "capture-4hz",
                "Paired steady requires runtime phase capture control; not implemented, never disable all quality")
    output = output.resolve()
    require(not output.is_relative_to(package.resolve()), "Quality output must be outside readonly package")
    environment = os.environ.copy()
    removed = sorted(key for key in environment if key.startswith(("VL_", "VULKANLEARN_")))
    for key in removed:
        environment.pop(key)
    environment["VL_GPU_TIMING_RAW"] = "1"
    environment["VULKANLEARN_RVT_QUALITY_DIR"] = str(output / "quality")
    contract = {"profile": profile, "worker": worker, "validation": profile == "correctness",
                "frameLimitHz": 30, "presentMode": "immediate", "uiEnabled": config.get("ui", {}).get("enabled"),
                "resolution": config.get("windowSize"), "msaa": config.get("msaa"),
                "environmentOverrides": {key: environment[key] for key in
                    ("VL_GPU_TIMING_RAW", "VULKANLEARN_RVT_QUALITY_DIR")},
                "clearedInheritedVariableNames": removed, "qualityMode": quality_mode,
                "qualityScope": "Bounded native 128² feedback/all captured taps, <=4Hz, max256 captures; not fullscreen",
                "timingScope": "Raw CPU unclamped render intervals plus completed GPU timestamp passes; no rolling means",
                "osCache": "uncontrolled; no OS/standby cache purge claimed",
                "shaderCache": "warm immutable installed outputs required; any compile/miss invalidates formal run",
                "fineDiskCache": "native observed disabled/bounded mode; disabled is not warm disk-cache benefit",
                "instrumentation": "Quality timestamps/readback/IO remain enabled; compare instrumented figures explicitly; no overhead subtraction"}
    return environment, contract


@contextmanager
def quiet_host_audit(path):
    from rvt_p0_quiet import compiler_processes
    stop = threading.Event()
    started = time.monotonic()
    records = []

    def observe():
        with path.open("w", encoding="utf-8") as stream:
            while not stop.is_set():
                try:
                    row = {"elapsedSeconds": time.monotonic() - started,
                           "available": True, "compilerProcesses": compiler_processes()}
                except Exception as error:
                    row = {"elapsedSeconds": time.monotonic() - started,
                           "available": False, "error": str(error)}
                records.append(row)
                stream.write(json.dumps(row) + "\n")
                stream.flush()
                stop.wait(0.5)
    watcher = threading.Thread(target=observe, daemon=True)
    watcher.start()
    try:
        yield
    finally:
        stop.set()
        watcher.join(timeout=5)
        summary = {"samples": len(records), "busy": [row for row in records if row.get("compilerProcesses")],
                   "errors": [row for row in records if not row.get("available")],
                   "scope": "Named compiler processes sampled at500ms; short-lived work can be missed; not all background workload",
                   "complete": not watcher.is_alive()}
        path.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")


def analyze_complete_raw(text, events):
    gpu, gpu_errors = extract(text, "[GPU_RAW] ")
    cpu, cpu_errors = extract(text, "[CPU_RAW] ")
    production, production_errors = extract(text, "[BAKE_GPU_RAW] ")
    problems = []
    if gpu_errors or cpu_errors or production_errors:
        problems.append("Malformed raw timing records")
    frames = [row["frame"] for row in gpu]
    if not frames or frames != list(range(frames[0], frames[0] + len(frames))):
        problems.append("GPU frame IDs missing, duplicated or reordered")
    if len(gpu) < 1000 or len(cpu) < 1000:
        problems.append("Fewer than1000 complete raw CPU/GPU frames")
    phases = [row for row in events if row.get("event") == "measurement-phase"]
    quality = [row for row in events if row.get("event") == "quality" and row.get("phase") == "ready"]
    if not {"loading", "streaming", "warm-ready"} <= {row.get("phase") for row in phases} or not quality:
        problems.append("Missing observed loading/streaming/warm-ready phases or ready quality proof")
    values = []
    for row in gpu:
        passes = row["passes"]
        if not passes or any(type(item.get("ms")) not in (int, float) or
                             not math.isfinite(item["ms"]) or item["ms"] < 0 for item in passes):
            problems.append(f"Incomplete GPU query frame {row['frame']}")
            continue
        values.append(sum(item["ms"] for item in passes))
    intervals = [row["renderFrameIntervalMs"] for row in cpu]
    require(all(type(value) in (int, float) and math.isfinite(value) and value >= 0 for value in intervals),
            "Invalid unclamped CPU interval")
    return {"status": "incomplete" if problems else "raw-collected", "problems": problems,
            "gpuFrames": len(gpu), "cpuFrames": len(cpu), "productionRecords": len(production),
            "wholeRunInstrumentedGpuPassSumMs": distribution(values),
            "wholeRunRawRenderFrameIntervalMs": distribution(intervals),
            "phases": phases, "readyQualityObservations": len(quality),
            "selection": "Entire raw stream retained including slow/loading/capture frames; no screenshot-centered or fastest-window selection",
            "formalPerformancePassed": False,
            "productionScope": "10936 unique tile00 pages across2 modes/all mip0..13,heightBlend3 required in separate finite suite; route samples do not substitute"}


def finite_production_suite(records):
    expected = set()
    for mode in (0, 1):
        for mip in range(14):
            pages = (max(1, 8192 >> mip) + 127) // 128
            expected.update((mode, mip, x, y) for y in range(pages) for x in range(pages))
    observed = [(row["weightSource"], row["mip"], row["x"], row["y"]) for row in records]
    require(len(observed) == 10936 and len(set(observed)) == len(observed)
            and set(observed) == expected, "Finite production requires every unique page in both modes; no filtering")
    require(all(row["heightBlend"] == 3 for row in records), "Finite reference heightBlend must remain3")
    for row in records:
        for field in ("productionMs", "readbackCopyMs"):
            require(type(row[field]) in (int, float) and math.isfinite(row[field]) and row[field] >= 0,
                    "Incomplete production/copy timestamp sample")
    return {"samples": len(records), "perMode": 5468,
            "maximumProductionMs": max(row["productionMs"] for row in records),
            "maximumCopyMs": max(row["readbackCopyMs"] for row in records),
            "maximumSamePageProductionAndCopyMs": max(row["productionMs"] + row["readbackCopyMs"] for row in records),
            "scope": "Finite historical-reference load; actual V2 GPU-to-atlas copy must be identified separately"}

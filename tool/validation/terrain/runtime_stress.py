"""S3 bounded runtime stress through existing launch console -> CommandBus.

No renderer is started by --prepare-only, --analyze or --self-test.
Run requires the coordinator's exclusive renderer window. Example:
  py -3 tool/validation/terrain/runtime_stress.py --exe build-vt-release/bin/main_editor.exe ^
    --scene Generated/Validation/TerrainCompositedVT/Variants/Near100m_R1024_Default/SC_composited_vt_100m.json ^
    --resource-root ../VukanLearnResources --output artifacts/s3-stress-01 --low-budget-16

Default: one baseline run. --low-budget-16 adds an isolated 16-slot run.
Each private variant is Cooked before consumption because residentPages is in
the current dependency configuration hash. Original packages are never written.
Exit codes: 0 checks passed, 1 setup error, 2 failure, 3 missing evidence.
"""

import argparse
import ctypes
import json
import math
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import unittest

from editor_s4_prepare import DEFAULT_SCENE, PROJECT, prepare
from run_composited_acceptance import (
    bmp_integrity, camera_shots, digest, inside, read_json,
    stage_project, terrain_stats, write_json,
)


def gpu_script(pid, interval_ms):
    if not isinstance(pid, int) or pid <= 0 or not 250 <= interval_ms <= 5000:
        raise ValueError("Invalid target PID / sample interval")
    return r"""
$ErrorActionPreference = 'Stop'
$targetPid = PID_VALUE
while ($true) {
  try { $null = [System.Diagnostics.Process]::GetProcessById($targetPid) }
  catch { break }
  try {
    $samples = (Get-Counter -Counter '\GPU Process Memory(*)\Dedicated Usage','\GPU Process Memory(*)\Shared Usage').CounterSamples |
      Where-Object { $_.InstanceName -like ("pid_" + $targetPid + "_*") -and $_.Status -eq 0 }
    $dedicated = @($samples | Where-Object { $_.Path -like '*\Dedicated Usage' })
    $shared = @($samples | Where-Object { $_.Path -like '*\Shared Usage' })
    @{pid=$targetPid; utc=[DateTime]::UtcNow.ToString('o'); available=($dedicated.Count -gt 0 -and $shared.Count -gt 0);
      dedicatedBytes=($dedicated | Measure-Object CookedValue -Sum).Sum;
      sharedBytes=($shared | Measure-Object CookedValue -Sum).Sum;
      dedicatedInstances=$dedicated.Count; sharedInstances=$shared.Count} | ConvertTo-Json -Compress
  } catch {
    @{pid=$targetPid; utc=[DateTime]::UtcNow.ToString('o'); available=$false; error='Target PID WDDM counter unavailable'} | ConvertTo-Json -Compress
  }
  Start-Sleep -Milliseconds INTERVAL_VALUE
}
""".replace("PID_VALUE", str(pid)).replace("INTERVAL_VALUE", str(interval_ms))


def cpu_memory(pid):
    if sys.platform != "win32":
        return {"pid": pid, "available": False, "error": "Windows GetProcessMemoryInfo unavailable"}
    from ctypes import wintypes

    class MemoryCounters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    *[(name, ctypes.c_size_t) for name in (
                        "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                        "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                        "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x1000 | 0x10, False, pid)
    if not handle:
        return {"pid": pid, "available": False, "error": f"OpenProcess error {ctypes.get_last_error()}"}
    try:
        counters = MemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return {"pid": pid, "available": False, "error": f"GetProcessMemoryInfo error {ctypes.get_last_error()}"}
        return {"pid": pid, "available": True, "workingBytes": counters.WorkingSetSize,
                "privateBytes": counters.PrivateUsage, "peakWorkingBytes": counters.PeakWorkingSetSize,
                "peakPagefileBytes": counters.PeakPagefileUsage}
    finally:
        kernel.CloseHandle(handle)


def sampled_process(argv, cwd, log_path, timeout, sample_ms, env=None):
    if sys.platform == "win32" and len(subprocess.list2cmdline(argv)) >= 32767:
        raise ValueError("Windows command line too long; reduce settle-frames")
    clock = time.monotonic()
    gpu_path = log_path.with_suffix(".gpu-memory.jsonl")
    cpu_path = log_path.with_suffix(".cpu-memory.jsonl")
    helper, helper_error = None, None
    timed_out = False
    with log_path.open("wb") as log, gpu_path.open("wb") as gpu_log, cpu_path.open("w", encoding="utf-8") as cpu_log:
        process = subprocess.Popen(argv, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, shell=False, env=env)
        if sys.platform == "win32":
            try:
                helper = subprocess.Popen(
                    ["powershell", "-NoProfile", "-NonInteractive", "-Command", gpu_script(process.pid, sample_ms)],
                    stdout=gpu_log, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW)
            except OSError as error:
                helper_error = type(error).__name__
        else:
            helper_error = "WDDM counters unavailable on this platform"
        try:
            while process.poll() is None:
                row = cpu_memory(process.pid)
                row["elapsedMs"] = (time.monotonic() - clock) * 1000
                cpu_log.write(json.dumps(row) + "\n")
                cpu_log.flush()
                if time.monotonic() - clock >= timeout:
                    timed_out = True
                    break
                try:
                    process.wait(timeout=min(sample_ms / 1000, max(0.001, timeout - (time.monotonic() - clock))))
                except subprocess.TimeoutExpired:
                    pass
        finally:
            if process.poll() is None:
                if sys.platform == "win32":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30, check=False)
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=30)
            if helper is not None:
                try:
                    helper.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    helper.kill()
                    helper.wait(timeout=10)
    text = log_path.read_text(encoding="utf-8", errors="replace")
    errors = {
        "validation": re.findall(r"VUID-[^\s]+|Validation Error[^\n]*", text, re.I),
        "runtime": re.findall(r"ErrorDeviceLost|VK_ERROR_[A-Z_]+|EngineLoop init failed[^\n]*|"
                              r"Render thread failed[^\n]*|terminate called[^\n]*|"
                              r"Terrain composited service:[^\n]*|Unknown command[^\n]*", text, re.I)}
    return {"argv": argv, "cwd": str(cwd), "pid": process.pid, "exitCode": process.returncode,
            "timeout": timed_out, "seconds": time.monotonic() - clock, "detectedErrors": errors,
            "log": str(log_path), "logSha256": digest(log_path),
            "cpuMemory": str(cpu_path), "gpuMemory": str(gpu_path), "gpuHelperError": helper_error,
            "gpuHelperExitCode": helper.returncode if helper else None,
            "sampleIntervalMs": sample_ms}, text


def percentile(values, percent):
    ordered = sorted(values)
    if not ordered:
        return None
    index = (len(ordered) - 1) * percent / 100
    lower = math.floor(index)
    fraction = index - lower
    return ordered[lower] * (1 - fraction) + ordered[math.ceil(index)] * fraction


def memory_summary(cpu_rows, gpu_rows, pid):
    missing = []
    def valid(rows, names):
        return [row for row in rows if row.get("pid") == pid and row.get("available") is True
                and all(isinstance(row.get(name), (int, float)) and not isinstance(row[name], bool)
                        and math.isfinite(row[name]) and row[name] >= 0 for name in names)]
    cpu = valid(cpu_rows, ("workingBytes", "privateBytes"))
    gpu = valid(gpu_rows, ("dedicatedBytes", "sharedBytes"))
    if len(cpu) < 3:
        missing.append("Fewer than 3 valid target-PID CPU memory samples")
    if len(gpu) < 3:
        missing.append("Fewer than 3 valid target-PID WDDM Dedicated/Shared memory samples")
    return {
        "pid": pid, "cpuValidSamples": len(cpu), "gpuValidSamples": len(gpu),
        "cpuFirstElapsedMs": cpu[0].get("elapsedMs") if cpu else None,
        "cpuLastElapsedMs": cpu[-1].get("elapsedMs") if cpu else None,
        "gpuFirstUtc": gpu[0].get("utc") if gpu else None,
        "gpuLastUtc": gpu[-1].get("utc") if gpu else None,
        "peakWorkingBytes": max((row["workingBytes"] for row in cpu), default=None),
        "peakPrivateBytes": max((row["privateBytes"] for row in cpu), default=None),
        "osPeakWorkingBytes": max((row.get("peakWorkingBytes", row["workingBytes"]) for row in cpu), default=None),
        "peakDedicatedBytes": max((row["dedicatedBytes"] for row in gpu), default=None),
        "peakSharedBytes": max((row["sharedBytes"] for row in gpu), default=None),
        "peakSimultaneousDedicatedPlusSharedBytes": max(
            (row["dedicatedBytes"] + row["sharedBytes"] for row in gpu), default=None),
        "missingEvidence": missing,
        "scope": "Target PID only, includes source textures and driver allocations; sampled WDDM process allocation/residency approximation. "
                 "Dedicated+Shared at the same sample; not Vulkan-only payload, not device-total memory, not an exact transient high-water mark.",
    }


def pose(position, target):
    values = position + target
    if len(values) != 6 or not all(math.isfinite(value) for value in values):
        raise ValueError("Nonfinite/incomplete camera pose")
    if sum((a - b) ** 2 for a, b in zip(position, target)) < 1e-8:
        raise ValueError("Camera target equals position")
    return "camera pose " + " ".join(format(value, ".9g") for value in values)


def schedule(scene, shots, settle, reloads, tail_wait=0):
    commands, sections = [], []
    ordinal = 0
    by_name = {shot["name"]: shot for shot in shots}

    def section(name, poses, stable=True, reload=False):
        nonlocal ordinal
        start = ordinal
        if reload:
            commands.append("loadworld " + scene)
        for position, target in poses:
            commands.append(pose(position, target))
            commands.append("camera get")
            commands.append("terrainvt stats")
            ordinal += 1
        for _ in range(settle if stable else 0):
            commands.append("terrainvt stats")
            ordinal += 1
        tail_start = ordinal
        commands.extend(["terrainvt stats"] * tail_wait)
        ordinal += tail_wait
        commands.append("perf json")
        commands.append("screenshot s3_" + name + ".bmp")
        commands.extend(["terrainvt stats"] * 4)
        ordinal += 4
        sections.append({"name": name, "statsStart": start, "statsCount": ordinal - start,
                         "stable": stable, "reload": reload, "poseCount": len(poses),
                         "tailWaitStatsStart": tail_start, "tailWaitFrames": tail_wait,
                         "screenshot": "s3_" + name + ".bmp"})

    for name in ("near", "far", "mid"):
        shot = by_name[name]
        section(name, [(shot["position"], shot["look_at"])])
    near = by_name["near"]
    target = near["look_at"]
    radius = max(100.0, math.dist(near["position"], target))
    moves = []
    for index in range(24):
        angle = index * math.pi / 3
        position = [target[0] + radius * math.sin(angle), target[1] + radius * 0.55,
                    target[2] + radius * math.cos(angle)]
        aim = target if index % 2 == 0 or index == 23 else [
            target[0] + radius * 0.35 * math.cos(angle + 2), target[1],
            target[2] + radius * 0.35 * math.sin(angle + 2)]
        moves.append((position, aim))
    section("fast_turn_move", moves)
    far = by_name["far"]
    teleports = [(shot["position"], shot["look_at"])
                 for shot in (near, far, by_name["mid"], far, near, far, near)]
    section("teleport", teleports)
    for index in range(reloads):
        section(f"reload_{index + 1}", [(near["position"], near["look_at"])], reload=True)
    section("final_near", [(near["position"], near["look_at"])])
    commands.append("quit")
    estimated_argv = ["runtime.exe"]
    for command in commands:
        estimated_argv.extend(["--console-command", command])
    if sys.platform == "win32" and len(subprocess.list2cmdline(estimated_argv)) + 2048 > 32767:
        raise ValueError("Schedule exceeds safe Windows command-line size; reduce --settle-frames")
    return commands, sections


def summarize(text, perf_rows, sections, expected_surfaces, capacity, stall_ms,
              low_budget=False, page_limits=None):
    failures, missing = [], []
    page_limits = page_limits or {}
    blocks, problems = terrain_stats(text, expected_surfaces)
    failures.extend(problem for problem in problems if not problem.startswith("Final "))
    expected_blocks = sum(section["statsCount"] for section in sections)
    if len(blocks) != expected_blocks:
        failures.append(f"Statistics schedule mismatch: expected {expected_blocks}, got {len(blocks)}")
    errors = re.findall(
        r"VUID-[^\s]+|Validation Error[^\n]*|VK_ERROR_[A-Z_]+|ErrorDeviceLost|"
        r"EngineLoop init failed[^\n]*|Render thread failed[^\n]*|"
        r"Scene\.LoadFailed[^\n]*|World\.LoadFailed[^\n]*|"
        r"Terrain composited service:[^\n]*|terminate called[^\n]*|"
        r"Unknown command[^\n]*|Unknown option[^\n]*|Camera pose ignored[^\n]*", text, re.I)
    failures.extend(errors)
    camera_count = text.count("Camera pose set:")
    expected_poses = sum(section["poseCount"] for section in sections)
    if camera_count != expected_poses:
        failures.append(f"Actual camera command acknowledgements {camera_count} != {expected_poses}")
    if text.count("Camera state: position=") != expected_poses:
        failures.append("Camera state readback acknowledgements do not match the trajectory")
    summaries = {}
    generations = []
    for section in sections:
        history = blocks[section["statsStart"]:section["statsStart"] + section["statsCount"]]
        if len(history) != section["statsCount"]:
            missing.append(f"{section['name']}: incomplete snapshots")
            continue
        final = history[-1]
        generation = int(final["header"]["generation"])
        generations.append((section["name"], generation))
        for block in history:
            for surface in block["surfaces"]:
                resident, limit = map(int, surface["resident"].split("/"))
                if limit != capacity or not 1 <= resident <= capacity:
                    failures.append(f"{section['name']}: resident/capacity {resident}/{limit}, expected {capacity}")
                if any(int(surface.get(key, -1)) < 0 for key in ("queued", "reading", "stale", "dirty", "readFailures")):
                    failures.append(f"{section['name']}: missing/invalid counters")
                if int(surface.get("readFailures", -1)) != 0:
                    failures.append(f"{section['name']}: page read failures")
                if low_budget:
                    bound = page_limits.get(surface["surfaceId"])
                    if bound is None:
                        missing.append("Missing Cook page-count bound for low-budget pending requests")
                    elif int(surface["queued"]) + int(surface["reading"]) > bound:
                        failures.append(f"{section['name']}: pending+reading exceeds Cook logical page count {bound}")
        for surface in final["surfaces"]:
            for key in ("queued", "reading", "stale", "dirty"):
                if int(surface.get(key, -1)) != 0:
                    failures.append(f"{section['name']}: final {key}={surface.get(key)} did not converge")
            if int(surface.get("revision", -1)) != 0:
                failures.append(f"{section['name']}: unexpected authoring revision")
        tail_evidence = {}
        if low_budget:
            for surface in final["surfaces"]:
                identity = surface["surfaceId"]
                tail = [row for block in history[-min(20, len(history)):] for row in block["surfaces"]
                        if row["surfaceId"] == identity]
                if len(tail) < 2:
                    missing.append(f"{section['name']}: insufficient tail snapshots")
                    continue
                for key in ("feedbackEstimatedMisses", "feedbackRequests"):
                    if any(key not in row for row in tail):
                        missing.append(f"{section['name']}: missing low-budget {key}")
                misses = int(tail[-1].get("feedbackEstimatedMisses", -1)) - int(tail[0].get("feedbackEstimatedMisses", -1))
                requests = int(tail[-1].get("feedbackRequests", 0)) - int(tail[0].get("feedbackRequests", 0))
                if misses != 0:
                    failures.append(f"{section['name']}: new unresolved mapping misses in steady tail: {misses}")
                if requests <= 0:
                    missing.append(f"{section['name']}: no tail feedback activity; empty terrain cannot prove fallback")
                tail_evidence[identity] = {"estimatedMissDelta": misses, "feedbackRequestDelta": requests,
                                           "finalPending": int(surface["queued"]),
                                           "finalReading": int(surface["reading"]),
                                           "logicalPageBound": page_limits.get(identity)}
        if int(final["header"].get("previewReadbacks", -1)) != 0:
            failures.append(f"{section['name']}: unexpected pending preview readback")
        for key in ("pendingEditorUpdates", "pendingMaterialUpdates"):
            values = re.findall(rf"\b{key}=(\d+)", text)
            if not values or int(values[-1]) != 0:
                failures.append(f"Final {key} is missing or nonzero")
        summaries[section["name"]] = {
            "generation": generation, "final": final,
            "boundedDemandTail": tail_evidence,
            "peakQueued": max(int(row["queued"]) for block in history for row in block["surfaces"]),
            "peakReading": max(int(row["reading"]) for block in history for row in block["surfaces"]),
            "residentRange": [min(int(row["resident"].split("/")[0]) for block in history for row in block["surfaces"]),
                              max(int(row["resident"].split("/")[0]) for block in history for row in block["surfaces"])],
        }
        budget_counters = {}
        for identity in {row["surfaceId"] for row in final["surfaces"]}:
            tail = [row for block in history[-min(20, len(history)):] for row in block["surfaces"]
                    if row["surfaceId"] == identity]
            counter_names = ("budgetMipBias", "budgetSelectedPages", "readsStarted", "retiring")
            if any(name not in row for name in counter_names for row in tail):
                missing.append(f"{section['name']}: missing budgetMipBias/budgetSelectedPages/readsStarted/retiring")
                continue
            if any(int(row["budgetSelectedPages"]) > capacity for row in tail):
                failures.append(f"{section['name']}: budgetSelectedPages exceeds physical capacity")
            read_delta = int(tail[-1]["readsStarted"]) - int(tail[0]["readsStarted"])
            if read_delta != 0:
                failures.append(f"{section['name']}: steady tail starts new IO repeatedly: readsStarted delta={read_delta}")
            budget_counters[identity] = {
                **{name: int(tail[-1][name]) for name in counter_names},
                **{name: int(tail[-1][name]) for name in ("queuedCancel", "evictedReads")
                   if name in tail[-1]},
                "tailReadsStartedDelta": read_delta,
                "peakBudgetMipBias": max(int(row["budgetMipBias"]) for block in history
                                         for row in block["surfaces"] if row["surfaceId"] == identity),
            }
        summaries[section["name"]]["budgetCounters"] = budget_counters
    for (previous_name, previous), (name, current) in zip(generations, generations[1:]):
        if name.startswith("reload_"):
            if current <= previous:
                failures.append(f"{name}: World generation did not advance from {previous_name}")
        elif current != previous:
            failures.append(f"{name}: unexpected generation change")
    fallback = any(int(row.get("feedbackEstimatedParentFallbacks", "0")) > 0
                   for block in blocks for row in block["surfaces"])
    if not fallback:
        missing.append("No CPU-estimated parent fallback observed; low capacity alone is not fallback evidence")
    request_activity = any(int(row.get("feedbackRequests", "0")) > 0
                           for block in blocks for row in block["surfaces"])
    if not request_activity:
        missing.append("No feedback request/readback activity observed")
    peak_resident = max((int(row["resident"].split("/")[0])
                         for block in blocks for row in block["surfaces"]), default=0)
    demand_queued = max((int(row.get("queued", 0))
                        for block in blocks for row in block["surfaces"]), default=0)
    peak_bias = max((int(row.get("budgetMipBias", 0))
                     for block in blocks for row in block["surfaces"]), default=0)
    if low_budget and not (peak_bias > 0 or peak_resident == capacity and demand_queued > 0):
        missing.append("16-slot pressure unproven: require coarsening bias>0 or full residency with pending demand")
    cpu = []
    gpu = []
    for row in perf_rows:
        for name, output in (("cpuMs", cpu), ("gpuTotalMs", gpu)):
            value = row.get(name)
            if value is not None:
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                    failures.append(f"Invalid performance value: {name}")
                else:
                    output.append(value)
    if len(cpu) < 30:
        missing.append("Fewer than 30 valid CPU frame timings")
    if len(gpu) < 30:
        missing.append("Fewer than 30 valid GPU timer readbacks")
    stalls = [index for index, row in enumerate(perf_rows)
              if isinstance(row.get("cpuMs"), (int, float)) and row["cpuMs"] > stall_ms]
    if stalls:
        failures.append(f"{len(stalls)} CPU frame stalls exceed {stall_ms}ms (includes reload frames)")
    if gpu and max(gpu) > stall_ms:
        failures.append(f"GPU frame stall exceeds {stall_ms}ms")
    def metric(values):
        return {"count": len(values), "max": max(values), "median": statistics.median(values),
                "p50": percentile(values, 50), "p95": percentile(values, 95),
                "p99": percentile(values, 99)} if values else {}
    return {"failures": sorted(set(failures)), "missingEvidence": sorted(set(missing)),
            "sections": summaries, "statisticsCount": len(blocks), "cameraAcknowledgements": camera_count,
            "estimatedParentFallbackObserved": fallback, "feedbackObserved": request_activity,
            "cachePressure": {"peakResident": peak_resident, "peakQueued": demand_queued,
                              "capacityReached": peak_resident == capacity, "peakBudgetMipBias": peak_bias},
            "pendingPolicy": "all final queues converge to zero; budget coarsening may reduce selected detail",
            "performance": {"cpuMs": metric(cpu), "gpuMs": metric(gpu), "stallSampleIndices": stalls},
            "limits": ["Parent fallback counters are CPU mapping estimates, not GPU pixel proof.",
                       "Vulkan validation and bounded convergence do not prove every physical slot's retirement.",
                       "Frame timings include reload; threshold is explicit, no automatic exclusion of slow frames.",
                       "Perf rows lack command frame IDs; stage-specific frame attribution is not asserted."]}


def analyze(directory):
    report = read_json(directory / "run.json")
    for variant in report["variants"]:
        name = variant["name"]
        text_path = directory / (name + ".log")
        if not text_path.exists():
            variant["verdict"] = {"failures": [], "missingEvidence": ["Runtime was not executed"]}
            continue
        perf_path = directory / (name + ".perf.jsonl")
        rows = [json.loads(line) for line in perf_path.read_text(encoding="utf-8-sig").splitlines()
                if line.strip()] if perf_path.exists() else []
        result = summarize(text_path.read_text(encoding="utf-8", errors="replace"), rows,
                           report["schedule"]["sections"], report["expectedSurfaces"],
                           variant["capacity"], report["stallMs"],
                           low_budget=name == "budget16", page_limits=variant.get("logicalPageBounds"))
        for key in ("cook", "process"):
            process = variant.get(key, {})
            if process.get("exitCode") != 0 or process.get("timeout") or any(process.get("detectedErrors", {}).values()):
                result["failures"].append(f"{key}: process failed/timeout/errors")
            def memory_rows(name):
                path = Path(process[name]) if process.get(name) else None
                if path is None or not path.is_file():
                    return []
                return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines()
                        if line.strip()]
            memory = memory_summary(memory_rows("cpuMemory"), memory_rows("gpuMemory"), process.get("pid"))
            result.setdefault("processMemory", {})[key] = memory
            result["missingEvidence"].extend(f"{key}: {item}" for item in memory["missingEvidence"])
        if variant.get("sourceBefore") != variant.get("sourceAfter"):
            result["failures"].append("Original resource closure changed during the run")
        if variant.get("snapshotBeforeRuntime") != variant.get("snapshotAfterRuntime"):
            result["failures"].append("Runtime modified private author/package inputs")
        if variant.get("stagedShaderBefore") != variant.get("stagedShaderAfter"):
            result["failures"].append("Staged shader sources changed during runtime")
        for section in report["schedule"]["sections"]:
            screenshot = directory / f"{name}_{section['name']}.bmp"
            try:
                result.setdefault("captures", {})[section["name"]] = bmp_integrity(screenshot, None)
            except (OSError, ValueError) as error:
                result["missingEvidence"].append(f"{section['name']}: screenshot invalid/missing: {error}")
        captures = result.get("captures", {})
        if "near" in captures and "far" in captures and captures["near"]["sha256"] == captures["far"]["sha256"]:
            result["failures"].append("Near/far captures are identical despite distinct camera acknowledgements")
        result["performance"]["samplingScope"] = (
            "Engine perf JSONL is one snapshot per wall-clock second, not every frame. "
            "Percentiles describe sampled frame snapshots; unsampled transient stalls are not ruled out.")
        variant["verdict"] = result
    report["status"] = ("failed" if any(v["verdict"]["failures"] for v in report["variants"]) else
                        "incomplete" if any(v["verdict"]["missingEvidence"] for v in report["variants"]) else "passed")
    write_json(directory / "report.json", report)
    return {"passed": 0, "failed": 2, "incomplete": 3}[report["status"]]


def snapshot(root, paths):
    return {name: digest(inside(root, name)) if inside(root, name).is_file() else None for name in sorted(paths)}


def shader_snapshot(project):
    return {path.relative_to(project).as_posix(): digest(path)
            for path in sorted((project / "shader/glsl").rglob("*"))
            if path.is_file() and "generate" not in path.relative_to(project / "shader/glsl").parts}


def run(options):
    output = options.output.resolve()
    resource = options.resource_root.resolve(strict=True)
    if output.is_relative_to(resource) or resource.is_relative_to(output):
        raise ValueError("Output and original resources must be disjoint")
    executable = options.exe.resolve(strict=True)
    output.mkdir(parents=True, exist_ok=False)
    base = options.staging_root.resolve()
    base.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix="s3-", dir=base))
    shots, origin = camera_shots(inside(resource, options.scene), inside(resource, str(Path(options.scene).parent / "capture_plan.json")))
    commands, sections = schedule(options.scene, shots, options.settle_frames, options.reloads, options.tail_wait_frames)
    report = {"status": "prepared", "rendererStarted": False, "staging": str(staging),
              "exe": str(executable), "exeSha256": digest(executable), "scene": options.scene,
              "workerCount": 2, "cameraPlan": origin, "stallMs": options.stall_ms,
              "sampleIntervalMs": options.sample_ms, "frameLimitHz": options.frame_limit,
              "schedule": {"initialDelayFrames": options.initial_delay_frames, "commands": commands, "sections": sections},
              "variants": [], "sourceGuard": "SHA256 before/after; no original resource writes",
              "cookReason": "residentPages participates in current dependency config; private Cook required"}
    for label, requested in [("baseline", None)] + ([("budget16", 16)] if options.low_budget_16 else []):
        private = staging / label
        copied, project = private / "r", private / "p"
        closure = prepare(resource, options.scene, copied)
        capacities = set()
        document = read_json(inside(copied, options.scene))
        for obj in document["objects"]:
            if obj.get("type") != "terrain":
                continue
            path = inside(copied, obj["terrainPath"])
            terrain = read_json(path)
            if requested is not None:
                terrain["compositedVT"]["residentPages"] = requested
                write_json(path, terrain)
            capacities.add(terrain["compositedVT"]["residentPages"])
        if len(capacities) != 1:
            raise ValueError("Use a fixture with a uniform surface capacity")
        report["expectedSurfaces"] = sum(obj.get("type") == "terrain" for obj in document["objects"])
        project.mkdir()
        stage_project(PROJECT, project, copied)
        bin_path = project / "bin"
        bin_path.mkdir()
        for dll in executable.parent.glob("*.dll"):
            shutil.copy2(dll, bin_path / dll.name)
        staged_exe = bin_path / executable.name
        shutil.copy2(executable, staged_exe)
        variant = {"name": label, "capacity": capacities.pop(), "closure": closure,
                   "sourceBefore": snapshot(resource, closure["files"])}
        report["variants"].append(variant)
        write_json(output / "run.json", report)
        if options.prepare_only:
            continue
        common = [str(staged_exe), "--initial-scene", options.scene, "--no-dev-ui", "--validation",
                  "--worker-thread-count", "2", "--frame-limit", str(options.frame_limit)]
        report["rendererStarted"] = True
        report["status"] = "running"
        write_json(output / "run.json", report)
        variant["cook"], _ = sampled_process(
            common + ["--terrain-vt-cook", "--exit-after-frames", "2"],
            project, output / (label + ".cook.log"), options.timeout, options.sample_ms)
        write_json(output / "run.json", report)
        if variant["cook"]["exitCode"] or variant["cook"]["timeout"] or any(variant["cook"]["detectedErrors"].values()):
            report["status"] = "cook-failed"
            write_json(output / "run.json", report)
            return 2
        variant["logicalPageBounds"] = {}
        variant["cookedSurfaces"] = {}
        for obj in document["objects"]:
            if obj.get("type") != "terrain":
                continue
            terrain = read_json(inside(copied, obj["terrainPath"]))
            manifest_path = inside(copied, terrain["compositedVT"]["packagePath"])
            manifest = read_json(manifest_path)
            identity = "terrain|" + obj["name"]
            variant["logicalPageBounds"][identity] = sum(
                page["surfaceId"] == identity for page in manifest["pages"])
            variant["cookedSurfaces"][identity] = manifest["surfaces"]
        tracked = set(closure["files"])
        tracked.update(path.relative_to(copied).as_posix() for path in copied.rglob("*")
                       if path.is_file() and "Cook" in path.parts)
        variant["snapshotBeforeRuntime"] = snapshot(copied, tracked)
        variant["stagedShaderBefore"] = shader_snapshot(project)
        argv = common + ["--perf-jsonl", str(output / (label + ".perf.jsonl")),
                         "--console-command-delay", str(options.initial_delay_frames),
                         "--exit-after-frames", str(len(commands) + options.initial_delay_frames + 60)]
        for command in commands:
            argv.extend(["--console-command", command])
        variant["process"], _ = sampled_process(argv, project, output / (label + ".log"),
                                               options.timeout, options.sample_ms)
        variant["sourceAfter"] = snapshot(resource, closure["files"])
        variant["snapshotAfterRuntime"] = snapshot(copied, tracked)
        variant["stagedShaderAfter"] = shader_snapshot(project)
        for section in sections:
            image = copied / "Generated/Screenshots" / section["screenshot"]
            if image.is_file():
                shutil.copy2(image, output / f"{label}_{section['name']}.bmp")
        write_json(output / "run.json", report)
    return 0 if options.prepare_only else analyze(output)


def self_test():
    class CheckerTests(unittest.TestCase):
        def fixture(self):
            shots = [{"name": name, "position": [0, height, height], "look_at": [0, 0, 0]}
                     for name, height in (("near", 100), ("far", 10000), ("mid", 1000))]
            commands, sections = schedule("fixture.json", shots, 2, 2)
            text = []
            generation = 1
            for section in sections:
                generation += int(section["reload"])
                text.extend(["Camera pose set: position=1", "Camera state: position=1"] * section["poseCount"])
                for _ in range(section["statsCount"]):
                    pending = 1 if _ == 0 else 0
                    text.extend([f"TerrainVT generation={generation} surfaces=1 previewReadbacks=0",
                                 f"terrain|fixture generation=1 revision=0 resident=16/16 queued={pending} reading=0 "
                                 f"dirty=0 stale=0 readFailures=0 budgetMipBias=1 budgetSelectedPages=12 readsStarted=30 retiring=1 feedbackRequests={50 + _} feedbackEstimatedMisses=0 "
                                 "feedbackEstimatedParentFallbacks=3 pendingEditorUpdates=0 pendingMaterialUpdates=0"])
            perf = [{"cpuMs": 10, "gpuTotalMs": 5}] * 40
            return commands, sections, "\n".join(text), perf

        def verdict(self, transform=lambda value: value, perf=None, low=False):
            _, sections, text, rows = self.fixture()
            return summarize(transform(text), rows if perf is None else perf, sections, 1, 16, 250,
                             low_budget=low, page_limits={"terrain|fixture": 92})

        def test_clean_evidence(self):
            result = self.verdict()
            self.assertEqual((result["failures"], result["missingEvidence"]), ([], []))

        def test_queue_cannot_pass(self):
            self.assertTrue(self.verdict(lambda text: text.replace("queued=0", "queued=1"))["failures"])

        def test_low_budget_without_pressure_stays_incomplete(self):
            self.assertTrue(self.verdict(lambda text: text.replace("16/16", "13/16").replace("budgetMipBias=1", "budgetMipBias=0"), low=True)["missingEvidence"])

        def test_low_budget_pending_must_still_converge(self):
            result = self.verdict(lambda text: text.replace("queued=0", "queued=10"), low=True)
            self.assertTrue(result["failures"])

        def test_low_budget_bias_proves_pressure_without_all_slots_resident(self):
            result = self.verdict(lambda text: text.replace("16/16", "15/16"), low=True)
            self.assertEqual((result["failures"], result["missingEvidence"]), ([], []))

        def test_steady_io_loop_fails(self):
            index = 0
            def reads(text):
                def replace(match):
                    nonlocal index
                    index += 1
                    return "readsStarted=" + str(index)
                return re.sub(r"readsStarted=30", replace, text)
            self.assertTrue(self.verdict(reads)["failures"])

        def test_low_budget_rejects_unbounded_pending(self):
            self.assertTrue(self.verdict(lambda text: text.replace("queued=0", "queued=100"), low=True)["failures"])

        def test_low_budget_empty_feedback_stays_incomplete(self):
            self.assertTrue(self.verdict(
                lambda text: re.sub(r"feedbackRequests=\d+", "feedbackRequests=0", text), low=True)["missingEvidence"])

        def test_low_budget_new_tail_misses_fail(self):
            def misses(text):
                index = 0
                def replace(match):
                    nonlocal index
                    index += 1
                    return "feedbackEstimatedMisses=" + str(index)
                return re.sub(r"feedbackEstimatedMisses=0", replace, text)
            self.assertTrue(self.verdict(misses, low=True)["failures"])

        def test_read_failure_cannot_pass(self):
            self.assertTrue(self.verdict(lambda text: text.replace("readFailures=0", "readFailures=1"))["failures"])

        def test_wrong_budget_cannot_pass(self):
            self.assertTrue(self.verdict(lambda text: text.replace("16/16", "16/64"))["failures"])

        def test_stall_cannot_pass(self):
            self.assertTrue(self.verdict(perf=[{"cpuMs": 900, "gpuTotalMs": 5}] * 40)["failures"])

        def test_reload_without_generation_cannot_pass(self):
            self.assertTrue(self.verdict(lambda text: re.sub(r"TerrainVT generation=\d+", "TerrainVT generation=1", text))["failures"])

        def test_validation_cannot_pass(self):
            self.assertTrue(self.verdict(lambda text: text + "\nValidation Error VUID-negative")["failures"])

        def test_fallback_missing_is_not_pass(self):
            self.assertTrue(self.verdict(lambda text: text.replace("ParentFallbacks=3", "ParentFallbacks=0"))["missingEvidence"])

        def test_missing_perf_is_not_pass(self):
            self.assertTrue(self.verdict(perf=[])["missingEvidence"])

        def test_camera_without_acknowledgement_fails(self):
            self.assertTrue(self.verdict(lambda text: text.replace("Camera pose set:", "Ignored:"))["failures"])

        def test_schedule_includes_existing_commands_only(self):
            commands, sections, _, _ = self.fixture()
            self.assertEqual(sum(section["reload"] for section in sections), 2)
            self.assertTrue(all(command.split()[0] in ("camera", "terrainvt", "loadworld", "perf", "screenshot", "quit")
                                for command in commands))

        def test_percentile_interpolation(self):
            self.assertEqual(percentile([0, 10, 20, 30, 40], 50), 20)
            self.assertEqual(percentile([0, 100], 95), 95)

        def test_memory_never_accepts_other_pids(self):
            other = [{"pid": 999, "available": True, "dedicatedBytes": 123, "sharedBytes": 456}] * 3
            result = memory_summary([], other, 123)
            self.assertIsNone(result["peakDedicatedBytes"])
            self.assertTrue(result["missingEvidence"])

        def test_memory_unavailable_is_missing_not_device_total(self):
            self.assertTrue(memory_summary([], [{"pid": 123, "available": False}], 123)["missingEvidence"])

        def test_memory_peak_is_simultaneous_sum(self):
            gpu = [{"pid": 123, "available": True, "dedicatedBytes": d, "sharedBytes": s}
                   for d, s in ((100, 5), (5, 100), (30, 20))]
            cpu = [{"pid": 123, "available": True, "workingBytes": 100, "privateBytes": 50}] * 3
            result = memory_summary(cpu, gpu, 123)
            self.assertEqual(result["peakSimultaneousDedicatedPlusSharedBytes"], 105)
            self.assertEqual(result["missingEvidence"], [])

        def test_gpu_script_filters_before_serialization(self):
            script = gpu_script(123, 1000)
            self.assertIn('$targetPid = 123', script)
            self.assertLess(script.index("Where-Object"), script.index("ConvertTo-Json"))
            self.assertNotIn("InstanceName=", script)
            with self.assertRaises(ValueError):
                gpu_script("123;exit", 1000)

        @unittest.skipUnless(sys.platform == "win32", "Windows memory API")
        def test_cpu_reader_current_checker_pid(self):
            import os
            row = cpu_memory(os.getpid())
            self.assertTrue(row["available"])
            self.assertGreater(row["workingBytes"], 0)
            self.assertGreater(row["privateBytes"], 0)

    return 0 if unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(CheckerTests)).wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--scene", default=DEFAULT_SCENE)
    parser.add_argument("--resource-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--low-budget-16", action="store_true")
    parser.add_argument("--staging-root", type=Path, default=Path(tempfile.gettempdir()) / "vls3")
    parser.add_argument("--settle-frames", type=int, default=60)
    parser.add_argument("--tail-wait-frames", type=int, default=0,
                        help="Extra unchanged-camera ticks with VT stats before each capture")
    parser.add_argument("--initial-delay-frames", type=int, default=600,
                        help="Initial baseline observation; default 20s at 30 Hz for >=30 1Hz perf snapshots")
    parser.add_argument("--reloads", type=int, default=2)
    parser.add_argument("--stall-ms", type=float, default=500)
    parser.add_argument("--frame-limit", type=int, choices=range(1, 1001), default=30,
                        metavar="1..1000", help="Runtime pacing cap; 30 Hz lengthens OS memory observation")
    parser.add_argument("--sample-ms", type=int, choices=range(250, 5001), default=1000,
                        metavar="250..5000", help="Target PID CPU/WDDM sampling cadence (Get-Counter adds latency)")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--analyze", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        return self_test()
    if options.output is None:
        parser.error("--output is required")
    if options.analyze:
        return analyze(options.output.resolve())
    if options.exe is None or options.resource_root is None:
        parser.error("--exe and --resource-root are required")
    if not 10 <= options.settle_frames <= 120 or not 1 <= options.reloads <= 3:
        parser.error("settle-frames must be 10..120 and reloads 1..3 (bounded Windows command line)")
    if not 1 <= options.initial_delay_frames <= 3600:
        parser.error("initial-delay-frames must be 1..3600")
    if not 0 <= options.tail_wait_frames <= 120:
        parser.error("tail-wait-frames must be 0..120")
    if not math.isfinite(options.stall_ms) or options.stall_ms <= 0 or not math.isfinite(options.timeout) or options.timeout <= 0:
        parser.error("stall-ms and timeout must be finite and positive")
    return run(options)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)

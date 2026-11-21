"""Collect Full16 poses/movement/reload evidence; final acceptance is separate."""

import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import threading
import time

from PIL import Image
from runtime_stress import memory_summary, percentile, sampled_process
from run_composited_acceptance import terrain_stats


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--worker", type=int, choices=(1, 2), required=True)
    parser.add_argument("--weight-source", choices=("CoverageAtlas", "LocalLayerArray"), required=True)
    parser.add_argument("--camera-group", choices=("core", "coverage"), default="core")
    parser.add_argument("--camera-offset", type=int, default=0)
    parser.add_argument("--camera-count", type=int, default=6)
    parser.add_argument("--no-validation", action="store_true")
    parser.add_argument("--first-frame-evidence", action="store_true")
    parser.add_argument("--no-world-reload", action="store_true")
    options = parser.parse_args()
    stage, output = options.stage.resolve(), options.output.resolve()
    config = read(stage / "config/config.json")
    resource = (stage / config["resourcePath"]).resolve(strict=True)
    scene = config["initScene"]
    plan = read(resource / Path(scene).parent / "capture_plan.json")
    shots = {shot["name"]: shot for shot in plan["shots"]}
    commands, sections = [], []

    def pose(shot):
        return "camera pose " + " ".join(f"{value:.9g}" for value in shot["position"] + shot["look_at"])

    def section(name, poses, reload=False):
        nonlocal commands
        if reload:
            commands.append("loadworld " + scene)
        commands.extend(pose(shot) for shot in poses)
        commands += ["camera get"] * 72
        commands += ["terrainvt stats"] * 4 + ["terrainchunk stats"]
        commands.append(f"screenshot {(output / (name + '.bmp')).as_posix()}")
        sections.append({"name": name, "poses": poses, "reload": reload})

    if options.camera_group == "core":
        for name in ("far", "mid", "near", "tile00_near", "cross_tile_corner"):
            section(name, [shots[name]])
    else:
        coverage = [shot for shot in plan["shots"]
                    if shot.get("category") in ("center", "edge", "junction", "outer")]
        if len(coverage) != 53 or options.camera_offset < 0 or options.camera_count < 1:
            raise ValueError("Full16 coverage requires 53 shots and a valid batch range")
        selected = coverage[options.camera_offset:options.camera_offset + options.camera_count]
        if not selected:
            raise ValueError("Empty coverage batch")
        for shot in selected:
            section(shot["name"], [shot])
    movement = []
    for step in range(1, 25):
        alpha = step / 24
        movement.append({
            field: [a + (b - a) * alpha for a, b in zip(shots["tile00_near"][field], shots["near"][field])]
            for field in ("position", "look_at")
        })
    if options.camera_group == "core":
        section("move_across_island", movement)
        section("teleport", [shots[name] for name in ("far", "near", "tile00_near", "cross_tile_corner")])
        section("reload", [shots["near"]], reload=not options.no_world_reload)
        section("final_far", [shots["far"]])
    commands += ["camera get"] * (60 if options.first_frame_evidence else 180) + ["terrainvt stats"] * 4 + ["terrainchunk stats"]
    initial_blocks = 0
    if options.first_frame_evidence:
        commands = [f"screenshot {(output / 'first-frame.bmp').as_posix()}",
                    "terrainvt stats"] + ["camera get"] * 120 + commands
        initial_blocks = 1
    command = [
        str(stage / ("bin/main_game.exe" if (stage / "bin/main_game.exe").is_file() else "main_game.exe")),
        "--worker-thread-count", str(options.worker), "--frame-limit", "30",
        "--exit-after-frames", str(160 + len(commands)), "--no-dev-ui",
        "--no-validation" if options.no_validation else "--validation",
        "--present-mode", "immediate",
        "--perf-jsonl", str(output / "runtime.perf.jsonl"), "--console-command-delay",
        "1" if options.first_frame_evidence else "120",
    ]
    for line in commands:
        command += ["--console-command", line]
    if len(subprocess.list2cmdline(command)) >= 32767:
        raise ValueError("Full runtime schedule exceeds Windows command-line bound")
    if not options.execute:
        print(json.dumps({"cwd": str(stage), "commandLength": len(subprocess.list2cmdline(command)),
                          "sections": sections, "completedFrameTarget": 160 + len(commands),
                          "rendererStarted": False}, indent=2))
        return
    output.mkdir(parents=True, exist_ok=False)
    (output / "command.json").write_text(json.dumps({"cwd": str(stage), "argv": command}, indent=2), encoding="utf-8")
    started = time.monotonic()
    stop = threading.Event()
    milestones = []

    def observe():
        offset, remaining = 0, ""
        while not stop.wait(0.25):
            path = output / "runtime.log"
            if not path.is_file():
                continue
            with path.open("r", encoding="utf-8", errors="replace") as stream:
                stream.seek(offset)
                remaining += stream.read()
                offset = stream.tell()
            complete = remaining.split("\n")
            remaining = complete.pop()
            for line in complete:
                if "World/graph transaction committed:" in line or "Screenshot saved:" in line:
                    record = {"elapsedSeconds": time.monotonic() - started, "line": line}
                    milestones.append(record)
                    with (output / "milestones.jsonl").open("a", encoding="utf-8") as stream:
                        stream.write(json.dumps(record) + "\n")

    watcher = threading.Thread(target=observe, daemon=True)
    watcher.start()
    print(f"Launching Full16 w{options.worker}; PID in {output / 'runtime.cpu-memory.jsonl'}", flush=True)
    try:
        process, text = sampled_process(command, stage, output / "runtime.log", 2400, 500)
    finally:
        stop.set()
        watcher.join(timeout=3)
    cpu_rows = rows(Path(process["cpuMemory"]))
    gpu_rows = rows(Path(process["gpuMemory"]))
    memory = memory_summary(cpu_rows, gpu_rows, process["pid"])
    perf = rows(output / "runtime.perf.jsonl")
    blocks, problems = terrain_stats(text, 16)
    expected_blocks = len(sections) * 4 + 4 + initial_blocks
    checks = {
        "exit0": process["exitCode"] == 0, "noTimeout": not process["timeout"],
        "noErrors": not any(process["detectedErrors"].values()),
        "cleanTeardown": "EngineLoop teardown complete" in text,
        "statisticsCount": len(blocks) == expected_blocks,
        "CPU20": memory["cpuValidSamples"] >= 20, "GPU20": memory["gpuValidSamples"] >= 20,
        "timings20": len(perf) >= 20,
        "noShaderCompile": not re.search(r"Shader cache miss:|compiled=[1-9]|shaderc=[1-9]", text),
    }
    for index, item in enumerate(sections):
        history = blocks[initial_blocks + index * 4:initial_blocks + (index + 1) * 4]
        final = history[-1] if history else None
        item["finalStats"] = final
        item["settled"] = bool(final) and all(
            int(surface.get(field, -1)) == 0
            for surface in final["surfaces"] for field in ("queued", "reading", "cpuBytes", "stale", "readFailures")
        )
        screenshot = output / (item["name"] + ".bmp")
        checks[item["name"] + "Screenshot"] = screenshot.is_file()
        if screenshot.is_file():
            Image.open(screenshot).save(screenshot.with_suffix(".png"))
        checks[item["name"] + "Settled"] = item["settled"]
        if final:
            header = final["header"]
            checks[item["name"] + "GpuBudget"] = int(header["cacheBytes"]) <= int(header["worldGpuCap"])
    generation_values = [int(block["header"]["generation"]) for block in blocks]
    checks["worldReloadGenerationAdvanced"] = options.camera_group != "core" or options.no_world_reload or (
        bool(generation_values) and max(generation_values) > min(generation_values))
    commit_times = [entry["elapsedSeconds"] for entry in milestones if "transaction committed:" in entry["line"]]
    far_times = [entry["elapsedSeconds"] for entry in milestones if entry["line"].endswith("\\far.bmp") or entry["line"].endswith("/far.bmp")]
    metric = lambda values: {"count": len(values), "p50": percentile(values, 50),
                             "p95": percentile(values, 95), "p99": percentile(values, 99)}
    cpu = [row["cpuMs"] for row in perf if isinstance(row.get("cpuMs"), (int, float))]
    gpu = [row["gpuTotalMs"] for row in perf if row.get("valid") and isinstance(row.get("gpuTotalMs"), (int, float))]
    report = {
        "status": "collected" if all(checks.values()) else "incomplete-or-failed",
        "acceptancePassed": False, "worker": options.worker, "weightSourceRequested": options.weight_source,
        "process": process, "checks": checks, "parserProblems": problems, "sections": sections,
        "memory": memory, "cpuMs": metric(cpu), "gpuReportedMs": metric(gpu),
        "gpuTimingScope": "Only reported instrumented passes, not guaranteed whole-frame terrain/TES time",
        "pacingScope": "30Hz cap; CPU percentile includes waits, full run includes reload; not unpaced throughput",
        "worldCommitElapsedSeconds": commit_times, "firstFarScreenshotElapsedSeconds": far_times[:1],
        "oldReaderBaselineWallSeconds": 254.018567,
        "comparisonScope": "Old baseline includes init+300 frames at60Hz; first far screenshot is a close startup milestone, not identical full-run workload. OS disk cache is not forced cold.",
        "milestones": milestones, "noCook": True, "actualLod": re.findall(r"\[TerrainChunk\] drawnByLevel=.*", text),
        "initialStats": blocks[0] if initial_blocks and blocks else None,
        "firstFrameEvidenceRequested": options.first_frame_evidence,
        "worldReloadRequested": not options.no_world_reload,
    }
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"status": report["status"], "pid": process["pid"], "checks": checks,
                      "worldCommitElapsedSeconds": commit_times, "memory": memory}, indent=2))
    if not all(checks.values()):
        raise RuntimeError("Full16 runtime evidence has unresolved checks; inspect report.json")


if __name__ == "__main__":
    main()

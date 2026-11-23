"""Run exactly two graph-MRT quality captures in an assigned GPU window."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from noether_quality_decode import Capture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--frames", type=int, default=180)
    options = parser.parse_args()
    if not 30 <= options.frames <= 300:
        parser.error("--frames must be 30..300")
    root = Path(__file__).resolve().parents[3]
    output = root / "artifacts/terrain-rvt-production/p3-quality-winner"
    prepared = json.loads((output / "prepared.json").read_text("utf-8"))
    stage = output / "stage"
    executable = stage / "bin/main_game.exe"
    if hashlib.sha256(executable.read_bytes()).hexdigest() != prepared["executableSha256"]:
        raise RuntimeError("Prepared Game executable changed")
    run_dir = output / ("run-" + str(time.time_ns()))
    captures = run_dir / "captures"
    captures.mkdir(parents=True)
    environment = os.environ.copy()
    environment["VULKANLEARN_RVT_QUALITY_DIR"] = str(captures)
    environment["VULKANLEARN_RVT_QUALITY_MAX_CAPTURES"] = "2"
    environment["VULKANLEARN_RVT_QUALITY_MODULE_ONLY"] = "1"
    command = [str(executable), "--worker-thread-count", "2", "--frame-limit", "30",
               "--validation", "--no-dev-ui", "--exit-after-frames", str(options.frames),
               "--gpu-stats"]
    started = time.monotonic()
    with (run_dir / "stdout.log").open("w", encoding="utf-8") as stdout, (
            run_dir / "stderr.log").open("w", encoding="utf-8") as stderr:
        process = subprocess.Popen(command, cwd=stage, env=environment, stdout=stdout, stderr=stderr)
        execution = {"pid": process.pid, "argv": command, "captureCap": 2, "gpuExecuted": True,
                     "scope": "winner semantics only; raw persisted before Service Observe; no interactive acceptance claim"}
        (run_dir / "launch.json").write_text(json.dumps(execution, indent=2), encoding="utf-8")
        try:
            process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)
            execution["timedOut"] = True
    execution.update(exitCode=process.returncode, elapsedSeconds=time.monotonic() - started,
                     gpuProcessEnded=True, captures=[])
    failure = None
    for binary in sorted(captures.glob("*.bin")):
        try:
            metadata = json.loads(binary.with_suffix(".json").read_text("utf-8"))
            capture = Capture(binary.read_bytes(), metadata)
            counts = capture.counts()
            execution["captures"].append({
                "binary": str(binary), "frame": capture.frame, "counts": counts,
                "allocationQuery": metadata["allocationQuery"],
                "workspacePeakBytes": metadata["workspacePeakBytes"],
                "captureGpuMilliseconds": metadata["captureGpuMilliseconds"],
                "visibilityTest": metadata["visibilityTest"]})
        except (ValueError, KeyError) as error:
            failure = str(error)
    logs = "".join((run_dir / name).read_text("utf-8", errors="replace")
                   for name in ("stdout.log", "stderr.log"))
    execution["validationErrors"] = logs.count("Validation Error") + logs.count("VUID-")
    execution["decodeFailure"] = failure
    (run_dir / "execution.json").write_text(json.dumps(execution, indent=2), encoding="utf-8")
    print(json.dumps({"directory": str(run_dir), **execution}, indent=2), flush=True)
    if process.returncode or failure or execution["validationErrors"] or len(execution["captures"]) != 2:
        raise RuntimeError("Winner actual-capture validation failed; inspect the run artifacts")


if __name__ == "__main__":
    main()

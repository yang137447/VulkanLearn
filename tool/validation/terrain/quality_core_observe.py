"""Prepare/run the frozen Release core without recompiling or overlaying any object."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

from noether_quality_decode import Capture


ROOT = Path(__file__).resolve().parents[3]
CORE = ROOT / "artifacts/terrain-rvt-production/core-native-r2-frozen"
OUTPUT = ROOT / "artifacts/terrain-rvt-production/p3-core-full-observe"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def inventory(directory):
    return {str(path.relative_to(directory)): digest(path)
            for path in sorted(directory.rglob("*")) if path.is_file()}


def check_core():
    checkpoint = json.loads((CORE / "checkpoint.json").read_text("utf-8"))
    if checkpoint["qualityABI"] != {"version": 2, "bytes": 1619200}:
        raise ValueError("Core Quality ABI differs")
    for name, expected in checkpoint["binaryAndObjectSha256"].items():
        if digest(Path(name)) != expected:
            raise ValueError("Core binary/object hash differs: " + name)
    manifest = CORE / "source-before.json"
    if digest(manifest) != checkpoint["sourceManifestSha256"]:
        raise ValueError("Core source manifest changed")
    sources = json.loads(manifest.read_text("utf-8"))
    quality = {}
    for name in ("source/render/resource/terrainCompositedQualityData.h",
                 "source/render/resource/terrainCompositedQualityGpu.h",
                 "source/render/resource/terrainCompositedQualityGpu.cpp",
                 "source/render/frame/terrainCompositedQualityFrame.cpp"):
        observed = digest(CORE / name)
        if observed != sources[name]:
            raise ValueError("Frozen Quality source differs: " + name)
        quality[name] = observed
    frame = (CORE / "source/render/frame/terrainCompositedQualityFrame.cpp").read_text("utf-8")
    if frame.index("quality->WriteReadback") > frame.index("producer->ObserveQuality"):
        raise ValueError("Core does not persist raw before Observe")
    if "VULKANLEARN_RVT_QUALITY_EVIDENCE_DIR" not in frame:
        raise ValueError("Core lacks black/ready evidence hooks")
    return {"checkpointSha256": digest(CORE / "checkpoint.json"),
            "sourceManifestSha256": digest(manifest), "sourceFileCount": len(sources),
            "qualitySourceSha256": quality, "qualityABI": checkpoint["qualityABI"],
            "gameSha256": checkpoint["binaryAndObjectSha256"][str(CORE / "bin/main_game.exe")],
            "gpuExecuted": False, "objectOverlays": [], "moduleOnly": False}


def events(log, marker):
    result = []
    for line in log.splitlines():
        if marker in line:
            result.append(json.loads(line.split(marker, 1)[1].strip()))
    return result


def bmp(path):
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("Invalid screenshot BMP")
    declared, = struct.unpack_from("<I", data, 2)
    offset, = struct.unpack_from("<I", data, 10)
    width, height, planes, bits, compression = struct.unpack_from("<iiHHI", data, 18)
    if width <= 0 or not height or planes != 1 or bits != 24 or compression != 0:
        raise ValueError("Unsupported screenshot BMP")
    stride = (width * 3 + 3) & ~3
    if len(data) != offset + stride * abs(height) or declared != len(data):
        raise ValueError("Truncated screenshot BMP")
    values = b"".join(data[offset + row * stride:offset + row * stride + width * 3]
                      for row in range(abs(height)))
    return {"path": str(path), "sha256": digest(path), "width": width, "height": abs(height),
            "maxChannel": max(values), "nonzeroChannels": sum(value != 0 for value in values)}


def analyze(run_dir):
    log = "".join((run_dir / name).read_text("utf-8", errors="replace")
                  for name in ("stdout.log", "stderr.log"))
    clocks = events(log, "[RVT_QUALITY_CLOCK]")
    states = events(log, "[RVT_QUALITY_STATE]")
    presents = events(log, "[RVT_QUALITY_PRESENT]")
    observations = []
    errors = []
    for binary in sorted((run_dir / "captures").glob("*.bin")):
        try:
            metadata = json.loads(binary.with_suffix(".json").read_text("utf-8"))
            if metadata.get("moduleOnly") or metadata["binaryAbi"] != "TVQ1-final-results-v2":
                raise ValueError("Capture is module-only or has an incorrect ABI")
            capture = Capture(binary.read_bytes(), metadata)
            counts = capture.counts()
            header = capture.header
            observations.append({"binary": str(binary), "sha256": digest(binary),
                "frame": capture.frame, "poseId": capture.pose, "worldGeneration": capture.generation,
                "submissionEpoch": capture.epoch, "completedEpoch": metadata["completedEpoch"],
                "completedMonotonicMs": metadata["completedMonotonicMs"],
                "captureSequence": header[19] | header[20] << 32,
                "visible": header[3], "resolved": header[6], "duplicate": header[4],
                "invalid": header[5], "dropped": header[21], "overflow": header[22],
                "complete": True, "counts": counts,
                "meetsQuality": bool(counts["visibleSamples"] and not any(counts[name] for name in
                    ("unresolved", "incorrectIdentity", "rootOnlySamples", "uninitializedDraws"))
                    and counts["effectiveMipBias"] <= 1 and
                    counts["validAtTargetPlusBias"] * 100 >= counts["visibleSamples"] * 99),
                "workspacePeakBytes": metadata["workspacePeakBytes"],
                "allocationQuery": metadata["allocationQuery"]})
        except (ValueError, KeyError) as error:
            errors.append({"binary": str(binary), "error": str(error)})
    ready = next((item for item in presents if item["event"] == "loading_released" and item["ready"]), None)
    black = next((item for item in presents if item["event"] == "loading_black_drawn"), None)
    accepted = next((item for item in states if ready and item["interactive"] and
        item["worldGeneration"] == ready["worldGeneration"] and item["poseId"] == ready["poseId"] and
        item["submissionEpoch"] == ready["observationSubmissionEpoch"]), None)
    witness = next((item for item in observations if accepted and item["meetsQuality"] and
        item["submissionEpoch"] == accepted["submissionEpoch"] and
        item["worldGeneration"] == accepted["worldGeneration"] and item["poseId"] == accepted["poseId"]), None)
    screenshots = {}
    for name in ("first-black.bmp", "first-ready.bmp"):
        path = run_dir / "evidence" / name
        if path.exists():
            screenshots[name] = bmp(path)
    deadline_ok = bool(clocks and ready and witness and
        clocks[0]["initialLimitMs"] == 86748.0 and
        ready["monotonicMs"] <= clocks[0]["deadlineMonotonicMs"] and
        witness["completedMonotonicMs"] <= clocks[0]["deadlineMonotonicMs"])
    result = {"scope": "same frozen Release core, full Service Observe; no module-only or object overlay",
        "clocks": clocks, "states": states, "presentEvents": presents, "captures": observations,
        "decodeErrors": errors, "screenshots": screenshots, "initialDeadlineMet": deadline_ok,
        "readyBoundToActualServiceAndRaw": bool(ready and accepted and witness),
        "validationErrors": log.count("VUID-") + log.count("Validation Error"),
        "engineTeardownComplete": "EngineLoop teardown complete" in log,
        "workspacePeakBytes": max((item["workspacePeakBytes"] for item in observations), default=0)}
    result["accepted"] = bool(deadline_ok and accepted and witness and black and
        screenshots.get("first-black.bmp", {}).get("maxChannel") == 0 and
        screenshots.get("first-ready.bmp", {}).get("maxChannel", 0) > 0 and
        result["workspacePeakBytes"] <= 32 * 1024 * 1024 and not errors and
        not result["validationErrors"] and result["engineTeardownComplete"])
    write(run_dir / "observations.json", result)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check-core", action="store_true")
    parser.add_argument("--prepare-stage", type=Path)
    parser.add_argument("--publication-receipt", type=Path)
    parser.add_argument("--run", action="store_true", help="Requires the main thread's GPU window")
    parser.add_argument("--analyze", type=Path)
    options = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if options.analyze:
        report = analyze(options.analyze.resolve())
        print(json.dumps({key: report[key] for key in
              ("accepted", "initialDeadlineMet", "readyBoundToActualServiceAndRaw",
               "validationErrors", "workspacePeakBytes")}, indent=2))
        return
    verified = check_core()
    write(OUTPUT / "core-check.json", verified)
    if options.check_core:
        print(json.dumps(verified, indent=2))
        return
    if options.prepare_stage:
        if not options.publication_receipt:
            parser.error("--prepare-stage requires --publication-receipt from the publication owner")
        source = options.prepare_stage.resolve()
        if digest(source / "bin/main_game.exe") != verified["gameSha256"]:
            raise ValueError("Publication stage Game differs from the unified frozen core")
        config = json.loads((source / "config/config.json").read_text("utf-8"))
        resource_root = Path(config["resourcePath"])
        if not resource_root.is_absolute() or not resource_root.is_dir():
            raise ValueError("Publication stage must use a complete absolute private resource root")
        if not (resource_root / config["initScene"]).is_file():
            raise ValueError("Private resource root is missing the initial scene")
        graph = json.loads((source / "config/renderGraphConfig.json").read_text("utf-8"))
        feedback = next(item for item in graph["passes"] if item["type"] == "terrainFeedback")
        names = [item["resource"] for item in feedback["output"]]
        if len(names) != 8 or names[5:7] != ["terrainQualityWinner0", "terrainQualityWinner1"]:
            raise ValueError("Publication graph does not contain seven feedback MRTs plus depth")
        destination = source.parent / ("QO-" + format(time.time_ns(), "x")[-10:])
        shutil.copytree(source, destination)
        receipt = options.publication_receipt.resolve()
        prepared = {**verified, "stage": str(destination), "sourceStage": str(source),
            "configSha256": digest(destination / "config/config.json"),
            "graphSha256": digest(destination / "config/renderGraphConfig.json"),
            "privateCacheBefore": inventory(destination / "shader/spv"),
            "shaderSources": inventory(destination / "shader/glsl"),
            "resourceRoot": str(resource_root), "initialScene": config["initScene"],
            "publicationReceipt": str(receipt), "publicationReceiptSha256": digest(receipt),
            "receiptValidation": "Owner receipt preserved; native Store/native32 result must be reviewed before GPU dispatch"}
        write(OUTPUT / "prepared.json", prepared)
        print("Prepared: " + str(OUTPUT / "prepared.json"))
        return
    if not options.run:
        parser.error("Choose --check-core, --prepare-stage, --run, or --analyze")
    prepared = json.loads((OUTPUT / "prepared.json").read_text("utf-8"))
    stage = Path(prepared["stage"])
    executable = stage / "bin/main_game.exe"
    if digest(executable) != verified["gameSha256"]:
        raise ValueError("Private Game identity changed")
    if digest(stage / "config/config.json") != prepared["configSha256"] or (
            digest(stage / "config/renderGraphConfig.json") != prepared["graphSha256"]):
        raise ValueError("Private config/graph changed after preparation")
    if inventory(stage / "shader/glsl") != prepared["shaderSources"]:
        raise ValueError("Private shader sources changed")
    run_dir = OUTPUT / ("run-" + str(time.time_ns()))
    for name in ("captures", "evidence"):
        (run_dir / name).mkdir(parents=True)
    environment = os.environ.copy()
    environment.pop("VULKANLEARN_RVT_QUALITY_MODULE_ONLY", None)
    environment["VULKANLEARN_RVT_QUALITY_MAX_CAPTURES"] = "256"
    environment["VULKANLEARN_RVT_QUALITY_DIR"] = str(run_dir / "captures")
    environment["VULKANLEARN_RVT_QUALITY_EVIDENCE_DIR"] = str(run_dir / "evidence")
    command = [str(executable), "--worker-thread-count", "2", "--frame-limit", "30",
               "--validation", "--no-dev-ui", "--exit-after-frames", "3000", "--gpu-stats"]
    started = time.monotonic()
    execution = {"argv": command, "workingDirectory": str(stage), "moduleOnly": False,
        "coreCheckpointSha256": verified["checkpointSha256"], "gameSha256": digest(executable),
        "sourceManifestSha256": verified["sourceManifestSha256"], "gpuExecuted": True,
        "rawRunCapBytes": 256 * 1619200, "autoRestart": False}
    with (run_dir / "stdout.log").open("w", encoding="utf-8") as stdout, (
            run_dir / "stderr.log").open("w", encoding="utf-8") as stderr:
        process = subprocess.Popen(command, cwd=stage, env=environment, stdout=stdout, stderr=stderr)
        execution["pid"] = process.pid
        write(run_dir / "launch.json", execution)
        quit_sent = False
        while process.poll() is None:
            log = (run_dir / "stdout.log").read_text("utf-8", errors="replace")
            if not quit_sent and "Screenshot saved:" in log and (
                    str(run_dir / "evidence/first-ready.bmp") in log) and '"loading_released"' in log:
                injection = subprocess.run(["pwsh", "-NoProfile", "-File",
                    str(ROOT / "tool/validation/console_inject.ps1"), "-InjectProcessId",
                    str(process.pid), "-InjectLine", "quit"], capture_output=True, text=True, timeout=15)
                execution["quitInjection"] = {"exitCode": injection.returncode,
                    "stdout": injection.stdout, "stderr": injection.stderr}
                quit_sent = True
            if time.monotonic() - started > 125:
                process.terminate()
                execution["forcedTermination"] = True
                break
            time.sleep(0.5)
        process.wait(timeout=10)
    execution.update(exitCode=process.returncode, gpuProcessEnded=True,
                     elapsedSeconds=time.monotonic() - started,
                     privateCacheAfter=inventory(stage / "shader/spv"))
    write(run_dir / "execution.json", execution)
    report = analyze(run_dir)
    execution["accepted"] = report["accepted"] and process.returncode == 0
    write(run_dir / "execution.json", execution)
    print(json.dumps({"runDirectory": str(run_dir), "exitCode": process.returncode,
        "gpuProcessEnded": True, "accepted": execution["accepted"],
        "initialDeadlineMet": report["initialDeadlineMet"],
        "workspacePeakBytes": report["workspacePeakBytes"],
        "validationErrors": report["validationErrors"]}, indent=2))


if __name__ == "__main__":
    main()

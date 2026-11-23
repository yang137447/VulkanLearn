"""Serial renderer acceptance driver; run only during the coordinator's exclusive window.

Exit codes: 0 = complete, 1 = driver/setup error, 2 = operational failure,
3 = incomplete evidence. Captures alone never establish visual parity.
Fault injection is deliberately absent: future negative tests must copy Cook
packages into an isolated resource root before changing any bytes.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import uuid


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def read_json(path):
    def reject_constant(value):
        raise ValueError(f"Non-finite JSON constant: {value}")
    return json.loads(path.read_text(encoding="utf-8-sig"), parse_constant=reject_constant)


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def inside(root, name):
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError(f"Path escapes resource root: {name}")
    return path


def inventory(resource_root, scene):
    files = {scene}
    document = read_json(scene)
    for item in document.get("objects", []):
        if item.get("type") != "terrain":
            continue
        terrain_path = inside(resource_root, item["terrainPath"])
        files.add(terrain_path)
        terrain = read_json(terrain_path)
        package = terrain.get("compositedVT", {}).get("packagePath")
        if not package:
            continue
        manifest = inside(resource_root, package)
        files.add(manifest)
        if not manifest.is_file():
            continue
        cooked = read_json(manifest)
        if isinstance(cooked.get("package"), str):
            files.add(inside(manifest.parent, cooked["package"]))
        for surface in cooked.get("surfaces", []):
            dependency = surface.get("dependencyHash", "")
            if not re.fullmatch(r"[0-9a-f]{64}", dependency):
                continue
            for suffix in (".dependencies.json", ".coverage.json", ".height.json"):
                sidecar = manifest.parent / (dependency + suffix)
                if not sidecar.is_file():
                    continue
                files.add(sidecar)
                value = read_json(sidecar)
                for key in ("package", "payload", "binary", "payloadFile"):
                    if isinstance(value.get(key), str):
                        files.add(inside(sidecar.parent, value[key]))
    return {str(path.relative_to(resource_root)): {
        "exists": path.is_file(),
        **({"bytes": path.stat().st_size, "sha256": digest(path)} if path.is_file() else {})
    } for path in sorted(files)}


def camera_shots(scene, capture_path):
    if capture_path and capture_path.is_file():
        shots = read_json(capture_path)["shots"]
        origin = str(capture_path)
    else:
        cameras = [item for item in read_json(scene).get("objects", [])
                   if item.get("type") == "camera"]
        if not cameras:
            raise ValueError("Scene needs a camera or a capture_plan.json")
        position = cameras[0]["position"]
        target = cameras[0]["look_at"]
        shots = [{"name": name, "position": [
            target[axis] + (position[axis] - target[axis]) * scale for axis in range(3)],
                  "look_at": target}
                 for name, scale in (("far", 1.0), ("mid", 0.25), ("near", 0.0625))]
        origin = "inferred from scene camera; framing needs visual review"
    by_name = {shot["name"]: shot for shot in shots}
    result = []
    for name in ("far", "mid", "near"):
        shot = by_name[name]
        values = [float(value) for value in shot["position"] + shot["look_at"]]
        if len(values) != 6 or not all(math.isfinite(value) for value in values):
            raise ValueError(f"Invalid camera: {name}")
        if sum((values[axis] - values[axis + 3]) ** 2 for axis in range(3)) < 1e-8:
            raise ValueError(f"Camera position equals target: {name}")
        result.append({"name": name, "position": values[:3], "look_at": values[3:]})
    if len({tuple(shot["position"] + shot["look_at"]) for shot in result}) != 3:
        raise ValueError("Three distinct camera poses are required")
    return result, origin


def stage_project(project, destination, resource_root):
    for name in ("config", "shader", "ui", "script"):
        source = project / name
        if source.is_dir():
            shutil.copytree(source, destination / name,
                            ignore=shutil.ignore_patterns(".git", "__pycache__", "obj", "bin"))
    config_path = destination / "config/config.json"
    config = read_json(config_path)
    config["resourcePath"] = resource_root.as_posix()
    write_json(config_path, config)
    return config.get("windowSize")


def run_process(argv, cwd, log_path, timeout):
    if sys.platform == "win32" and len(subprocess.list2cmdline(argv)) >= 32767:
        raise ValueError("Command line exceeds Windows limit; reduce --settle-frames")
    started = datetime.now(timezone.utc).isoformat()
    clock = time.monotonic()
    timed_out = False
    with log_path.open("wb") as log:
        process = subprocess.Popen(argv, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                                   shell=False)
        try:
            returncode = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
        finally:
            if process.poll() is None:
                if sys.platform == "win32":
                    try:
                        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       timeout=30, check=False)
                    except subprocess.TimeoutExpired:
                        pass
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=30)
        if timed_out:
            returncode = process.returncode
    text = log_path.read_text(encoding="utf-8", errors="replace")
    patterns = {
        "validation": r"VUID-[^\s]+|Validation Error[^\n]*",
        "runtime": (r"VK_ERROR_[A-Z_]+|Scene\.LoadFailed[^\n]*|World\.LoadFailed[^\n]*"
                    r"|Terrain composited service:[^\n]*|terminate called[^\n]*"
                    r"|Assertion failed[^\n]*|Unknown command[^\n]*|Unknown option[^\n]*"),
    }
    errors = {name: re.findall(pattern, text, flags=re.IGNORECASE)
              for name, pattern in patterns.items()}
    return {"argv": argv, "cwd": str(cwd), "startedUtc": started,
            "seconds": time.monotonic() - clock, "exitCode": returncode,
            "timeout": timed_out, "log": str(log_path), "logSha256": digest(log_path),
            "detectedErrors": errors}, text


def terrain_stats(text, expected_surfaces):
    blocks = []
    current = None
    tokens = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")
    for line in text.splitlines():
        if "TerrainVT generation=" in line:
            current = {"header": dict(tokens.findall(line)), "surfaces": []}
            blocks.append(current)
        elif current and "resident=" in line and "readFailures=" in line:
            fields = dict(tokens.findall(line))
            fields["surfaceId"] = line.strip().split(" generation=", 1)[0]
            current["surfaces"].append(fields)
    problems = []
    if not blocks:
        return blocks, ["Missing TerrainVT statistics"]
    for block in blocks:
        if int(block["header"].get("surfaces", -1)) != expected_surfaces:
            problems.append(f"Expected surfaces={expected_surfaces}: {block['header']}")
        for surface in block["surfaces"]:
            count, capacity = (int(value) for value in surface["resident"].split("/"))
            if not 1 <= count <= capacity:
                problems.append(f"Invalid resident/capacity: {surface['resident']}")
            if int(surface["readFailures"]) != 0:
                problems.append(f"Read failures: {surface['readFailures']}")
    last = blocks[-1]
    if len(last["surfaces"]) != expected_surfaces:
        problems.append("Final TerrainVT block lacks per-surface entries")
    if len({surface["surfaceId"] for surface in last["surfaces"]}) != expected_surfaces:
        problems.append("Final TerrainVT block lacks distinct surface identities")
    for surface in last["surfaces"]:
        for key in ("queued", "reading", "dirty", "stale"):
            if key not in surface:
                problems.append(f"Missing final {key} counter")
            elif int(surface[key]) != 0:
                problems.append(f"Final {key} did not settle: {surface[key]}")
    return blocks, sorted(set(problems))


def counter_evidence(blocks):
    histories = {}
    previous_histograms = {}
    mip_distributions = {}
    scalar_mips = {}
    for block in blocks:
        for fields in [block["header"], *block["surfaces"]]:
            for key, value in fields.items():
                if re.search(r"feedback.*(request|hit|count|fallback|miss)|request.*(page|count)|target.*mip",
                             key, flags=re.IGNORECASE):
                    histories.setdefault(key, set()).add(value)
                if not re.search(r"target.*mip", key, flags=re.IGNORECASE):
                    continue
                identity = (fields.get("surfaceId", "world"), key)
                pairs = re.findall(r"(\d+):(\d+)", value)
                counts = None
                if pairs:
                    indexed = {int(index): int(count) for index, count in pairs}
                    if max(indexed) <= 127:
                        counts = [indexed.get(index, 0) for index in range(max(indexed) + 1)]
                elif re.fullmatch(r"\d+(?:[,;/|]\d+)+", value):
                    counts = [int(count) for count in re.split(r"[,;/|]", value)]
                if counts is None:
                    if re.fullmatch(r"\d+(?:\.\d+)?", value):
                        scalar_mips.setdefault(identity, set()).add(float(value))
                    continue
                previous = previous_histograms.get(identity)
                previous_histograms[identity] = counts
                delta = counts if previous is None or len(previous) != len(counts) else [
                    current - old for current, old in zip(counts, previous)]
                if any(count < 0 for count in delta):
                    delta = counts
                total = sum(delta)
                if total:
                    mip_distributions.setdefault(identity, set()).add(
                        tuple(round(count / total, 6) for count in delta))
    feedback = [key for key in histories if "mip" not in key.lower() and
                re.search(r"request|feedback.*count", key, flags=re.IGNORECASE)]
    mips = [key for key in histories if re.search(r"target.*mip", key, flags=re.IGNORECASE)]
    missing = []
    if not feedback:
        missing.append("No feedback/readback request counter is exposed")
    if not mips:
        missing.append("No target-mip counter/histogram is exposed")
    elif not (any(len(values) > 1 for values in mip_distributions.values()) or
              any(len(values) > 1 for values in scalar_mips.values())):
        missing.append("No target-mip variation observed across the three poses")
    if feedback and not any(re.search(r"[1-9]", value)
                            for key in feedback for value in histories[key]):
        missing.append("Feedback request counters never showed activity")
    missing.append("GPU sampled-mip/fallback pixels are unobserved; CPU estimates cannot prove shader residency")
    return {key: sorted(values) for key, values in histories.items()}, missing


def bmp_integrity(path, viewport):
    raw = path.read_bytes()
    if len(raw) < 54 or raw[:2] != b"BM":
        raise ValueError("Missing BMP header")
    size, offset = struct.unpack_from("<I", raw, 2)[0], struct.unpack_from("<I", raw, 10)[0]
    dib = struct.unpack_from("<I", raw, 14)[0]
    width, signed_height, planes, bits, compression = struct.unpack_from("<iiHHI", raw, 18)
    if dib < 40 or offset < 14 + dib or planes != 1 or bits not in (24, 32):
        raise ValueError("Unsupported/inconsistent BMP layout")
    if compression not in (0, 3) or (bits == 24 and compression != 0):
        raise ValueError("Unsupported BMP compression")
    if not 1 <= width <= 32768 or not 1 <= abs(signed_height) <= 32768:
        raise ValueError("Invalid BMP dimensions")
    height = abs(signed_height)
    stride = ((width * bits + 31) // 32) * 4
    required = offset + stride * height
    if size != len(raw) or required > len(raw):
        raise ValueError(f"Truncated/inconsistent BMP: required={required}, actual={len(raw)}, header={size}")
    if viewport and [width, height] != viewport:
        raise ValueError(f"BMP extent {[width, height]} differs from viewport {viewport}")
    pixels = width * height
    unique = set()
    for index in range(0, pixels, max(1, pixels // 4096)):
        row, column = divmod(index, width)
        start = offset + row * stride + column * (bits // 8)
        unique.add(raw[start:start + 3])
    return {"width": width, "height": height, "bits": bits, "bytes": len(raw),
            "requiredBytes": required, "sampledUniqueRgb": len(unique), "sha256": digest(path)}


def performance(path):
    def reject_constant(value):
        raise ValueError(f"Non-finite performance sample: {value}")
    rows = [json.loads(line, parse_constant=reject_constant)
            for line in path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    if not rows or any(not isinstance(row, dict) for row in rows):
        raise ValueError("Performance file lacks JSON object samples")
    values = {}
    def visit(value, prefix=""):
        if isinstance(value, dict):
            for key, item in value.items():
                visit(item, f"{prefix}.{key}" if prefix else key)
        elif isinstance(value, (float, int)) and not isinstance(value, bool):
            if not math.isfinite(value):
                raise ValueError("Non-finite performance sample")
            values.setdefault(prefix, []).append(value)
    for row in rows:
        visit(row)
    return {"samples": len(rows), "sha256": digest(path), "metrics": {
        key: {"min": min(samples), "median": statistics.median(samples), "max": max(samples)}
        for key, samples in values.items()}}


def execute(options, report, directory, staged):
    project = Path(__file__).resolve().parents[3]
    resource = options.resource_root.resolve(strict=True)
    executable = options.exe.resolve(strict=True)
    scene = inside(resource, options.scene)
    capture = (options.capture_plan.resolve(strict=True) if options.capture_plan else
               scene.parent / "capture_plan.json")
    shots, origin = camera_shots(scene, capture)
    viewport = stage_project(project, staged, resource)
    source_hashes = {}
    for folder in ("config", "shader"):
        for path in sorted((staged / folder).rglob("*")):
            if path.is_file() and path.suffix in (
                    ".json", ".glsl", ".comp", ".frag", ".vert", ".tesc", ".tese"):
                if "spv" not in path.relative_to(staged).parts:
                    source_hashes[path.relative_to(staged).as_posix()] = digest(path)
    report.update({"exe": str(executable), "exeSha256": digest(executable),
                   "exeModifiedUtc": datetime.fromtimestamp(
                       executable.stat().st_mtime, timezone.utc).isoformat(),
                   "cppSnapshot": {name: {
                       "sha256": digest(project / name),
                       "modifiedUtc": datetime.fromtimestamp(
                           (project / name).stat().st_mtime, timezone.utc).isoformat()
                   } for name in (
                       "source/render/resource/terrainCompositedService.cpp",
                       "source/render/terrain/terrainCompositedRuntime.cpp",
                       "source/terrain/terrainCompositedResidency.cpp",
                       "source/terrain/terrainCompositedVT.cpp",
                       "source/terrain/terrainCompositedVTStore.cpp")},
                   "binarySourceEquivalence": "not proven by timestamps or hashes",
                   "driverSha256": digest(Path(__file__)), "resourceRoot": str(resource),
                   "scene": str(scene), "sceneSha256": digest(scene),
                   "shots": shots, "cameraPlan": origin, "viewport": viewport,
                   "stagedSourceSha256": source_hashes,
                   "settings": {"validation": True, "timeoutSeconds": options.timeout,
                                "delayFrames": options.delay, "settleFrames": options.settle_frames,
                                "frameLimitHz": options.frame_limit,
                                "expectedSurfaces": options.expected_surfaces, "cook": options.cook},
                   "coldStartDefinition": "Fresh process/GPU residency; OS file cache is uncontrolled",
                   "failureInjection": "Not run; requires an isolated copied Cook resource root",
                   "quality": "unverified: no direct/composite image or material-channel reference",
                   "telemetryInterpretation": (
                       "fallback/misses and feedbackEstimated* describe CPU Resolve at feedback "
                       "consumption, not GPU pixel sampling. Zero is never proof of no shader fallback. "
                       "Cumulative target-mip histogram growth alone is not evidence of LOD variation."),
                   "before": inventory(resource, scene)})
    common = [str(executable), "--initial-scene", scene.relative_to(resource).as_posix(),
              "--no-dev-ui", "--validation", "--frame-limit", str(options.frame_limit)]
    if options.cook:
        cook, _ = run_process(common + ["--terrain-vt-cook", "--worker-thread-count", "1",
                                       "--exit-after-frames", "4", "--console-command-delay", "1",
                                       "--console-command", "terrainvt stats"],
                              staged, directory / "cook.log", options.timeout)
        report["cook"] = cook
        write_json(directory / "run.json", report)
        if cook["exitCode"] or cook["timeout"] or any(cook["detectedErrors"].values()):
            report["failures"].append("Cook failed; readonly runs were not started")
            return
    report["cookedInventory"] = inventory(resource, scene)
    for worker in (1, 2):
        commands = []
        captures = []
        for shot in shots:
            values = shot["position"] + shot["look_at"]
            commands.append("camera pose " + " ".join(format(value, ".9g") for value in values))
            commands.extend(["terrainvt stats"] * options.settle_frames)
            name = f"vt_{report['runId']}_w{worker}_{shot['name']}.bmp"
            commands.append(f"screenshot {name}")
            commands.extend(["terrainvt stats"] * 8)
            captures.append((shot["name"], resource / "Generated/Screenshots" / name))
        commands.extend(["terrainvt stats"] * options.settle_frames)
        perf = directory / f"worker{worker}.perf.jsonl"
        argv = common + ["--worker-thread-count", str(worker), "--perf-jsonl", str(perf),
                         "--console-command-delay", str(options.delay),
                         "--exit-after-frames", str(options.delay + len(commands) + 16)]
        for command in commands:
            argv.extend(["--console-command", command])
        run, text = run_process(argv, staged, directory / f"worker{worker}.log", options.timeout)
        run["workerCount"] = worker
        run["commandSchedule"] = [{"frameAtLeast": options.delay + index, "command": command}
                                  for index, command in enumerate(commands)]
        run["failures"] = []
        if run["exitCode"] or run["timeout"] or any(run["detectedErrors"].values()):
            run["failures"].append("Process exit/timeout/runtime/validation failure")
        run["terrainSnapshots"], problems = terrain_stats(text, options.expected_surfaces)
        run["failures"].extend(problems)
        run["counterHistories"], missing = counter_evidence(run["terrainSnapshots"])
        report["missingEvidence"].extend(f"worker{worker}: {item}" for item in missing)
        run["screenshots"] = {}
        for name, source in captures:
            try:
                target = directory / f"worker{worker}_{name}.bmp"
                shutil.copyfile(source, target)
                run["screenshots"][name] = bmp_integrity(target, viewport)
            except (OSError, ValueError) as error:
                run["failures"].append(f"{name} BMP: {error}")
        if len(run["screenshots"]) == 3 and len({
                item["sha256"] for item in run["screenshots"].values()}) < 3:
            report["missingEvidence"].append(f"worker{worker}: camera captures are not all distinct")
        try:
            run["performance"] = performance(perf)
            if run["performance"]["samples"] < 2 or not run["performance"]["metrics"]:
                report["missingEvidence"].append(f"worker{worker}: insufficient performance samples")
        except (OSError, ValueError) as error:
            run["failures"].append(f"Performance JSONL: {error}")
        report["runs"].append(run)
        report["failures"].extend(f"worker{worker}: {item}" for item in run["failures"])
        write_json(directory / "run.json", report)
    report["after"] = inventory(resource, scene)
    if report["after"] != report["cookedInventory"]:
        report["failures"].append("Readonly runs changed scene/TR/Cook package closure")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--scene", type=Path, required=True)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cook", action="store_true")
    parser.add_argument("--capture-plan", type=Path)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--delay", type=int, default=120)
    parser.add_argument("--settle-frames", type=int, default=120)
    parser.add_argument("--frame-limit", type=int, default=60)
    parser.add_argument("--expected-surfaces", type=int, default=4)
    options = parser.parse_args()
    if not math.isfinite(options.timeout) or options.timeout <= 0 or min(
            options.delay, options.settle_frames, options.frame_limit, options.expected_surfaces) < 1:
        parser.error("Timeout/frame/count options must be positive")
    if options.frame_limit > 1000:
        parser.error("--frame-limit must be <=1000")
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "_" + uuid.uuid4().hex[:8]
    directory = options.output.resolve() / f"run_{run_id}"
    directory.mkdir(parents=True, exist_ok=False)
    report = {"runId": run_id, "status": "running", "runs": [], "failures": [],
              "missingEvidence": [
                  "Direct/composited visual and material-channel parity is not measured",
                  "Negative corruption/version tests are not run against isolated package copies",
                  "Observed target-mip changes need attribution to individual camera poses",
                  "Composited force-mip override is not exercised; legacy layer forcing is not evidence",
              ]}
    fatal = False
    try:
        with tempfile.TemporaryDirectory(prefix="vl-vt-") as temporary:
            staged = Path(temporary).resolve()
            report["stagingPath"] = str(staged)
            report["stagingEphemeral"] = True
            execute(options, report, directory, staged)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report["failures"].append(f"Driver/setup failure: {type(error).__name__}: {error}")
        fatal = True
    except KeyboardInterrupt:
        report["failures"].append("Interrupted; any active renderer child was terminated")
        fatal = True
    report["missingEvidence"] = sorted(set(report["missingEvidence"]))
    report["status"] = ("failed" if report["failures"] else
                        "incomplete" if report["missingEvidence"] else "passed")
    report["finishedUtc"] = datetime.now(timezone.utc).isoformat()
    write_json(directory / "run.json", report)
    print(json.dumps({"status": report["status"], "report": str(directory / "run.json"),
                      "failures": report["failures"],
                      "missingEvidence": report["missingEvidence"]}, indent=2, ensure_ascii=False))
    return 1 if fatal else 2 if report["failures"] else 3 if report["missingEvidence"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

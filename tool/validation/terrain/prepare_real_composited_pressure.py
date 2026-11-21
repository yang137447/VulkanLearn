import argparse
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess

from compile_frozen_cpp import compile_frozen


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


@contextlib.contextmanager
def observe_gpu(directory):
    executable = shutil.which("nvidia-smi")
    metadata = {"available": bool(executable), "clockControlChanged": False}
    if not executable:
        yield metadata
        return
    command = [executable, "--query-gpu=timestamp,index,name,driver_version,pstate,"
               "clocks.current.graphics,clocks.current.memory,power.draw,temperature.gpu,utilization.gpu",
               "--format=csv", "-lms", "200"]
    metadata["command"] = command
    with (directory / "gpu-clock-pstate.csv").open("w", encoding="utf-8") as stdout, \
            (directory / "gpu-clock-pstate.stderr.log").open("w", encoding="utf-8") as stderr:
        collector = subprocess.Popen(command, stdout=stdout, stderr=stderr)
        try:
            yield metadata
        finally:
            if collector.poll() is None:
                collector.terminate()
            metadata["collectorExitCode"] = collector.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--candidate-build", type=Path, required=True)
    parser.add_argument("--coverage-manifest", type=Path, required=True)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--local-terrain", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--execute-gpu", action="store_true")
    parser.add_argument("--reuse-ready", action="store_true")
    parser.add_argument("--reuse-objects", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = options.output.resolve()
    candidate = options.candidate_build.resolve()
    coverage = options.coverage_manifest.resolve()
    resources = options.resource_root.resolve()
    build = root / "build-vt-release/source"
    output.mkdir(parents=True, exist_ok=True)
    private = output / "project"
    executable = output / "realCompositedPressure.exe"
    if not options.reuse_ready:
        candidate_build = json.loads((candidate / "build.json").read_text())
        if not candidate_build.get("coverageStaticTop4") or candidate_build.get("area4LanesPerTexel") != 8:
            raise RuntimeError("Real pressure requires the independently reviewed adaptive8+top4 candidate")
        manifest = {"gpuExecuted": False, "scope": "10936 unique real tile00 pages; two weights; no fixture images",
                    "candidateBuild": str(candidate), "coverageManifest": str(coverage),
                    "candidateBuildSha256": digest(candidate / "build.json"),
                    "coverageManifestSha256": digest(coverage), "compiler": options.compiler,
                    "compilationClosures": {}, "sourceProductionModified": False}
        for path, key in ((candidate / "parallelBaker.cpp.obj", "candidateBakerObjectSha256"),):
            manifest[key] = digest(path)
        shutil.copytree(candidate / "project", private, dirs_exist_ok=True)
        config = json.loads((private / "config/config.json").read_text())
        config["resourcePath"] = str(resources)
        config["windowSize"] = [1280, 720]
        config["ui"]["enabled"] = False
        (private / "config/config.json").write_text(json.dumps(config, indent=2))
        support_source = build / "libvl_runtime.a"
        support = output / "runtime-support.a"
        support_hash = digest(support_source)
        shutil.copy2(support_source, support)
        if digest(support) != support_hash:
            raise RuntimeError("Main runtime archive changed while taking the read-only link snapshot")
        manifest["runtimeSupportSha256"] = support_hash
        libraries = (candidate / "libraries.rsp").read_text()
        old_archive = root / "artifacts/terrain-rvt-production/p3-baker-world-repeat-fix/current-runtime.a"
        if old_archive.as_posix() not in libraries:
            raise RuntimeError("Candidate link archive identity does not match the recorded runtime support")
        libraries = libraries.replace(old_archive.as_posix(), support.as_posix(), 1)
        response = output / "libraries.rsp"
        response.write_text(libraries)
        flags = ["-O2", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
                 "-DVULKANLEARN_ENABLE_VULKAN_VALIDATION", "-std=gnu++17", "-ffp-contract=off",
                 "-ffunction-sections", "-fdata-sections",
                 "@" + str(build / "CMakeFiles/vl_runtime.dir/includes_CXX.rsp")]
        sources = [
            "source/render/resource/terrainCompositedProductionGpu.cpp",
            "source/terrain/terrainCompositedProduction.cpp",
            "source/terrain/terrainCompositedVTStore.cpp",
            "source/terrain/terrainCompositedVT.cpp",
            "source/terrain/terrainCompositedAdjacency.cpp",
            "source/terrain/terrainCompositedDependencies.cpp",
            "source/terrain/terrainRvtBudget.cpp",
            "source/terrain/terrainCompositedWeightOverlay.cpp",
            "source/terrain/terrainAssetLoader.cpp",
            "tool/validation/terrain/composited_real_pressure_check.cpp",
        ]
        objects = [candidate / "parallelBaker.cpp.obj"]
        for name in sources:
            source = root / name
            target = output / (source.name + ".obj")
            print("Freezing and compiling " + name, flush=True)
            closure_file = target.with_suffix(".compile-closure.json")
            record = json.loads(closure_file.read_text()) if closure_file.exists() else None
            reusable = options.reuse_objects and record and target.exists() and \
                record["objectSha256"] == digest(target) and record["flags"] == flags and \
                all(Path(path).exists() and digest(Path(path)) == expected
                    for path, expected in record["filesBeforeAndAfterPreprocessing"].items())
            if not reusable:
                record = compile_frozen(options.compiler, flags, source, target, build)
            manifest["compilationClosures"][name] = record
            objects.append(target)
            (output / "build-in-progress.json").write_text(json.dumps(manifest, indent=2))
        subprocess.run([options.compiler, *map(str, objects), "-Wl,--gc-sections",
                        "-Wl,-Map=" + str(output / "link.map"), "-o", str(executable),
                        "@" + str(response)], cwd=build, check=True)
        environment = os.environ.copy()
        environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
        local = output / "local-snapshot"
        command = [str(executable), "--freeze-local", str(coverage), options.local_terrain, str(local)]
        result = subprocess.run(command, cwd=private, env=environment,
                                capture_output=True, text=True, errors="replace")
        (output / "freeze-local.stdout.log").write_text(result.stdout, encoding="utf-8")
        (output / "freeze-local.stderr.log").write_text(result.stderr, encoding="utf-8")
        if result.returncode:
            raise RuntimeError("CPU real Local snapshot failed: " + result.stderr[-3000:])
        manifest["localFreezeReceipt"] = json.loads((local / "freeze-receipt.json").read_text())
        manifest["executableSha256"] = digest(executable)
        manifest["runtimeFilesSha256"] = {
            str(path.relative_to(output)): digest(path)
            for directory in (private / "shader", local)
            for path in directory.rglob("*") if path.is_file()}
        publication = json.loads(coverage.read_text())
        manifest["realInputFilesSha256"] = {}
        for item in publication["production"]["objects"]:
            path = coverage.parent / item["path"]
            manifest["realInputFilesSha256"][str(path)] = digest(path)
        manifest["status"] = "ready-not-executed"
        (output / "build.json").write_text(json.dumps(manifest, indent=2))
    else:
        manifest = json.loads((output / "build.json").read_text())
    if digest(executable) != manifest["executableSha256"] or \
            digest(coverage) != manifest["coverageManifestSha256"]:
        raise RuntimeError("Frozen executable or base World manifest changed")
    for key in ("runtimeFilesSha256", "realInputFilesSha256"):
        for name, expected in manifest[key].items():
            path = output / name if key == "runtimeFilesSha256" else Path(name)
            if digest(path) != expected:
                raise RuntimeError("Frozen pressure input changed: " + name)
    if not options.execute_gpu:
        print(json.dumps({"status": "ready-not-executed", "gpuExecuted": False,
                          "scope": manifest["scope"], "executable": str(executable)}))
        return
    runs = output / "runs"
    runs.mkdir(exist_ok=True)
    directory = runs / f"run-{len(list(runs.iterdir())) + 1:03}"
    directory.mkdir()
    shutil.copy2(output / "build.json", directory / "build.json")
    environment = os.environ.copy()
    environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
    summary = {"gpuExecuted": True, "scope": manifest["scope"], "modes": [],
               "formalFrameRenderingScope": False, "coldSamplesRetained": True}
    rows = []
    with observe_gpu(directory) as telemetry:
        summary["gpuClockAndPstateObservation"] = telemetry
        for mode in (0, 1):
            selected = directory / f"mode-{mode}"
            selected.mkdir()
            command = [str(executable), "--run", str(coverage),
                       str(output / "local-snapshot/local-closure.json"), str(mode), str(selected)]
            with (selected / "stdout.log").open("w", encoding="utf-8") as stdout, \
                    (selected / "stderr.log").open("w", encoding="utf-8") as stderr:
                process = subprocess.Popen(command, cwd=private, env=environment, stdout=stdout, stderr=stderr)
                (selected / "launch.json").write_text(json.dumps({"pid": process.pid, "command": command}, indent=2))
                code = process.wait()
            report = json.loads((selected / "report.json").read_text())
            logs = (selected / "stdout.log").read_text() + (selected / "stderr.log").read_text()
            summary["modes"].append({"weightSource": mode, "exitCode": code, "report": report,
                                     "validationErrors": logs.count("Validation Error:")})
            raw = selected / "production-raw.jsonl"
            if raw.exists():
                rows.extend(json.loads(line) for line in raw.read_text().splitlines() if line.strip())
    expected = {(mode, mip, page_x, page_y)
                for mode in (0, 1) for mip in range(14)
                for page_y in range((max(1, 8192 >> mip) + 127) // 128)
                for page_x in range((max(1, 8192 >> mip) + 127) // 128)}
    observed = [(row["weightSource"], row["mip"], row["x"], row["y"]) for row in rows]
    complete = len(rows) == 10936 and len(set(observed)) == 10936 and set(observed) == expected
    complete = complete and all(
        row["status"] == 0 and row["heightBlend"] == 3 and row["areaSamples"] == (1 if row["mip"] == 0 else 4)
        and all(isinstance(row[field], (int, float)) and math.isfinite(row[field]) and row[field] >= 0
                for field in ("productionMs", "atlasCopyMs", "combinedMs")) for row in rows)
    complete = complete and all(not mode["report"].get("error") and not mode["validationErrors"]
                                for mode in summary["modes"])
    summary["completeUnique10936"] = complete
    summary["samples"] = len(rows)
    summary["maximumProductionMs"] = max((row["productionMs"] for row in rows), default=None)
    summary["maximumAtlasCopyMs"] = max((row["atlasCopyMs"] for row in rows), default=None)
    summary["maximumSamePageCombinedMs"] = max((row["combinedMs"] for row in rows), default=None)
    summary["overBudgetPages"] = [row for row in rows
                                  if row["productionMs"] > 4 or row["atlasCopyMs"] > 4 or row["combinedMs"] > 6]
    summary["status"] = "passed-finite-real-input" if complete and not summary["overBudgetPages"] else \
        "failed-performance" if complete else "incomplete"
    (directory / "report.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({"status": summary["status"], "samples": len(rows),
                      "report": str(directory / "report.json")}))
    if summary["status"] != "passed-finite-real-input":
        raise SystemExit(1)


if __name__ == "__main__":
    main()

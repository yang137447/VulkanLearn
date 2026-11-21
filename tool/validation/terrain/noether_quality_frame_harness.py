"""Stage/link the real game with fresh quality/frame code. GPU execution is opt-in."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true", help="Requires an assigned exclusive GPU window")
    parser.add_argument("--scene")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--worker", type=int, choices=(1, 2), default=2)
    options = parser.parse_args()
    if not 1 <= options.frames <= 2000:
        parser.error("--frames must be 1..2000")
    root = Path(__file__).resolve().parents[3]
    output = root / "artifacts/terrain-rvt-production/p3-noether-frame"
    output.mkdir(parents=True, exist_ok=True)
    build = root / "build-vt-release/source"
    compiler = shutil.which("g++")
    source_archive = build / "libvl_runtime.a"
    support = output / "runtime-support.a"
    if not support.exists():
        before = digest(source_archive)
        shutil.copy2(source_archive, support)
        if digest(source_archive) != before or digest(support) != before:
            raise RuntimeError("Shared runtime archive changed while staging support")
    sources = [
        "source/main_game.cpp",
        "source/render/frame/renderFrameRecorder.cpp",
        "source/render/frame/terrainCompositedQualityFrame.cpp",
        "source/render/pass/terrainQualityCapturePass.cpp",
        "source/render/resource/terrainCompositedQualityGpu.cpp",
        "source/render/resource/terrainCompositedQualityReplay.cpp",
        "source/render/resource/terrainCompositedService.cpp",
    ]
    hashes = {name: digest(root / name) for name in sources}
    previous_file = output / "build.json"
    previous = json.loads(previous_file.read_text("utf-8")) if previous_file.exists() else {}
    manifest = {"scope": "real main_game; fresh Noether frame/readback and Euclid snapshot conversion",
                "supportSha256": digest(support), "freshSourceSha256": hashes,
                "objectSha256": {}, "commands": [], "gpuExecuted": False}

    def run(command, cwd=root, environment=None):
        manifest["commands"].append(list(map(str, command)))
        result = subprocess.run(list(map(str, command)), cwd=cwd, env=environment,
                                capture_output=True, text=True)
        with (output / "checks.log").open("a", encoding="utf-8") as stream:
            stream.write(repr(command) + "\n" + result.stdout + result.stderr + "\n")
        print(result.stdout + result.stderr, end="", flush=True)
        result.check_returncode()

    (output / "checks.log").write_text("", encoding="utf-8")
    objects = []
    for name in sources:
        target = output / (name.replace("/", "_") + ".obj")
        if not (target.exists() and previous.get("freshSourceSha256", {}).get(name) == hashes[name]
                and previous.get("objectSha256", {}).get(target.name) == digest(target)):
            print("Compiling " + name, flush=True)
            run([compiler, "-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL", "-std=gnu++17",
                 "-ffunction-sections", "-fdata-sections",
                 "@" + str(build / "CMakeFiles/vl_runtime.dir/includes_CXX.rsp"),
                 "-c", root / name, "-o", target])
        objects.append(target)
        manifest["objectSha256"][target.name] = digest(target)
    libraries = (build / "CMakeFiles/main_game.dir/linkLibs.rsp").read_text()
    libraries = libraries.replace("libvl_runtime.a", '"' + support.as_posix() + '"', 1)
    (output / "libraries.rsp").write_text(libraries, encoding="utf-8")
    executable = output / "main_game_quality.exe"
    run([compiler, *objects, "-Wl,--gc-sections", "-Wl,-Map=" + str(output / "link.map"),
         "-o", executable, "@" + str(output / "libraries.rsp")], cwd=build)
    if any(digest(root / name) != source_hash for name, source_hash in hashes.items()):
        raise RuntimeError("Source changed during native frame harness compilation")
    manifest["exeSha256"] = digest(executable)
    (output / "build.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    if options.run:
        run_directory = output / "runs" / str(time.time_ns())
        run_directory.mkdir(parents=True)
        environment = os.environ.copy()
        environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
        environment["VULKANLEARN_RVT_QUALITY_DIR"] = str(run_directory / "captures")
        command = [str(executable), "--exit-after-frames", str(options.frames),
                   "--worker-thread-count", str(options.worker), "--frame-limit", "30", "--no-dev-ui"]
        if options.scene:
            command.extend(["--initial-scene", options.scene])
        with (run_directory / "stdout.log").open("w", encoding="utf-8") as stdout, (
                run_directory / "stderr.log").open("w", encoding="utf-8") as stderr:
            result = subprocess.run(command, cwd=root, env=environment, stdout=stdout, stderr=stderr)
        shutil.copy2(output / "build.json", run_directory / "build.json")
        (run_directory / "execution.json").write_text(json.dumps(
            {"argv": command, "exitCode": result.returncode}, indent=2), encoding="utf-8")
        result.check_returncode()


if __name__ == "__main__":
    main()

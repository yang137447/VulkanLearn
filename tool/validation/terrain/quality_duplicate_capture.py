"""Incrementally overlay the successful real Game; only --run starts Vulkan."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

from quality_duplicate_decode import decode


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--frames", type=int, default=300)
    options = parser.parse_args()
    if not 30 <= options.frames <= 600:
        parser.error("--frames must be 30..600")
    root = Path(__file__).resolve().parents[3]
    output = root / "artifacts/terrain-rvt-production/p3-quality-duplicates-v2"
    output.mkdir(parents=True, exist_ok=True)
    native = Path(os.environ["TEMP"]) / "vulkanlearn-noether-quality"
    base = root / "artifacts/terrain-rvt-production/p3-noether-game-min"
    stage = output / "stage"
    commands = []

    def run(command, cwd=root):
        commands.append(list(map(str, command)))
        result = subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True, text=True)
        with (output / "build.log").open("a", encoding="utf-8") as stream:
            stream.write(repr(commands[-1]) + "\n" + result.stdout + result.stderr + "\n")
        if result.returncode:
            print(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout

    if not options.run:
        (output / "build.log").write_text("", encoding="utf-8")
        if not stage.exists():
            shutil.copytree(base / "stage", stage)
        source = root / "shader/glsl/terrain/terrainCompositedQualityVisible.glsl"
        shutil.copy2(source, stage / "shader/glsl/terrain" / source.name)
        for shader_stage in ("vert", "frag"):
            shader = stage / f"shader/glsl/pass/terrainFeedback.{shader_stage}"
            text = shader.read_text("utf-8").replace(
                "#version 450", "#version 450\n#extension GL_GOOGLE_include_directive : require", 1)
            (output / f"feedback.{shader_stage}").write_text(text, encoding="utf-8")
        run(["glslangValidator", "-V", "--target-env", "vulkan1.2", "-l",
             "-I" + str(stage / "shader/glsl/pass"), "-DVL_TERRAIN_QUALITY_CAPTURE=1",
             output / "feedback.vert", output / "feedback.frag", "-o", output / "visible.spv"])
        run(["spirv-val", "--target-env", "vulkan1.2", output / "visible.spv"])
        run(["glslangValidator", "-V", "--target-env", "vulkan1.2",
             stage / "shader/glsl/terrain/terrainCompositedQuality.comp",
             "-o", output / "resolve.spv"])
        run(["spirv-val", "--target-env", "vulkan1.2", output / "resolve.spv"])
        includes = native / "run006-coherent/includes.rsp"
        sources = ["source/main_game.cpp", "source/render/resource/terrainCompositedQualityGpu.cpp"]
        hashes = {name: digest(root / name) for name in sources}
        objects = []
        for name in sources:
            target = output / (Path(name).stem + ".obj")
            print("Compiling " + name, flush=True)
            run(["g++", "-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL", "-std=gnu++17",
                 "-ffunction-sections", "-fdata-sections", "@" + str(includes),
                 "-c", root / name, "-o", target])
            objects.append(target)
        launch = json.loads((base / "run-20261121T035333501660Z/launch.json").read_text("utf-8"))
        overlays = [Path(launch["deltaObject"]),
                    *(Path(item["path"]) for item in launch["requiredObjects"])]
        for item, expected in zip(overlays, [launch["deltaObjectSha256"],
                                             *(item["sha256"] for item in launch["requiredObjects"])]):
            if digest(item) != expected:
                raise RuntimeError("Successful real Game overlay changed: " + str(item))
        libraries = native / "run006-coherent/libraries.rsp"
        run(["g++", *objects, *overlays, "-Wl,--gc-sections",
             "-Wl,-Map=" + str(output / "game.map"), "-o", stage / "bin/main_game.exe",
             "@" + str(libraries)])
        if any(digest(root / name) != value for name, value in hashes.items()):
            raise RuntimeError("Source changed during compilation")
        manifest = {"gpuExecuted": False, "commands": commands, "sourceSha256": hashes,
                    "visibleSourceSha256": digest(source),
                    "executableSha256": digest(stage / "bin/main_game.exe"),
                    "supportArchiveSha256": digest(native / "run006-coherent/runtime-support-run006.a"),
                    "baseRun": "run-20261121T035333501660Z",
                    "overlays": {str(path): digest(path) for path in overlays},
                    "payloadBytes": 1619200, "duplicateAppendixBytes": 12288,
                    "maxCaptures": 2, "sharedBuildWritten": False}
        (output / "build.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        print("Prepared real Game diagnostic: " + str(stage / "bin/main_game.exe"), flush=True)
        return
    manifest = json.loads((output / "build.json").read_text("utf-8"))
    executable = stage / "bin/main_game.exe"
    if digest(executable) != manifest["executableSha256"]:
        raise RuntimeError("Prepared binary changed")
    run_dir = output / ("run-" + str(time.time_ns()))
    captures = run_dir / "captures"
    captures.mkdir(parents=True)
    environment = os.environ.copy()
    environment["VULKANLEARN_RVT_QUALITY_DIR"] = str(captures)
    environment["VULKANLEARN_RVT_QUALITY_MAX_CAPTURES"] = "2"
    command = [str(executable), "--worker-thread-count", "2", "--frame-limit", "30",
               "--validation", "--no-dev-ui", "--exit-after-frames", str(options.frames)]
    started = time.monotonic()
    with (run_dir / "stdout.log").open("w", encoding="utf-8") as stdout, (
            run_dir / "stderr.log").open("w", encoding="utf-8") as stderr:
        process = subprocess.Popen(command, cwd=stage, env=environment, stdout=stdout, stderr=stderr)
        execution = {"pid": process.pid, "argv": command, "gpuExecuted": True,
                     "workingDirectory": str(stage), "captureCap": 2}
        (run_dir / "launch.json").write_text(json.dumps(execution, indent=2), encoding="utf-8")
        try:
            process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=10)
            execution["timedOut"] = True
    execution.update(exitCode=process.returncode, elapsedSeconds=time.monotonic() - started,
                     gpuProcessEnded=True)
    reports = []
    for binary in sorted(captures.glob("*.bin")):
        metadata = json.loads(binary.with_suffix(".json").read_text("utf-8"))
        report = decode(binary.read_bytes(), metadata)
        report["allocationQuery"] = metadata["allocationQuery"]
        binary.with_suffix(".duplicates.json").write_text(
            json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
        reports.append({key: value for key, value in report.items() if key != "witnesses"})
    execution["captures"] = reports
    (run_dir / "execution.json").write_text(json.dumps(execution, indent=2), encoding="utf-8")
    print(json.dumps({"directory": str(run_dir), **execution}, indent=2), flush=True)
    if len(reports) != 2 or not all(item["witnessesComplete"] for item in reports):
        raise RuntimeError("Expected exactly two complete actual diagnostic captures")


if __name__ == "__main__":
    main()

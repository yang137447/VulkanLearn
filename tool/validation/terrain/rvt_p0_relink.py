"""Relink frozen V1 objects with one telemetry object, without building live Store."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    project = Path(__file__).resolve().parents[3]
    build = project / "build-vt-release/source"
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    frozen = project / "artifacts/terrain-rvt-production/p0/baseline-source/source"
    source = project / "source/render/frame/renderFrameRecorder.cpp"
    snapshot = output / "renderFrameRecorder.cpp"
    shutil.copy2(source, snapshot)
    archive = output / "libvl_runtime.a"
    shutil.copy2(build / "libvl_runtime.a", archive)
    compiler = shutil.which("g++")
    archiver = shutil.which("ar")
    includes = (build / "CMakeFiles/vl_runtime.dir/includes_CXX.rsp").read_text()
    includes = includes.replace((project / "source").as_posix(), frozen.as_posix())
    (output / "includes.rsp").write_text(includes)
    libraries = (build / "CMakeFiles/main_game.dir/linkLibs.rsp").read_text()
    libraries = libraries.replace("libvl_runtime.a", '"' + archive.as_posix() + '"', 1)
    (output / "libraries.rsp").write_text(libraries)
    commands = [
        [compiler, "-DIMATH_DLL", "-DNDEBUG", "-DOPENEXR_DLL",
         "-DVULKANLEARN_ENABLE_VULKAN_VALIDATION", "@" + str(output / "includes.rsp"),
         "-O3", "-std=gnu++17", "-msse2", "-mfpmath=sse", "-c", str(snapshot),
         "-o", str(output / "renderFrameRecorder.cpp.obj")],
        [archiver, "r", str(archive), str(output / "renderFrameRecorder.cpp.obj")],
        [compiler, "-O3", "-DNDEBUG", "-Wl,--whole-archive",
         "CMakeFiles/main_game.dir/objects.a", "-Wl,--no-whole-archive",
         "-o", str(output / "main_game.exe"), "@" + str(output / "libraries.rsp")],
    ]
    manifest = {"commands": commands, "cwd": str(build),
                "frozenHeaders": str(frozen), "archiveBeforeSha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "telemetrySourceSha256": hashlib.sha256(snapshot.read_bytes()).hexdigest()}
    for command in commands:
        subprocess.run(command, cwd=build, check=True)
    manifest["exeSha256"] = hashlib.sha256((output / "main_game.exe").read_bytes()).hexdigest()
    (output / "build.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()

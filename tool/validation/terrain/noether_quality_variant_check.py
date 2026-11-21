"""Use the production CPU material/ShaderCompiler path; never create a Vulkan device."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[3]
    output = root / "artifacts/terrain-rvt-production/p3-noether-variant"
    output.mkdir(parents=True, exist_ok=True)
    build = root / "build-vt-release"
    includes = build / "tool/terrain-tests/CMakeFiles/terrain_tests.dir/includes_CXX.rsp"
    runtime_includes = build / "source/CMakeFiles/vl_runtime.dir/includes_CXX.rsp"
    frozen = root / "artifacts/terrain-rvt-production/p0/local-producer-link-r2/libvl_runtime.a"
    compiler = shutil.which("g++")
    if not compiler or not frozen.is_file():
        raise RuntimeError("Production CPU variant check requires MinGW and immutable P0 support")
    sources = [
        "source/shaderCompiler.cpp", "source/shaderCompilerApi.cpp",
        "source/shader/build/shaderBuildManifest.cpp", "source/shader/build/contentHash.cpp",
        "source/shader/shaderAbiSignature.cpp", "source/shaderReflect.cpp",
        "source/pipeline/shaderReflectionService.cpp",
        "source/material/loader/materialInstanceResolver.cpp",
        "source/material/validation/materialAssetValidator.cpp",
        "source/material/generator/materialParameterIncludeGenerator.cpp",
        "source/material/materialDescriptorSchema.cpp",
        "source/material/materialPipelineLayout.cpp",
        "source/materialInstanceValidator.cpp",
        "source/render/resource/terrainCompositedQualityReplay.cpp",
        "tool/validation/terrain/noether_quality_variant_tests.cpp",
    ]
    hashes = {name: digest(root / name) for name in sources}
    previous_path = output / "build.json"
    if not previous_path.exists():
        previous_path = output / "failed-build.json"
    previous = json.loads(previous_path.read_text("utf-8")) if previous_path.exists() else {}
    manifest = {"scope": "production CPU resolver/validator/compiler/reflection; no Vulkan instance/device/pipeline",
                "freshSourceSha256": hashes, "supportSha256": digest(frozen), "commands": [],
                "objectSha256": {}}
    (output / "checks.log").write_text("", encoding="utf-8")

    def run(command, cwd=root, environment=None):
        manifest["commands"].append(list(map(str, command)))
        result = subprocess.run(list(map(str, command)), cwd=cwd, env=environment,
                                capture_output=True, text=True)
        with (output / "checks.log").open("a", encoding="utf-8") as stream:
            stream.write(repr(command) + "\n" + result.stdout + result.stderr + "\n")
        print(result.stdout + result.stderr, end="", flush=True)
        if result.returncode:
            (output / "failed-build.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        result.check_returncode()

    objects = []
    for name in sources:
        target = output / (name.replace("/", "_") + ".obj")
        if not (target.exists() and previous.get("freshSourceSha256", {}).get(name) == hashes[name]
                and previous.get("objectSha256", {}).get(target.name) == digest(target)):
            print("Compiling actual production CPU source " + name, flush=True)
            run([compiler, "-std=gnu++17", "-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
                 "-ffunction-sections", "-fdata-sections", "@" + str(includes),
                 "@" + str(runtime_includes),
                 '-DVL_QUALITY_SOURCE_ROOT="' + root.as_posix() + '"',
                 '-DVL_QUALITY_TEST_OUTPUT="' + output.as_posix() + '"',
                 "-c", root / name, "-o", target])
        objects.append(target)
        manifest["objectSha256"][target.name] = digest(target)
    libraries = (build / "source/CMakeFiles/main_game.dir/linkLibs.rsp").read_text()
    libraries = libraries.replace("libvl_runtime.a", '"' + frozen.as_posix() + '"', 1)
    (output / "libraries.rsp").write_text(libraries, encoding="utf-8")
    executable = output / "quality_variant_tests.exe"
    run([compiler, *objects, build / "lib/libgtest_main.a", build / "lib/libgtest.a",
         "-Wl,--gc-sections", "-Wl,-Map=" + str(output / "link.map"),
         "-o", executable, "@" + str(output / "libraries.rsp")], cwd=build / "source")
    environment = os.environ.copy()
    environment["PATH"] = str(build / "bin") + ";" + environment["PATH"]
    run([executable, "--gtest_output=xml:" + str(output / "gtest.xml")], environment=environment)
    for enabled in (0, 1):
        run(["spirv-val", "--target-env", "vulkan1.4",
             output / f"production-macro-{enabled}.frag.spv"])
    if any(digest(root / name) != source_hash for name, source_hash in hashes.items()):
        raise RuntimeError("Production source changed during CPU variant compilation")
    manifest["exeSha256"] = digest(executable)
    (output / "build.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()

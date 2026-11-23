"""Compile/test Noether-owned files and shader variants without starting Vulkan."""

import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[3]
    output = root / "artifacts/terrain-rvt-production/p3-noether-cpu"
    output.mkdir(parents=True, exist_ok=True)
    build = root / "build-vt-release"
    commands = []
    owned_before = [
        *root.glob("source/render/resource/terrainCompositedQuality*"),
        *root.glob("shader/glsl/terrain/terrainCompositedQuality*"),
        *root.glob("tool/validation/terrain/noether_quality*"),
        root / "shader/glsl/pass/terrainFeedback.frag",
        root / "shader/glsl/pass/M_terrainFeedback.json",
    ]
    hashes_before = {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in owned_before if path.is_file()}

    def run(command):
        commands.append(list(map(str, command)))
        result = subprocess.run(list(map(str, command)), cwd=root, capture_output=True, text=True)
        with (output / "checks.log").open("a", encoding="utf-8") as stream:
            stream.write("\n" + repr(command) + "\n" + result.stdout + result.stderr)
        if str(command[0]) != "spirv-dis":
            print(result.stdout + result.stderr, end="", flush=True)
        result.check_returncode()
        return result.stdout

    (output / "checks.log").write_text("", encoding="utf-8")
    compiler = shutil.which("g++")
    def require(condition):
        if not condition:
            raise RuntimeError("CPU build/reflection check failed")
    require(compiler and shutil.which("glslangValidator") and shutil.which("spirv-val"))
    includes = build / "source/CMakeFiles/vl_runtime.dir/includes_CXX.rsp"
    test_includes = build / "tool/terrain-tests/CMakeFiles/terrain_tests.dir/includes_CXX.rsp"
    run([compiler, "-std=gnu++17", "-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
         "@" + str(includes), "-c", root / "source/render/resource/terrainCompositedQualityGpu.cpp",
         "-o", output / "quality.obj"])
    for name in (
        "source/render/frame/terrainCompositedQualityFrame.cpp",
        "source/render/pass/terrainQualityCapturePass.cpp",
        "source/render/frame/renderFrameRecorder.cpp",
    ):
        run([compiler, "-std=gnu++17", "-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
             "@" + str(includes), "-c", root / name, "-o", output / (Path(name).stem + ".obj")])
    run([compiler, "-std=gnu++17", "-O0", "@" + str(test_includes),
         root / "tool/validation/terrain/noether_quality_cpu_tests.cpp",
         build / "lib/libgtest_main.a", build / "lib/libgtest.a", "-pthread",
         "-o", output / "quality_cpu_tests.exe"])
    run([output / "quality_cpu_tests.exe", "--gtest_output=xml:" + str(output / "gtest.xml")])
    run([compiler, "-std=gnu++17", "-O0", "@" + str(test_includes),
         "@" + str(includes), root / "tool/validation/terrain/noether_quality_depth_tests.cpp",
         build / "lib/libgtest_main.a", build / "lib/libgtest.a", "-pthread",
         "-o", output / "quality_depth_tests.exe"])
    run([output / "quality_depth_tests.exe", "--gtest_output=xml:" + str(output / "depth-gtest.xml")])
    run(["py", "-3", "-m", "unittest", "discover", "-s", str(root / "tool/validation/terrain"),
         "-p", "noether_quality_decode_test.py", "-v"])
    for stage in ("vert", "frag"):
        source = root / f"shader/glsl/pass/terrainFeedback.{stage}"
        content = source.read_text("utf-8").replace(
            "#version 450", "#version 450\n#extension GL_GOOGLE_include_directive : require", 1)
        (output / f"feedback.{stage}").write_text(content, encoding="utf-8")
    for capture in (0, 1):
        target = output / f"feedback-capture-{capture}.spv"
        run(["glslangValidator", "-V", "--target-env", "vulkan1.2", "-Od", "-l",
             "-I" + str(root / "shader/glsl/pass"), f"-DVL_TERRAIN_QUALITY_CAPTURE={capture}",
             output / "feedback.vert", output / "feedback.frag", "-o", target])
        run(["spirv-val", "--target-env", "vulkan1.2", target])
        assembly = run(["spirv-dis", target])
        bindings, sets = {}, {}
        for identifier, kind, value in re.findall(r"OpDecorate\s+(%\S+)\s+(Binding|DescriptorSet)\s+(\d+)",
                                                 assembly):
            (bindings if kind == "Binding" else sets)[identifier] = int(value)
        reflected = sorted((sets[identifier], binding, identifier) for identifier, binding
                           in bindings.items() if identifier in sets)
        (output / f"feedback-capture-{capture}-bindings.json").write_text(
            json.dumps(reflected, indent=2), encoding="utf-8")
        actual = {binding for descriptor_set, binding, _ in reflected if descriptor_set == 3}
        require({0, 4}.issubset(actual))
        require(({5, 6}.issubset(actual)) if capture else not ({5, 6} & actual))
        if capture:
            for binding, storage in ((5, "StorageBuffer"), (6, "UniformConstant")):
                identifier = next(identifier for descriptor_set, slot, identifier in reflected
                                  if descriptor_set == 3 and slot == binding)
                require(re.search(r"^\s*" + re.escape(identifier) +
                                  r"\s*=\s*OpVariable\s+%\S+\s+" + storage + r"\b",
                                  assembly, re.MULTILINE))
    compute = output / "quality.spv"
    run(["glslangValidator", "-V", "--target-env", "vulkan1.2",
         root / "shader/glsl/terrain/terrainCompositedQuality.comp", "-o", compute])
    run(["spirv-val", "--target-env", "vulkan1.2", compute])
    owned = [
        "source/render/resource/terrainCompositedQualityData.h",
        "source/render/resource/terrainCompositedQualityGpu.h",
        "source/render/resource/terrainCompositedQualityGpu.cpp",
        "shader/glsl/terrain/terrainCompositedQualityVisible.glsl",
        "shader/glsl/terrain/terrainCompositedQuality.comp",
        "shader/glsl/pass/terrainFeedback.frag", "shader/glsl/pass/M_terrainFeedback.json",
        "tool/validation/terrain/noether_quality_decode.py",
        "tool/validation/terrain/noether_quality_decode_test.py",
        "tool/validation/terrain/noether_quality_cpu_tests.cpp",
        "tool/validation/terrain/noether_quality_cpu_check.py",
    ]
    manifest = {"scope": "CPU tests and offline shader compilation/linking only; no Vulkan execution",
                "commands": commands,
                "sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in owned}}
    require(all(hashlib.sha256((root / name).read_bytes()).hexdigest() == digest
                for name, digest in hashes_before.items()))
    manifest["allOwnedSourceSha256"] = hashes_before
    (output / "build.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()

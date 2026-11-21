import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--reuse-fresh-objects", action="store_true")
    parser.add_argument("--cpu-tests", action="store_true")
    parser.add_argument("--large-atlas", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = root / "artifacts/terrain-rvt-production/p1-gpu"
    output.mkdir(parents=True, exist_ok=True)
    build = root / "build-vt-release/source"
    frozen = root / "artifacts/terrain-rvt-production/p0/local-producer-link-r2/libvl_runtime.a"
    compiler = shutil.which("g++")
    archiver = shutil.which("ar")
    if not compiler or not archiver:
        raise RuntimeError("MinGW g++ and ar are required")
    includes = build / "CMakeFiles/vl_runtime.dir/includes_CXX.rsp"
    shutil.copy2(includes, output / "includes.rsp")
    support = output / "frozen-support.a"
    if not support.exists():
        shutil.copy2(frozen, support)
    sources = [
        "source/render/resource/terrainCompositedProductionGpu.cpp",
        "source/terrain/terrainCompositedProduction.cpp",
        "source/terrain/terrainCompositedVTStore.cpp",
        "source/terrain/terrainCompositedVT.cpp",
        "source/terrain/terrainCompositedAdjacency.cpp",
        "source/terrain/terrainCompositedDependencies.cpp",
        "source/terrain/terrainRvtBudget.cpp",
        "source/resource/device/deviceTextureFactory.cpp",
        "source/resource/image/hostImage.cpp",
        "source/texture.cpp",
        "source/render/backend/rendererBackendVulkanResources.cpp",
        "source/shader/build/contentHash.cpp",
        "source/shader/build/atomicFile.cpp",
        "tool/validation/terrain/rvt_production_gpu_check.cpp",
    ]
    previous_path = output / "build.json"
    previous = json.loads(previous_path.read_text()) if previous_path.exists() else {}
    manifest = {"frozenSupport": str(frozen), "frozenSupportSha256": digest(support),
                "freshSources": {}, "commands": [], "scope": "no writes to Euclid build; current ProductionGpu and direct data/upload dependencies"}
    manifest["productionGpuHeaderSha256"] = digest(root / "source/render/resource/terrainCompositedProductionGpu.h")
    manifest["freshObjectSha256"] = {}
    objects = []
    for name in sources:
        source = root / name
        target = output / (source.name + ".obj")
        manifest["freshSources"][name] = digest(source)
        command = [compiler, "-O2", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
                   "-DVULKANLEARN_ENABLE_VULKAN_VALIDATION", "-std=gnu++17",
                   "-ffunction-sections", "-fdata-sections", "@" + str(output / "includes.rsp"),
                   "-c", str(source), "-o", str(target)]
        manifest["commands"].append(command)
        if (options.reuse_fresh_objects and target.exists()
                and previous.get("freshSources", {}).get(name) == digest(source)
                and previous.get("freshObjectSha256", {}).get(target.name) == digest(target)
                and (source.name not in ("terrainCompositedProductionGpu.cpp", "rvt_production_gpu_check.cpp")
                     or previous.get("productionGpuHeaderSha256") == manifest["productionGpuHeaderSha256"])):
            print("Reusing this harness's fresh object: " + name, flush=True)
        else:
            print("Compiling " + name, flush=True)
            subprocess.run(command, cwd=root, check=True)
        if digest(source) != manifest["freshSources"][name]:
            raise RuntimeError("Source changed while compiling: " + name)
        objects.append(target)
        manifest["freshObjectSha256"][target.name] = digest(target)
        (output / "build.json").write_text(json.dumps(manifest, indent=2))
    libraries = (build / "CMakeFiles/main_game.dir/linkLibs.rsp").read_text()
    libraries = libraries.replace("libvl_runtime.a", '"' + support.as_posix() + '"', 1)
    (output / "libraries.rsp").write_text(libraries)
    executable = output / "rvt_production_gpu_check.exe"
    link_command = [compiler, *map(str, objects), "-Wl,--gc-sections",
               "-Wl,-Map=" + str(output / "link.map"), "-o", str(executable),
               "@" + str(output / "libraries.rsp")]
    manifest["commands"].append(link_command)
    (output / "build.json").write_text(json.dumps(manifest, indent=2))
    if options.cpu_tests:
        cpu_dir = output / "cpu-chunk"
        cpu_dir.mkdir(exist_ok=True)
        test_source = root / "tool/terrain-tests/terrainCompositedProductionGpuTests.cpp"
        test_object = cpu_dir / "tests.obj"
        command = [compiler, "-O2", "-DNDEBUG", "-std=gnu++17",
                   "@" + str(output / "includes.rsp"),
                   "-ID:/YYBWorkSpace/VulkanLearn-deps/_sources/googletest/googletest/include",
                   "-c", str(test_source), "-o", str(test_object)]
        subprocess.run(command, cwd=root, check=True)
        cpu_sources = ["terrainCompositedProduction.cpp.obj", "terrainCompositedAdjacency.cpp.obj",
                       "terrainRvtBudget.cpp.obj", "hostImage.cpp.obj", "contentHash.cpp.obj",
                       "atomicFile.cpp.obj"]
        cpu_executable = cpu_dir / "production_gpu_cpu_tests.exe"
        command = [compiler, str(test_object), *[str(output / name) for name in cpu_sources],
                   str(root / "build-vt-release/lib/libgtest_main.a"),
                   str(root / "build-vt-release/lib/libgtest.a"),
                   str(root / "build-vt-release/extern/BLAKE3/libblake3.a"),
                   "-o", str(cpu_executable)]
        subprocess.run(command, cwd=root, check=True)
        result = subprocess.run([str(cpu_executable), "--gtest_output=xml:" + str(cpu_dir / "results.xml")],
                                cwd=root, capture_output=True, text=True)
        (cpu_dir / "tests.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        (cpu_dir / "build.json").write_text(json.dumps({
            "testSourceSha256": digest(test_source), "testObjectSha256": digest(test_object),
            "exeSha256": digest(cpu_executable), "implementationBuild": manifest}, indent=2))
        print(result.stdout)
        print(result.stderr)
        result.check_returncode()
    subprocess.run(link_command, cwd=build, check=True)
    manifest["exeSha256"] = digest(executable)
    manifest["freshObjectSha256"] = {path.name: digest(path) for path in objects}
    (output / "build.json").write_text(json.dumps(manifest, indent=2))
    if options.run:
        import os
        runs = output / "runs"
        runs.mkdir(exist_ok=True)
        run_dir = runs / ("run-" + str(len(list(runs.iterdir())) + 1).zfill(3))
        run_dir.mkdir()
        for path in (executable, output / "build.json", output / "link.map",
                     Path(__file__), root / "tool/validation/terrain/rvt_production_gpu_check.cpp"):
            shutil.copy2(path, run_dir / path.name)
        environment = os.environ.copy()
        environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
        arguments = [str(run_dir / executable.name), str(run_dir / "report.json")]
        if options.large_atlas:
            arguments.append("--large-atlas")
        result = subprocess.run(arguments,
                                cwd=root, env=environment, capture_output=True, text=True)
        (run_dir / "run.stdout.log").write_text(result.stdout, encoding="utf-8")
        (run_dir / "run.stderr.log").write_text(result.stderr, encoding="utf-8")
        (output / "latest-run.txt").write_text(str(run_dir))
        print(result.stdout)
        print(result.stderr)
        result.check_returncode()


if __name__ == "__main__":
    main()

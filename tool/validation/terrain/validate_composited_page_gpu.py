import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--execute-gpu", action="store_true")
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--reuse-objects", action="store_true")
    parser.add_argument("--validation-off", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    build = root / "build-vt-release/source"
    if not options.compiler:
        parser.error("MinGW compiler required")
    private = output / "project"
    (private / "config").mkdir(parents=True, exist_ok=True)
    (private / "resources").mkdir(exist_ok=True)
    shaders = private / "shader/spv/terrain"
    shaders.mkdir(parents=True, exist_ok=True)
    config = json.loads((root / "config/config.json").read_text(encoding="utf-8-sig"))
    config["resourcePath"] = str(private / "resources")
    (private / "config/config.json").write_text(json.dumps(config), encoding="utf-8")
    manifest = {"status": "building", "freshSources": {}, "commands": [],
                "runtimeArchive": str(build / "libvl_runtime.a")}
    previous_path = output / "build.json"
    previous = json.loads(previous_path.read_text()) if previous_path.exists() else {}

    def run(command, cwd=root):
        manifest["commands"].append([str(part) for part in command])
        subprocess.run([str(part) for part in command], cwd=cwd, check=True)

    for name in ("terrainCompositedBakeWorld", "terrainCompositedBakeWorldPadding"):
        source = root / f"shader/glsl/terrain/{name}.comp"
        manifest["freshSources"][str(source.relative_to(root))] = digest(source)
        target = shaders / f"{name}_comp.spv"
        run(["glslc", "-O", "--target-env=vulkan1.4", source, "-o", target])
        run(["glslc", "-O", "-g", "--target-env=vulkan1.4",
             source, "-o", shaders / f"{name}_comp.debug"])
        run(["spirv-val", "--target-env", "vulkan1.4", target])
        run(["spirv-val", "--target-env", "vulkan1.4", shaders / f"{name}_comp.debug"])
    manifest["shaderPolicy"] = "shaderc performance optimization, Vulkan1.4, separate runtime/debug SPV, matching ShaderCompiler CompileGlsl"
    for name in ("composited_page_fixture.h", "composited_page_reference.h", "composited_adjacency_cases.h"):
        source = root / "tool/validation/terrain" / name
        manifest["freshSources"][str(source.relative_to(root))] = digest(source)
    sources = [
        "source/render/resource/terrainCompositedBakeGpu.cpp",
        "source/terrain/terrainCompositedAdjacency.cpp",
        "tool/validation/terrain/composited_page_gpu_check.cpp",
    ]
    for name in ("terrainCompositedBakeGpu.h", "terrainCompositedBakeAdjacency.h"):
        source = root / "source/render/resource" / name
        manifest["freshSources"][str(source.relative_to(root))] = digest(source)
    source = root / "shader/glsl/terrain/terrainCompositedBakeRoute.glsl"
    manifest["freshSources"][str(source.relative_to(root))] = digest(source)
    for name in ("terrainCompositedBakeWorldEvaluator.glsl", "terrainCompositedBakeWorldTypes.glsl"):
        source = root / "shader/glsl/terrain" / name
        manifest["freshSources"][str(source.relative_to(root))] = digest(source)
    objects = []
    for name in sources:
        source = root / name
        target = output / (source.name + ".obj")
        manifest["freshSources"][name] = digest(source)
        if not (options.reuse_objects and target.exists() and
                previous.get("freshSources", {}).get(name) == digest(source) and
                all(previous.get("freshSources", {}).get(header) == value
                    for header, value in manifest["freshSources"].items() if header.endswith(".h"))):
            run([options.compiler, "-O2", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
                 "-DVULKANLEARN_ENABLE_VULKAN_VALIDATION", "-std=gnu++17", "-ffp-contract=off",
                 "-ffunction-sections", "-fdata-sections",
                 "@" + str(build / "CMakeFiles/vl_runtime.dir/includes_CXX.rsp"),
                 "-c", source, "-o", target], build)
        objects.append(target)
    library = output / "current-runtime.a"
    source_library = build / "libvl_runtime.a"
    before = digest(source_library)
    shutil.copy2(source_library, library)
    if before != digest(library) or before != digest(source_library):
        raise RuntimeError("Current runtime library changed while copying; retry after owner completes")
    manifest["runtimeArchiveSha256"] = before
    libraries = (build / "CMakeFiles/main_game.dir/linkLibs.rsp").read_text()
    libraries = libraries.replace("libvl_runtime.a", '"' + library.as_posix() + '"', 1)
    response = output / "libraries.rsp"
    response.write_text(libraries)
    executable = output / "actualBakerPageCheck.exe"
    run([options.compiler, *objects, "-Wl,--gc-sections", "-Wl,-Map=" + str(output / "link.map"),
         "-o", executable, "@" + str(response)], build)
    manifest["executableSha256"] = digest(executable)
    manifest["status"] = "prepared-not-executed"
    (output / "build.json").write_text(json.dumps(manifest, indent=2))
    if options.execute_gpu:
        runs = output / "runs"
        runs.mkdir(exist_ok=True)
        run_dir = runs / f"run-{len(list(runs.iterdir())) + 1:03}"
        run_dir.mkdir()
        shutil.copy2(executable, run_dir / executable.name)
        shutil.copytree(private / "shader", run_dir / "shader")
        shutil.copy2(output / "link.map", run_dir / "link.map")
        for name in manifest["freshSources"]:
            source = root / name
            target = run_dir / "sources" / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        environment = os.environ.copy()
        environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
        command = [str(run_dir / executable.name), str(run_dir)]
        if options.validation_off:
            command.append("--validation-off")
        result = subprocess.run(command, cwd=private,
                                env=environment, capture_output=True, text=True, errors="replace")
        (run_dir / "stdout.log").write_text(result.stdout, encoding="utf-8")
        (run_dir / "stderr.log").write_text(result.stderr, encoding="utf-8")
        shutil.copy2(output / "build.json", run_dir / "build.json")
        print(result.stdout[-10000:])
        print(result.stderr[-10000:])
        result.check_returncode()


if __name__ == "__main__":
    main()

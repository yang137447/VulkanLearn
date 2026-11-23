"""Prepare a real Game with seven feedback MRTs without executing Vulkan."""

from concurrent.futures import ThreadPoolExecutor
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
    output = root / "artifacts/terrain-rvt-production/p3-quality-winner"
    output.mkdir(parents=True, exist_ok=True)
    stage = output / "stage"
    native = Path(os.environ["TEMP"]) / "vulkanlearn-noether-quality"
    baseline = root / "artifacts/terrain-rvt-production/p3-noether-game-min"
    if not stage.exists():
        shutil.copytree(baseline / "stage", stage)
    shaders = ["pass/terrainFeedback.frag", "pass/M_terrainFeedback.json",
               "terrain/terrainCompositedQuality.comp"]
    for name in shaders:
        shutil.copy2(root / "shader/glsl" / name, stage / "shader/glsl" / name)
    graph_file = stage / "config/renderGraphConfig.json"
    graph = json.loads(graph_file.read_text("utf-8"))
    source_graph = json.loads((root / "config/renderGraphConfig.json").read_text("utf-8"))
    names = {"terrainQualityWinner0", "terrainQualityWinner1"}
    graph["resources"] = [item for item in graph["resources"] if item["name"] not in names]
    graph["resources"].extend(item for item in source_graph["resources"] if item["name"] in names)
    feedback = next(item for item in graph["passes"] if item["type"] == "terrainFeedback")
    feedback["output"] = [item for item in feedback["output"] if item["resource"] not in names]
    feedback["output"][-1:-1] = [
        {"resource": name, "loadOp": "clear", "storeOp": "store"} for name in sorted(names)]
    graph_file.write_text(json.dumps(graph, indent=2) + "\n", encoding="utf-8")
    commands = []

    def run(command, label):
        command = list(map(str, command))
        commands.append(command)
        result = subprocess.run(command, cwd=root, capture_output=True, text=True)
        (output / (label + ".log")).write_text(
            repr(command) + "\n" + result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            print(label + ": " + result.stdout + result.stderr, flush=True)
        result.check_returncode()

    for shader_stage in ("vert", "frag"):
        source = stage / f"shader/glsl/pass/terrainFeedback.{shader_stage}"
        text = source.read_text("utf-8").replace(
            "#version 450", "#version 450\n#extension GL_GOOGLE_include_directive : require", 1)
        (output / f"feedback.{shader_stage}").write_text(text, encoding="utf-8")
    run(["glslangValidator", "-V", "--target-env", "vulkan1.2", "-l",
         "-I" + str(stage / "shader/glsl/pass"), output / "feedback.vert",
         output / "feedback.frag", "-o", output / "winner.spv"], "winner-compile")
    run(["spirv-val", "--target-env", "vulkan1.2", output / "winner.spv"], "winner-validate")
    run(["glslangValidator", "-V", "--target-env", "vulkan1.2",
         stage / "shader/glsl/terrain/terrainCompositedQuality.comp",
         "-o", output / "resolve.spv"], "resolve-compile")
    run(["spirv-val", "--target-env", "vulkan1.2", output / "resolve.spv"], "resolve-validate")
    sources = [
        "source/main_game.cpp",
        "source/render/resource/terrainCompositedQualityGpu.cpp",
        "source/render/frame/terrainCompositedQualityFrame.cpp",
        "source/render/rendergraph/renderGraphCompiler.cpp",
        "source/render/rendergraph/renderGraphCompilerValidation.cpp",
        "source/renderGraphFormats.cpp", "source/renderGraphPassResources.cpp",
    ]
    headers = ["source/render/resource/terrainCompositedQualityGpu.h",
               "source/render/resource/terrainCompositedQualityData.h",
               "source/render/pass/passRuntime.h"]
    hashes = {name: digest(root / name) for name in sources + headers}
    includes = native / "run006-coherent/includes.rsp"

    def compile_source(name):
        print("Compiling " + name, flush=True)
        target = output / (Path(name).stem + ".obj")
        run(["g++", "-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL", "-std=gnu++17",
             "-ffunction-sections", "-fdata-sections", "@" + str(includes),
             "-c", root / name, "-o", target], Path(name).stem)
        return target

    with ThreadPoolExecutor(max_workers=2) as pool:
        objects = list(pool.map(compile_source, sources))
    launch = json.loads((baseline / "run-20261121T035333501660Z/launch.json").read_text("utf-8"))
    overlays = [Path(launch["deltaObject"]),
                *(Path(item["path"]) for item in launch["requiredObjects"])]
    for path, expected in zip(overlays, [launch["deltaObjectSha256"],
                                         *(item["sha256"] for item in launch["requiredObjects"])]):
        if digest(path) != expected:
            raise RuntimeError("Successful Game overlay changed: " + str(path))
    run(["g++", *objects, *overlays, "-Wl,--gc-sections", "-Wl,-Map=" + str(output / "game.map"),
         "-o", stage / "bin/main_game.exe",
         "@" + str(native / "run006-coherent/libraries.rsp")], "link")
    if any(digest(root / name) != expected for name, expected in hashes.items()):
        raise RuntimeError("Source changed while compiling")
    manifest = {"gpuExecuted": False, "sourceSha256": hashes, "commands": commands,
                "executable": str(stage / "bin/main_game.exe"),
                "executableSha256": digest(stage / "bin/main_game.exe"),
                "overlaySha256": {str(path): digest(path) for path in overlays},
                "graphSha256": digest(graph_file), "shaderSha256": {
                    name: digest(stage / "shader/glsl" / name) for name in shaders},
                "sharedBuildWritten": False, "cpuBuildOutput": str(output),
                "winnerABI": "loc5 UV2+major2; loc6 target,bias,surface|taps,depthBits|0x80000000",
                "snapshotABIChanged": False, "captureReplayRemoved": True,
                "requiresAssignedGpuWindow": True}
    (output / "prepared.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("Prepared: " + str(output / "prepared.json"), flush=True)


if __name__ == "__main__":
    main()

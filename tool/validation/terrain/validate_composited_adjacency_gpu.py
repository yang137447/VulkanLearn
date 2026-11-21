import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def run(arguments, root):
    result = subprocess.run(
        [str(argument) for argument in arguments], cwd=root,
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(result.stdout, end="")
    print(result.stderr, end="")
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {arguments}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--validator", default=shutil.which("glslangValidator"))
    parser.add_argument("--spirv-val", default=shutil.which("spirv-val"))
    parser.add_argument("--execute-gpu", action="store_true")
    options = parser.parse_args()
    if not all((options.compiler, options.validator, options.spirv_val)):
        parser.error("MinGW g++, glslangValidator and spirv-val are required")
    root = Path(__file__).resolve().parents[3]
    here = Path(__file__).resolve().parent
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    shaders = {
        "route": here / "composited_adjacency_route.comp",
        "address": root / "shader/glsl/terrain/terrainCompositedBakeAddress.comp",
        "bake": root / "shader/glsl/terrain/terrainCompositedBake.comp",
    }
    for name, source in shaders.items():
        destination = output / f"{name}.spv"
        run([options.validator, "-V", "--target-env", "vulkan1.2",
             source, "-o", destination], root)
        run([options.spirv_val, "--target-env", "vulkan1.2", destination], root)
    executable = output / "adjacencyGpuCheck.exe"
    run([options.compiler, "-std=c++17", "-O2", "-ffp-contract=off",
         "-Isource", "-Iextern/VulkanSDK/include",
         here / "composited_adjacency_gpu_check.cpp",
         root / "source/terrain/terrainCompositedAdjacency.cpp",
         "-Lextern/VulkanSDK/lib/Win32", "-lvulkan-1", "-o", executable], root)
    inputs = [
        *shaders.values(), here / "composited_adjacency_gpu_check.cpp",
        here / "composited_adjacency_cases.h",
        root / "shader/glsl/terrain/terrainCompositedBakeRoute.glsl",
        root / "source/render/resource/terrainCompositedBakeAdjacency.h",
        root / "source/terrain/terrainCompositedAdjacency.h",
        root / "source/terrain/terrainCompositedAdjacency.cpp",
    ]
    command = [str(executable), str(output / "route.spv"), str(output / "address.spv")]
    report = {
        "status": "prepared-not-executed",
        "gpuExecuted": False,
        "performanceMeasurement": False,
        "oracle": "TerrainCompositedAdjacencyGraph::Resolve, double, exact surfaceId comparison",
        "scope": [
            "Full16 connected grid and explicitly disconnected surface",
            "all fixture sources, exact boundary, signed zero, adjacent representable floats",
            "partial edges, nonunit Jacobians, rotated and reflected UV charts",
            "stored136 UVs at mip extents 1,2,7,128,8192",
            "production Address.comp packed routes at extent1, samples4 and samples8",
        ],
        "command": command,
        "sourceSha256": {
            str(path.relative_to(root)).replace("\\", "/"):
                hashlib.sha256(path.read_bytes()).hexdigest()
            for path in inputs
        },
        "artifactSha256": {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in [executable, *(output / f"{name}.spv" for name in shaders)]
        },
    }
    if options.execute_gpu:
        report["gpuExecuted"] = True
        report["status"] = "running"
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        try:
            stdout = run(command, root)
            (output / "gpu-stdout.txt").write_text(stdout, encoding="utf-8")
            report["measurement"] = json.loads(stdout.splitlines()[-1])
            report["status"] = "passed"
        except Exception:
            report["status"] = "failed"
            (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            raise
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"status": report["status"], "report": str(output / "report.json")}))


if __name__ == "__main__":
    main()

"""Compile actual production Validate and dispatch center/area reconstruction probes."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import struct
import subprocess

from PIL import Image

from validate_composited_bake_gpu import run


TEXELS = 136 * 136
CHANNEL_BYTES = TEXELS * 4


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare(first, second):
    maximum = total = count = 0
    errors = []
    for row in range(5, 131):
        for column in range(5, 131):
            offset = (row * 136 + column) * 4
            for component in range(3):
                error = abs(first[offset + component] - second[offset + component])
                maximum = max(maximum, error)
                total += error
                count += 1
                errors.append(error)
    errors.sort()
    return {"maxByteError": maximum, "meanByteError": total / count,
            "p95ByteError": errors[(len(errors) * 95 + 99) // 100 - 1],
            "pixels": 126 * 126, "passedTwoByteGate": maximum <= 2}


def gradient_energy(pixels):
    total = count = 0
    for row in range(6, 130):
        for column in range(6, 130):
            offset = (row * 136 + column) * 4
            for component in range(3):
                total += abs(pixels[offset + component] - pixels[offset - 4 + component])
                total += abs(pixels[offset + component] - pixels[offset - 136 * 4 + component])
                count += 2
    return total / count


def actual_validate_unit(root, output):
    production = (root / "source/render/resource/terrainCompositedBakeGpu.cpp").read_text(encoding="utf-8")
    helpers_start = production.index("uint32_t MipExtent(")
    helpers_end = production.index("\n}\n\nstruct TerrainCompositedBakeGpu::GpuParams")
    start = production.index("bool TerrainCompositedBakeGpu::Validate(")
    body = production.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (production[end] == "{") - (production[end] == "}")
        end += 1
    generated = output / "actual-production-validate.cpp"
    generated.write_text(
        '#include "render/resource/terrainCompositedBakeGpu.h"\n#include <algorithm>\n#include <cmath>\n'
        "namespace VL { namespace { using Baker=TerrainCompositedBakeGpu;\n" +
        production[helpers_start:helpers_end] + "\n}\n" + production[start:end] + "\n}\n",
        encoding="utf-8")
    return generated


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=Path("build-vt-release"))
    parser.add_argument("--prepare-only", action="store_true",
                        help="Compile production/CPU tests and GPU shaders, but do not dispatch GPU work")
    parser.add_argument("--run-prepared", action="store_true",
                        help="Dispatch a previously prepared unchanged test after Cook releases the GPU")
    options = parser.parse_args()
    if options.prepare_only and options.run_prepared:
        parser.error("--prepare-only and --run-prepared are mutually exclusive")
    root = Path(__file__).resolve().parents[3]
    here = Path(__file__).resolve().parent
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=options.run_prepared)
    log = json.loads((output / "commands.json").read_text()) if options.run_prepared else []
    prepared_sources = (
        root / "source/render/resource/terrainCompositedBakeGpu.cpp",
        root / "source/render/resource/terrainCompositedBakeGpu.h",
        root / "shader/glsl/terrain/terrainCompositedBake.comp",
        root / "shader/glsl/materialFunction/terrainCompositedEvaluation.glsl",
        here / "composited_bake_gpu_check.cpp", here / "composited_center_validate.cpp",
        here / "composited_center_reconstruction.comp", Path(__file__))
    if options.run_prepared:
        prepared = json.loads((output / "prepared.json").read_text(encoding="utf-8"))
        if any(digest(root / name) != value for name, value in prepared["sourceSha256"].items()):
            raise ValueError("Prepared test source changed; recompile before dispatch")
        if any(digest(output / name) != value for name, value in prepared["binarySha256"].items()):
            raise ValueError("Prepared executable/shader changed")
    def execute(arguments, cwd=root):
        text = run(arguments, cwd)
        log.append({"argv": [str(value) for value in arguments], "cwd": str(cwd),
                    "stdout": text, "exitCode": 0})
        (output / "commands.json").write_text(json.dumps(log, indent=2) + "\n", encoding="utf-8")
        return text
    compile_commands = json.loads((root / options.build / "compile_commands.json").read_text(encoding="utf-8"))
    entry = next(item for item in compile_commands if item["file"].replace("\\", "/").endswith(
        "/source/render/resource/terrainCompositedBakeGpu.cpp"))
    arguments = entry.get("arguments") or [
        value.strip('"') for value in shlex.split(entry["command"], posix=False)]
    arguments = list(arguments)
    object_path = output / "actual-production-baker.o"
    output_index = arguments.index("-o") + 1
    arguments[output_index] = str(object_path)
    arguments.extend(["-ffunction-sections", "-fdata-sections"])
    if not options.run_prepared:
        execute(arguments, Path(entry["directory"]))
    compiler = arguments[0]
    cpu_test = output / "actual-validate.exe"
    if not options.run_prepared:
        execute([compiler, "-std=gnu++17", "-O2", "-ffunction-sections", "-fdata-sections",
                 "-Isource", "-Iextern/VulkanSDK/include", here / "composited_center_validate.cpp",
                 actual_validate_unit(root, output), "-Lextern/VulkanSDK/lib/Win32", "-lvulkan-1",
                 "-o", cpu_test])
    cpu_result = execute([cpu_test])
    gpu_test = output / "raw-check.exe"
    if not options.run_prepared:
        execute([compiler, "-std=gnu++17", "-O2", "-Iextern/VulkanSDK/include",
                 here / "composited_bake_gpu_check.cpp", "-Lextern/VulkanSDK/lib/Win32",
                 "-lvulkan-1", "-o", gpu_test])
    validator = shutil.which("glslangValidator")
    bake = output / "production-bake.spv"
    check = output / "center-reconstruction.spv"
    if not options.run_prepared:
        execute([validator, "-V", "--target-env", "vulkan1.2",
                 root / "shader/glsl/terrain/terrainCompositedBake.comp", "-o", bake])
        execute([validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl",
                 here / "composited_center_reconstruction.comp", "-o", check])
    resource = root.parent / "VukanLearnResources"
    source_path = resource / "Generated/HoudiniPegasusIsland/tile_00/Textures/Source/layer_00_albedo.png"
    source = Image.open(source_path).convert("RGBA")
    if source.size != (1024, 1024):
        raise ValueError("Expected real 1024x1024 Gravel Ground source")
    source = source.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    (output / "source-albedo.bin").write_bytes(source.tobytes() * 2)
    if options.prepare_only:
        prepared = {
            "status": "cpu-and-shaders-passed-awaiting-gpu-release",
            "gpuDispatched": False, "cpuActualProductionValidate": cpu_result,
            "sourceSha256": {path.relative_to(root).as_posix(): digest(path) for path in prepared_sources},
            "binarySha256": {path.name: digest(path) for path in (cpu_test, gpu_test, bake, check)},
        }
        (output / "prepared.json").write_text(json.dumps(prepared, indent=2) + "\n", encoding="utf-8")
        print(f"CPU/GLSL compiled, no GPU dispatch: {output / 'prepared.json'}")
        return 0
    modes = ("center", "area", "mip1", "mip2", "mip3")
    pages = []
    for mode in modes:
        target = output / f"bake-{mode}.bin"
        execute([gpu_test, bake, f"bounded-reconstruction-{mode}", target, output])
        pages.append(target.read_bytes()[:CHANNEL_BYTES])
    (output / "reconstruction-pages.bin").write_bytes(b"".join(pages))
    target = output / "reconstruction-readback.bin"
    execute([gpu_test, check, "bounded-reconstruction-check", target, output])
    data = target.read_bytes()
    if len(data) != 9 * CHANNEL_BYTES:
        raise ValueError("Incomplete GPU readback")
    planes = [data[index * CHANNEL_BYTES:(index + 1) * CHANNEL_BYTES] for index in range(9)]
    center = compare(planes[0], planes[1])
    old_area = compare(planes[2], planes[1])
    coarse = {f"mip{mip}": compare(planes[2*mip+1], planes[2*mip+2]) for mip in range(1, 4)}
    energy = {"center": gradient_energy(planes[0]), "source": gradient_energy(planes[1]),
              "oldArea": gradient_energy(planes[2])}
    negative_detected = old_area["maxByteError"] > 2 and energy["oldArea"] < energy["center"] * 0.9
    report = {
        "status": "passed" if center["passedTwoByteGate"] and
                  all(value["passedTwoByteGate"] for value in coarse.values()) and negative_detected else "failed",
        "cpuActualProductionValidate": cpu_result,
        "mip0CenterVsGpuDirectSource": center,
        "mip0OldAreaNegativeControlVsSource": old_area,
        "coarseArea4x4VsGpuSourceAreaReference": coarse,
        "negativeSharpnessControlDetected": negative_detected,
        "gradientEnergy": energy,
        "source": {"path": str(source_path), "sha256": digest(source_path),
                   "extent": [1024, 1024], "uvScale": 4,
                   "virtualMip0Extent": [4096, 4096], "sourceLayer": "real Gravel Ground",
                   "weight": "single known source layer to isolate reconstruction; spatially detailed actual texture, not constant colors"},
        "thresholds": {"maxReconstructionByteError": 2},
        "semantics": {"mip0": "one center sample", "mip1AndAbove": "4x4 area samples in tested mip1/2/3",
                      "PageRequestDefault": 4, "productionShaderModified": False},
        "limits": ["Raw GPU source and producer reconstruction only; no renderer/Cook run",
                   "Single-layer texture isolates filter; full mixed-layer/height/anisotropy ROI remains a separate unchanged gate",
                   "GPU direct source is an independent sampling reference here, not a second CPU material interpreter"],
        "sourceSha256": {path.relative_to(root).as_posix(): digest(path) for path in (
            root / "source/render/resource/terrainCompositedBakeGpu.cpp",
            root / "source/render/resource/terrainCompositedBakeGpu.h",
            root / "shader/glsl/terrain/terrainCompositedBake.comp",
            here / "composited_bake_gpu_check.cpp", here / "composited_center_validate.cpp",
            here / "composited_center_reconstruction.comp", Path(__file__))},
        "evidenceSha256": {path.name: digest(path) for path in output.iterdir() if path.is_file()},
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("status", "mip0CenterVsGpuDirectSource",
          "mip0OldAreaNegativeControlVsSource", "coarseArea4x4VsGpuSourceAreaReference",
          "gradientEnergy", "negativeSharpnessControlDetected")}, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(arguments, cwd):
    result = subprocess.run(
        [str(argument) for argument in arguments],
        cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    print(result.stdout, end="")
    print(result.stderr, end="")
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {arguments}")
    return result.stdout.strip()


def material_stages(root, output, validator, tessellation=True):
    schema = json.loads((root / "shader/glsl/M_terrain.json").read_text(encoding="utf-8"))
    common = [
        "#version 450",
        "#extension GL_GOOGLE_include_directive : require",
        "#define MATERIAL_WRITES_EVERY_PIXEL 1",
        "#define MATERIAL_USES_OPACITY_MASK 0",
        "#define MATERIAL_MODIFIES_MESH_POSITION 0",
        "#define MATERIAL_TWO_SIDED 0",
        "#define VL_LOCAL_SHADOW 0",
        "#define MATERIAL_SHADING_MODEL 1",
        f"#define VL_TERRAIN_TESSELLATION {int(tessellation)}",
        '#include "engine/materialContext.glsl"',
    ]
    for binding, (name, descriptor) in enumerate(schema["textures"].items(), 1):
        common.append(
            f'layout(set=1,binding={binding}) uniform {descriptor["type"]} {name};'
        )
    common.append("layout(std140,set=1,binding=0) uniform MaterialParams {")
    for name, descriptor in schema["parameters"].items():
        common.append(f'{descriptor["type"]} {name};')
    common.append("};")
    stages = []
    for stage in (("vert", "tesc", "tese", "frag") if tessellation else ("vert", "frag")):
        lines = list(common)
        if stage == "vert":
            lines.append('#include "M_terrain.vertex.glsl"')
        if stage == "frag":
            lines.extend((
                '#include "engine/materialSurface.glsl"',
                '#include "M_terrain.surface.glsl"',
            ))
        lines.append(f'#include "engine/passTemplate/base.{stage}.glsl"')
        path = output / f"terrain_full.{stage}"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        stages.append(path)
        run([validator, "-V", "--target-env", "vulkan1.2",
             "-Ishader/glsl", path, "-o", output / f"{stage}.spv"], root)
    run([validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl",
         "-l", *stages, "-o", output / "linked.spv"], root)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--validator", default=shutil.which("glslangValidator"))
    parser.add_argument("--output", type=Path)
    options = parser.parse_args()
    if not options.compiler or not options.validator:
        parser.error("MinGW g++ and glslangValidator must be available or explicitly supplied")
    root = Path(__file__).resolve().parents[3]
    output = (options.output or root / "artifacts/terrain/composited-gpu").resolve()
    output.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    report = {
        "heightBlend": {
            "strengths": [0, 3],
            "distancesM": [0, 7.999, 8, 8.001, 26.5, 44.999, 45, 45.001, 100],
            "cookFade": 1,
            "baseRoute": "composite-all-distance",
            "byteTolerance": 1,
        },
        "measurements": {},
    }
    report["sourceSha256"] = {
        str(path.relative_to(root)).replace("\\", "/"):
            hashlib.sha256(path.read_bytes()).hexdigest()
        for path in (
            root / "shader/glsl/M_terrain.json",
            root / "shader/glsl/terrain/terrainCompositedBake.comp",
            root / "shader/glsl/materialFunction/terrainCompositedEvaluation.glsl",
            root / "shader/glsl/materialFunction/mf_terrainLayerBlend.glsl",
            root / "shader/glsl/materialFunction/mf_terrainCompositedVT.glsl",
            here / "composited_height_distance.comp",
            here / "composited_bake_gpu_check.cpp",
        )
    }
    with tempfile.TemporaryDirectory(prefix="vl-composited-gpu-") as temporary:
        temporary = Path(temporary)
        executable = temporary / "check.exe"
        run([options.compiler, "-std=gnu++17", "-O2", "-Iextern/VulkanSDK/include",
             here / "composited_bake_gpu_check.cpp", "-Lextern/VulkanSDK/lib/Win32",
             "-lvulkan-1", "-o", executable], root)
        shader = temporary / "bake.spv"
        run([options.validator, "-V", "--target-env", "vulkan1.2",
             root / "shader/glsl/terrain/terrainCompositedBake.comp", "-o", shader], root)
        for mode in ("coverage", "repeat", "local", "height", "tail", "missing", "lod", "area"):
            report["measurements"][mode] = run([executable, shader, mode, output / f"{mode}.bin"], root)
        distance_shader = temporary / "distance.spv"
        run([options.validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl",
             here / "composited_height_distance.comp", "-o", distance_shader], root)
        report["measurements"]["distance"] = run(
            [executable, distance_shader, "distance", output / "distance.bin"], root)
        material_stages(root, temporary, options.validator)
        report["fullMaterialStages"] = "vert/tesc/tese/frag compile and link passed"
    report["status"] = "passed"
    report["readbackSha256"] = {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in output.glob("*.bin")
    }
    (output / "report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Report: {output / 'report.json'}")


if __name__ == "__main__":
    main()

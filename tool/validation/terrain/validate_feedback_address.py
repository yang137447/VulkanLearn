"""Check composite-only terrain shaders and optionally compile production stages."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from validate_composited_feedback import validate_shared_footprint
from validate_composited_bake_gpu import material_stages


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def strip_comments(source):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def validate_shaders(root):
    shader_root = root / "shader/glsl"
    blend = strip_comments((shader_root / "materialFunction/mf_terrainLayerBlend.glsl")
                           .read_text(encoding="utf-8"))
    feedback = strip_comments((shader_root / "pass/terrainFeedback.frag")
                              .read_text(encoding="utf-8"))
    evaluation = strip_comments((shader_root / "materialFunction/terrainCompositedEvaluation.glsl")
                                .read_text(encoding="utf-8"))
    material = json.loads((shader_root / "M_terrain.json").read_text(encoding="utf-8"))
    schema = json.loads((shader_root / "pass/M_terrainFeedback.json").read_text(encoding="utf-8"))
    forbidden = (
        "u_terrainLayerMode", "u_layerForceMip", "u_layerPageResolution",
        "u_layerAtlasResolution", "u_layerPagesPerAtlasSlice", "u_layerTilesPerAtlasSide",
        "u_layerMaxMip", "u_heightBlendDistanceFade", "u_compositedEnabled",
        "u_terrainHeightAtlasEnabled", "u_layerFeedbackEnabled", "u_compositedFeedbackEnabled",
        "TerrainCompositedUsePage", "TerrainCompositedHeightBlendFade",
        "AccumulateTerrainLayerVT", "EncodeLayerSlot", "TerrainLayerTargetMip",
    )
    combined = blend + feedback + evaluation + json.dumps(material) + json.dumps(schema)
    for name in forbidden:
        require(name not in combined, f"Retired production mode remains: {name}")
    for name in ("mf_terrainLayerVT.glsl", "terrainLayerVTMath.glsl"):
        require(not (shader_root / "materialFunction" / name).exists(),
                f"Retired shader still exists: {name}")
    for name in material["textures"]:
        require(not re.fullmatch(r"layerPageTable\d+|layer(?:Albedo|Normal|Pbr)Atlas", name),
                f"Retired texture remains: {name}")
    for name in ("pageTable", "layerTable", "layerIds", "layerWeights", "layerWeightMaps",
                 "layerAlbedo", "layerNormal", "layerPbr", "layerHeight",
                 "terrainHeightAtlas", "terrainHeightLayerMap", "terrainLayerSourceMap"):
        require(name in material["textures"], f"Required dependency removed: {name}")
    surface = blend.split("MaterialInputs EvaluateMFTerrainInputs(", 1)[1]
    require(surface.count("MF_TerrainCompositedVTAtFootprint(") == 1,
            "Main fragment must sample composite base")
    require("TerrainCompositedAccumulate(" not in surface
            and "TerrainSampleHeightAtlasOrSource(" not in surface,
            "Main fragment must not evaluate source base or height")
    require(re.search(r"TerrainResolveTopLayers\(\s*context,\s*true,\s*false,", surface),
            "Main fragment must retain raw coverage weights")
    require("if (useCompositedBase)" not in surface,
            "Composite sampling must be unconditional")
    require(re.search(
        r"if \(requestedLod == 2\.0\)\s*return SampleTerrainHeightAtlas\(", blend),
        "Mip2 must use resident atlas without source fallback")
    require("3.0" in blend and "1.0, weights[slot]" in blend,
            "Shared test evaluation must use source mip3 and static fade one")
    atlas = (shader_root / "materialFunction/terrainHeightAtlas.glsl").read_text(encoding="utf-8")
    require(re.search(r"if \(any\(lessThan\(sourceSize, vec2\(1\.0\)\)\)\)\s*return 0\.5;", atlas),
            "Unmapped height layers must return neutral height")
    main = feedback.split("void main()", 1)[1]
    require(not re.search(r"\b(return|discard|break|continue|for|while)\b", main),
            "Feedback derivatives must execute without skipped paths")
    require(re.findall(r"\bif\s*\(([^)]+)\)", main) == ["miss"],
            "Feedback may branch only for coverage miss priority")
    require(main.index("TerrainCompositedFeedbackFootprintFromGradients(") < main.index("if (miss)"),
            "Evaluate composite derivatives before divergent coverage handling")
    require("uint(page.x)" in main and "float(page.y)" in main
            and "coveragePage.a < 0.5" in main,
            "Coverage feedback must keep real virtual coordinates and residency")
    require("const bool compositedValid = compositedSurface.r >= 1.0;" in main,
            "Composite validity must not depend on coverage residency")
    for slot, operation in enumerate(("Address", "Metadata", "Address", "Metadata")):
        require(re.search(
            rf"outLayerSlot{slot}\s*=\s*compositedValid\s*\?\s*"
            rf"TerrainEncodeCompositedFeedback{operation}\(", main),
            f"Attachment {slot} must contain composite feedback only")
    require("int(floor(compositedMip))" in main and
            "min(int(floor(compositedMip)) + 1, int(compositedSurface.a))" in main,
            "Feedback must request both target mips")
    validate_shared_footprint(root)
    print("PASS: composite-only base/feedback, retained coverage/sources, atlas-only mip2")


def stage_feedback_shader(shader, destination):
    shader_root = shader.parent.parent.resolve()
    schema = json.loads(shader.with_name("M_terrainFeedback.json").read_text(encoding="utf-8"))
    generated = "layout(set=1,binding=0) uniform MaterialParams {\n"
    for name, definition in sorted(schema["parameters"].items()):
        generated += f'    {definition["type"]} {name};\n'
    generated += "};\n"
    parameter_path = (shader.parent / "generate/M_terrainFeedbackParamter.glsl").resolve()
    visited = set()

    def stage(path):
        path = path.resolve()
        if path in visited:
            return
        visited.add(path)
        source = generated if path == parameter_path else path.read_text(encoding="utf-8")
        target = destination / path.relative_to(shader_root)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source, encoding="utf-8")
        for include in re.findall(r'^\s*#include\s+"([^"]+)"', source, flags=re.M):
            stage(path.parent / include)

    stage(shader)
    return destination / shader.resolve().relative_to(shader_root)


def compile_shader(shader):
    compiler = shutil.which("glslc")
    validator = shutil.which("spirv-val")
    require(compiler and validator, "--compile requires glslc and spirv-val")
    with tempfile.TemporaryDirectory(prefix="terrain-feedback-") as temporary:
        output = Path(temporary)
        staged = stage_feedback_shader(shader, output / "glsl")
        for optimization in ("-O0", "-O"):
            subprocess.run([compiler, "--target-env=vulkan1.2", optimization,
                            str(staged), "-o", str(output / "feedback.spv")], check=True)
            subprocess.run([validator, "--target-env", "vulkan1.2",
                            str(output / "feedback.spv")], check=True)
            print(f"PASS: full feedback fragment and SPIR-V validation ({optimization})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compile", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    validate_shaders(root)
    if options.compile:
        compile_shader(root / "shader/glsl/pass/terrainFeedback.frag")
        validator = shutil.which("glslangValidator")
        require(validator, "Production stage checks require glslangValidator")
        with tempfile.TemporaryDirectory(prefix="terrain-material-") as temporary:
            output = Path(temporary)
            material_stages(root, output, validator)
            for binary in output.glob("*.spv"):
                subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(binary)], check=True)
            material_stages(root, output, validator, tessellation=False)
            for binary in output.glob("*.spv"):
                subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(binary)], check=True)
            for source in (root / "shader/glsl/terrain/terrainCompositedBake.comp",
                           Path(__file__).with_name("composited_height_distance.comp")):
                binary = output / f"{source.stem}.spv"
                subprocess.run(["glslc", "--target-env=vulkan1.2", "-Ishader/glsl",
                                str(source), "-o", str(binary)], cwd=root, check=True)
                subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(binary)], check=True)
            print("PASS: full terrain material, linked TES pipeline, bake and shared evaluation compute")


if __name__ == "__main__":
    main()

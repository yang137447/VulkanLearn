"""CPU-only tourr4 staging and real feedback graphics cache rebuild; no Vulkan execution."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from feedback_mip_pair_test import cases, expressions, production_address


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    return {str(path.relative_to(root)): digest(path) for path in root.rglob("*") if path.is_file()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-stage", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--core", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = root / "artifacts/terrain-rvt-production/feedback-mip-pair-tourr4"
    output.mkdir(parents=True, exist_ok=True)
    source = options.source_stage.resolve()
    stage = options.destination.resolve()
    core = options.core.resolve()
    if stage.exists() and not options.resume:
        raise ValueError("Use a new private destination; never overwrite an existing stage")
    source_before = inventory(source / "shader")
    if not stage.exists():
        shutil.copytree(source, stage)
    shader = root / "shader/glsl/pass/terrainFeedback.frag"
    shutil.copy2(shader, stage / "shader/glsl/pass/terrainFeedback.frag")
    stage_before = inventory(source / "shader/spv")
    commands = []

    def run(command, label, environment=None):
        command = list(map(str, command))
        commands.append(command)
        result = subprocess.run(command, cwd=root, env=environment, capture_output=True, text=True)
        (output / (label + ".log")).write_text(
            repr(command) + "\n" + result.stdout + result.stderr, encoding="utf-8")
        print(result.stdout + result.stderr, end="", flush=True)
        result.check_returncode()

    run(["py", "-3", "-m", "unittest", "discover", "-s", "tool/validation/terrain",
         "-p", "feedback_mip_pair_test.py", "-v"], "boundary-tests")
    old_source = source / "shader/glsl/pass/terrainFeedback.frag"
    old = old_source.read_text("utf-8")
    new = shader.read_text("utf-8")
    if old.replace("int(ceil(compositedMip))",
                   "min(int(floor(compositedMip)) + 1, int(compositedSurface.a))") != new:
        raise ValueError("Source-stage fragment differs beyond the authorized high-mip expression")
    includes = Path(os.environ["TEMP"]) / "vulkanlearn-noether-quality/run006-coherent/includes.rsp"
    target = output / "feedback_graphics_cache.obj"
    run(["g++", "-std=gnu++17", "-O0", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
         "-ffunction-sections", "-fdata-sections", "@" + str(includes),
         "-c", root / "tool/validation/terrain/feedback_graphics_cache.cpp", "-o", target], "compiler-build")
    native = Path(os.environ["TEMP"]) / "vulkanlearn-noether-quality/run006-coherent"
    libraries = (native / "libraries.rsp").read_text("utf-8")
    first = libraries.index('"', 1)
    libraries = '"' + (core / "libvl_runtime.a").as_posix() + '"' + libraries[first + 1:]
    response = output / "libraries.rsp"
    response.write_text(libraries, encoding="utf-8")
    executable = output / "feedback_graphics_cache.exe"
    run(["g++", target, "-Wl,--gc-sections", "-o", executable, "@" + str(response)], "compiler-link")
    environment = os.environ.copy()
    environment["PATH"] = str(stage / ("bin" if (stage / "bin").is_dir() else "")) + ";" + environment["PATH"]
    receipt = output / "graphics-cache.json"
    compile_receipt = output / "graphics-cache-verification.json" if receipt.exists() else receipt
    run([executable, stage, compile_receipt], "actual-shader-compiler", environment)
    pair = expressions()
    fixture_source = new.replace(
        "#version 450", "#version 450\n#extension GL_GOOGLE_include_directive : require", 1)
    fixture_source = fixture_source.replace("void main()", "void ProductionFeedbackMain()", 1)
    targets = ", ".join(repr(target) for target, _, _ in cases())
    maxima = ", ".join(str(maximum) for _, maximum, _ in cases())
    fixture_source += f"""
void main()
{{
    const float targets[9] = float[9]({targets});
    const int maxima[9] = int[9]({maxima});
    int index = clamp(int(gl_FragCoord.x), 0, 8);
    float compositedMip = targets[index];
    vec4 compositedSurface = vec4(4.0, 8192.0, 8192.0, float(maxima[index]));
    outLayerSlot0 = TerrainEncodeCompositedFeedbackAddress(
        vec2(0.5, 0.37), vec2(8192.0), 3, {pair[0]});
    outLayerSlot2 = TerrainEncodeCompositedFeedbackAddress(
        vec2(0.5, 0.37), vec2(8192.0), 3, {pair[1]});
    outFeedback = vec4(0.0);
    outLayerSlot1 = TerrainEncodeCompositedFeedbackMetadata(17u);
    outLayerSlot3 = TerrainEncodeCompositedFeedbackMetadata(17u);
    outQualityWinner0 = uvec4(0u);
    outQualityWinner1 = uvec4(0u);
}}
"""
    fixture = output / "feedback-mip-pair.frag"
    fixture.write_text(fixture_source, encoding="utf-8")
    run(["glslangValidator", "-V", "--target-env", "vulkan1.2",
         "-I" + str(stage / "shader/glsl/pass"), fixture,
         "-o", output / "feedback-mip-pair.spv"], "gpu-fixture-compile")
    run(["spirv-val", "--target-env", "vulkan1.2", output / "feedback-mip-pair.spv"], "gpu-fixture-validate")
    expected = [{"pixel": index, "target": target, "maximum": maximum,
                 "lowAddressBytes": production_address(expected[0], (0.5, 0.37)),
                 "highAddressBytes": production_address(expected[1], (0.5, 0.37)),
                 "metadataBytes": [17, 1, 0, 0]}
                for index, (target, maximum, expected) in enumerate(cases())]
    (output / "gpu-fixture-expected.json").write_text(json.dumps(expected, indent=2), encoding="utf-8")
    stage_after = inventory(stage / "shader/spv")
    changed = [name for name in set(stage_before) | set(stage_after)
               if stage_before.get(name) != stage_after.get(name)]
    if any(name != "shader-build-cache.json" and
           not name.replace("\\", "/").startswith(("pass/terrainFeedback/", "pass/terrainFeedback_"))
           for name in changed):
        raise ValueError("Unexpected non-feedback cache changes")
    if inventory(source / "shader") != source_before:
        raise ValueError("Original package shader closure changed during preparation")
    report = {"gpuExecuted": False, "gpuAddressExecutionPending": True,
        "sourceStage": str(source), "privateTourr4Stage": str(stage), "core": str(core),
        "coreCheckpointSha256": digest(core / "checkpoint.json"),
        "runtimeArchiveSha256": digest(core / "libvl_runtime.a"),
        "sourceBeforeSha256": digest(old_source), "sourceAfterSha256": digest(shader),
        "onlyProductionSourceChange": "terrainFeedback.frag high request ceil -> min(floor+1,maxMip)",
        "shaderSchemaOrTagVersionChanged": False, "baseGeneratorIdentityChanged": False,
        "qualityFrameChanged": False, "recookRequired": False,
        "cpuBoundaryCases": 9, "cpuEncodedAddressChecks": 54,
        "gpuFixture": str(fixture), "cacheChangedFiles": sorted(changed),
        "actualGraphicsCompilerReceipt": json.loads(receipt.read_text("utf-8")),
        "commands": commands, "sourceStageUnchanged": True}
    (output / "prepared.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Prepared tourr4 feedback-only stage: " + str(stage), flush=True)


if __name__ == "__main__":
    main()

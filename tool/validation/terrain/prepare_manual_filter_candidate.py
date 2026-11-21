import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from prepare_parallel_sample_prototype import MAIN
from check_composited_world_spirv import inspect_spirv


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    terrain = root / "shader/glsl/terrain"
    original = terrain / "terrainCompositedBakeWorld.comp"
    evaluator_path = terrain / "terrainCompositedBakeWorldEvaluator.glsl"
    sampler_path = terrain / "terrainCompositedSourceFilterCandidate.glsl"
    evaluator = evaluator_path.read_text(encoding="utf-8")
    replacements = {}
    for name in ("layerAlbedo", "layerNormal", "layerOrm", "layerHeight"):
        evaluator, count = re.subn(r"\btextureLod\(\s*" + name + r"\s*,",
                                   "TerrainBakeSourceTrilinear(" + name + ",", evaluator)
        if count != 1:
            raise RuntimeError(f"Expected exactly one source sampler operation for {name}, found {count}")
        replacements[name] = count
    evaluator = sampler_path.read_text(encoding="utf-8") + "\n" + evaluator
    source = original.read_text(encoding="utf-8")
    source = source.replace("layout(local_size_x = 8, local_size_y = 8) in;",
                            "layout(local_size_x = 64) in;")
    source = source[:source.index("void main()")] + MAIN

    def expand(path, text):
        def replacement(match):
            target = (path.parent / match[1]).resolve()
            selected = evaluator if target == evaluator_path else target.read_text(encoding="utf-8")
            return expand(target, selected)
        return re.sub(r'#include "([^"]+)"', replacement, text)

    source = expand(original, source)
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    target = output / "manual-parallel-candidate.comp"
    target.write_text(source, encoding="utf-8")
    report = {
        "status": "compiled-not-executed", "gpuExecuted": False, "productionFilesModified": False,
        "sourceSamplerOperationsReplaced": replacements,
        "unchangedSampling": ["coverage", "local weights", "frame normal", "frame tangent", "catalog fetch"],
        "filter": "decoded-view four texelFetch bilinear per actual mip, two-level trilinear, repeat",
        "height": "original fixed mip3, now same explicit bilinear",
        "normalOrm": "linear-view filtering, original normal decode/normalize and roughness RMS",
        "parallel": "one 64-lane workgroup per output texel, sequential original-order reduction",
        "dualReference": "independent double AND unchanged original actual-GPU, separate one-byte gates",
        "sourceHashes": {str(path.relative_to(root)): digest(path)
                         for path in (original, evaluator_path, sampler_path)},
        "artifacts": {},
    }
    for debug in (False, True):
        artifact = output / ("manual-parallel-candidate.debug.spv" if debug else "manual-parallel-candidate.spv")
        subprocess.run(["glslc", "-O", *(["-g"] if debug else []), "--target-env=vulkan1.4",
                        str(target), "-o", str(artifact)], check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.4", str(artifact)], check=True)
        report["artifacts"][artifact.name] = inspect_spirv(artifact, artifact.with_suffix(".spvasm"))
    (output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

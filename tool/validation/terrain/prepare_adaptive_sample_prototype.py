import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from prepare_parallel_sample_prototype import MAIN
from check_composited_world_spirv import inspect_spirv
import coverage_static_top4_prototype


def phased_main(area4_lanes):
    shared = MAIN[:MAIN.index("void main()")]
    evaluation = MAIN[MAIN.index("    parallelColour[lane] ="):MAIN.index("    barrier();")]
    evaluation = evaluation.replace("if (lane < sampleCount)",
                                    "if (validPixel && sampleBase + sampleLane < sampleCount)")
    evaluation = evaluation.replace("vec2(lane % samples, lane / samples)",
                                    "vec2((sampleBase + sampleLane) % samples, "
                                    "(sampleBase + sampleLane) / samples)")
    encode = MAIN[MAIN.index("    const float inverseCount ="):]
    return shared + """
void main()
{
    const uint lane = gl_LocalInvocationIndex;
    const uint samples = world.control.x;
    const uint sampleCount = samples * samples;
    const uint lanesPerTexel = sampleCount <= 1u ? 1u :
        sampleCount <= 4u ? 4u : sampleCount <= 16u ? AREA4_LANESu : 64u;
    const uint texelsPerGroup = 64u / lanesPerTexel;
    const uint sampleLane = lane % lanesPerTexel;
    const uint texelLane = lane / lanesPerTexel;
    const uvec2 pixel = uvec2(gl_WorkGroupID.x * texelsPerGroup + texelLane, gl_WorkGroupID.y);
    const uvec2 evaluationExtent = world.control.y != 0u ? uvec2(136) :
        min(uvec2(128), world.page.xy - world.page.zw * 128u) + 8u;
    const bool validPixel = all(lessThan(pixel, evaluationExtent));
    vec3 colourSum = vec3(0);
    vec3 normalSum = vec3(0);
    vec3 ormSum = vec3(0);
    for (uint sampleBase = 0u; sampleBase < sampleCount; sampleBase += lanesPerTexel)
    {
""".replace("AREA4_LANES", str(area4_lanes)) + evaluation + """
        barrier();
        if (validPixel && sampleLane == 0u)
        {
            const uint phaseCount = min(lanesPerTexel, sampleCount - sampleBase);
            for (uint sampleIndex = 0u; sampleIndex < phaseCount; ++sampleIndex)
            {
                const uint sharedIndex = texelLane * lanesPerTexel + sampleIndex;
                colourSum += parallelColour[sharedIndex].xyz;
                normalSum += parallelNormal[sharedIndex].xyz;
                ormSum += parallelOrm[sharedIndex].xyz;
            }
        }
        if (sampleBase + lanesPerTexel < sampleCount)
            barrier();
    }
    if (!validPixel || sampleLane != 0u)
        return;
""" + encode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--area4-lanes", type=int, choices=(4, 8, 16), default=16)
    parser.add_argument("--coverage-static-top4", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    original = root / "shader/glsl/terrain/terrainCompositedBakeWorld.comp"
    main = MAIN
    start = main.index("    const uvec2 pixel =")
    end = main.index("    parallelColour[lane] =")
    main = main[:start] + """
    const uint lane = gl_LocalInvocationIndex;
    const uint samples = world.control.x;
    const uint sampleCount = samples * samples;
    const uint lanesPerTexel = sampleCount <= 1u ? 1u :
        sampleCount <= 4u ? 4u : sampleCount <= 16u ? 16u : 64u;
    const uint texelsPerGroup = 64u / lanesPerTexel;
    const uint sampleLane = lane % lanesPerTexel;
    const uint texelLane = lane / lanesPerTexel;
    const uvec2 pixel = uvec2(gl_WorkGroupID.x * texelsPerGroup + texelLane, gl_WorkGroupID.y);
    const uvec2 evaluationExtent = world.control.y != 0u ? uvec2(136) :
        min(uvec2(128), world.page.xy - world.page.zw * 128u) + 8u;
    const bool validPixel = all(lessThan(pixel, evaluationExtent));
""" + main[end:]
    main = main.replace("if (lane < sampleCount)", "if (validPixel && sampleLane < sampleCount)")
    main = main.replace("vec2(lane % samples, lane / samples)", "vec2(sampleLane % samples, sampleLane / samples)")
    main = main.replace("if (lane != 0u)\n        return;",
                        "if (!validPixel || sampleLane != 0u)\n        return;")
    main = main.replace("parallelColour[sampleIndex]", "parallelColour[texelLane * lanesPerTexel + sampleIndex]")
    main = main.replace("parallelNormal[sampleIndex]", "parallelNormal[texelLane * lanesPerTexel + sampleIndex]")
    main = main.replace("parallelOrm[sampleIndex]", "parallelOrm[texelLane * lanesPerTexel + sampleIndex]")
    if options.area4_lanes != 16:
        main = phased_main(options.area4_lanes)
    source = original.read_text(encoding="utf-8")
    source = source.replace("layout(local_size_x = 8, local_size_y = 8) in;",
                            "layout(local_size_x = 64) in;")
    source = source[:source.index("void main()")] + main

    def expand(path, text):
        return re.sub(r'#include "([^"]+)"', lambda match:
                      expand((path.parent / match[1]).resolve(),
                             (path.parent / match[1]).read_text(encoding="utf-8")), text)

    source = expand(original, source)
    coverage_check = None
    if options.coverage_static_top4:
        coverage_check = coverage_static_top4_prototype.cpu_check()
        source = coverage_static_top4_prototype.apply(source)
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    path = output / "adaptive-samples-prototype.comp"
    path.write_text(source, encoding="utf-8")
    report = {
        "status": "compiled-not-executed", "gpuExecuted": False,
        "productionFilesModified": False,
        "sourceSha256": hashlib.sha256(original.read_bytes()).hexdigest(),
        "mapping": {"sampleCounts1": "64 texels x1 lane",
                    "sampleCounts2to4": "16 texels x4 lanes",
                    "sampleCounts5to16": f"{64 // options.area4_lanes} texels x{options.area4_lanes} lanes",
                    "sampleCounts17to64": "1 texel x64 lanes"},
        "area4LanesPerTexel": options.area4_lanes,
        "area4SamplePhases": (16 + options.area4_lanes - 1) // options.area4_lanes,
        "sharedReductionBytes": 3072,
        "phaseSynchronization": "all lanes barrier after evaluation; before shared reuse between phases"
            if options.area4_lanes != 16 else "single barrier before ordered leader reduction",
        "partialGroups": "all 64 lanes reach barrier; invalid texels suppress work and output",
        "reduction": "sequential original sample order within each texel lane partition",
        "sampler": "unchanged original hardware textureLod evaluator",
        "coverageStaticTop4": coverage_check,
        "artifacts": {},
    }
    coverage = []
    for samples in range(1, 9):
        count = samples * samples
        lanes = 1 if count <= 1 else 4 if count <= 4 else options.area4_lanes if count <= 16 else 64
        texels = 64 // lanes
        for width in (1, 9, 11, 72, 136):
            records = []
            for group in range((width + texels - 1) // texels):
                for sample_base in range(0, count, lanes):
                    for lane in range(64):
                        pixel = group * texels + lane // lanes
                        sample = sample_base + lane % lanes
                        if pixel < width and sample < count:
                            records.append((pixel, sample))
            if sorted(records) != [(pixel, sample) for pixel in range(width) for sample in range(count)]:
                raise RuntimeError("Adaptive mapping loses or duplicates a texel/sample")
            for pixel in range(width):
                if [sample for selected, sample in records if selected == pixel] != list(range(count)):
                    raise RuntimeError("Adaptive phases change the original per-texel reduction order")
        coverage.append({"samplesPerAxis": samples, "lanesPerTexel": lanes,
                         "texelsPerGroup": texels, "testedWidths": [1, 9, 11, 72, 136]})
    report["cpuSampleCoverage"] = coverage
    for debug in (False, True):
        target = output / ("adaptive.debug.spv" if debug else "adaptive.spv")
        subprocess.run(["glslc", "-O", *(["-g"] if debug else []), "--target-env=vulkan1.4",
                        str(path), "-o", str(target)], check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.4", str(target)], check=True)
        report["artifacts"][target.name] = inspect_spirv(target, target.with_suffix(".spvasm"))
    (output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

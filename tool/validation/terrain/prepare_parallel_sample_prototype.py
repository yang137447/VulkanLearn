import argparse
import json
from pathlib import Path
import re
import subprocess


MAIN = r"""
shared vec4 parallelColour[64];
shared vec4 parallelNormal[64];
shared vec4 parallelOrm[64];

void main()
{
    const uvec2 pixel = gl_WorkGroupID.xy;
    const uvec2 evaluationExtent = world.control.y != 0u ? uvec2(136) :
        min(uvec2(128), world.page.xy - world.page.zw * 128u) + 8u;
    if (any(greaterThanEqual(pixel, evaluationExtent)))
        return;
    const uint lane = gl_LocalInvocationIndex;
    const uint samples = world.control.x;
    const uint sampleCount = samples * samples;
    parallelColour[lane] = vec4(0);
    parallelNormal[lane] = vec4(0);
    parallelOrm[lane] = vec4(0);
    if (lane < sampleCount)
    {
        const vec2 resolution = vec2(world.page.xy);
        const vec2 origin = vec2(world.page.zw * 128u) + vec2(pixel) - 4.0;
        const vec2 subpixel = (vec2(lane % samples, lane / samples) + 0.5) / float(samples);
        const vec2 query = (origin + subpixel) / resolution;
        const vec2 footprint = 1.0 / (resolution * float(samples));
        uint target = world.control.z;
        vec2 domainUv = fract(query);
        vec2 footprintU = vec2(footprint.x, 0);
        vec2 footprintV = vec2(0, footprint.y);
        uint diagnostic = 0u;
        if (world.control.y == 0u)
        {
            target = TerrainBakeResolve(query, world.control.z, world.control.w,
                world.surfaces[0].adjacency.x == 2u);
            if (target < world.control.w)
            {
                const Chart chart = charts.entries[target];
                domainUv = clamp(vec2(dot(chart.rowU.xyz, vec3(query, 1)),
                    dot(chart.rowV.xyz, vec3(query, 1))), 0.0, 1.0);
                footprintU = vec2(chart.rowU.x, chart.rowV.x) * footprint.x;
                footprintV = vec2(chart.rowU.y, chart.rowV.y) * footprint.y;
            }
            else
                diagnostic |= 32u;
        }
        if (diagnostic == 0u)
        {
            vec3 colour;
            vec3 normal;
            vec3 orm;
            EvaluateTarget(target, domainUv, footprintU, footprintV,
                colour, normal, orm, diagnostic);
            normal = normalize(mat3(world.surfaces[target].normalToSource) * normal);
            parallelColour[lane] = vec4(colour, 0);
            parallelNormal[lane] = vec4(normal, 0);
            parallelOrm[lane] = vec4(orm.r, orm.g * orm.g, orm.b, 0);
        }
        if (diagnostic != 0u)
            atomicOr(outputPage.words[3u * 136u * 136u], diagnostic);
    }
    barrier();
    if (lane != 0u)
        return;
    vec3 colourSum = vec3(0);
    vec3 normalSum = vec3(0);
    vec3 ormSum = vec3(0);
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        colourSum += parallelColour[sampleIndex].xyz;
        normalSum += parallelNormal[sampleIndex].xyz;
        ormSum += parallelOrm[sampleIndex].xyz;
    }
    const float inverseCount = 1.0 / float(sampleCount);
    const vec3 colour = TerrainCompositedLinearToSrgb(colourSum * inverseCount);
    const vec3 normal = TerrainCompositedNormal(normalSum, vec3(0, 1, 0));
    vec3 orm = ormSum * inverseCount;
    orm.g = sqrt(max(orm.g, 0.0));
    const uint texel = pixel.y * 136u + pixel.x;
    outputPage.words[texel] = packUnorm4x8(vec4(colour, 1));
    outputPage.words[136u * 136u + texel] = packUnorm4x8(vec4(normal * 0.5 + 0.5, 1));
    outputPage.words[2u * 136u * 136u + texel] = packUnorm4x8(vec4(orm, 1));
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    original = root / "shader/glsl/terrain/terrainCompositedBakeWorld.comp"
    source = original.read_text(encoding="utf-8")
    source = source.replace("layout(local_size_x = 8, local_size_y = 8) in;",
                            "layout(local_size_x = 64) in;")
    source = source[:source.index("void main()")] + MAIN
    def expand(path, text):
        return re.sub(r'#include "([^"]+)"', lambda match:
                      expand((path.parent / match[1]).resolve(),
                             (path.parent / match[1]).read_text(encoding="utf-8")), text)
    source = expand(original, source)
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    path = output / "parallel-samples-prototype.comp"
    path.write_text(source, encoding="utf-8")
    target = output / "parallel-samples-prototype.spv"
    subprocess.run(["glslc", "-O", "--target-env=vulkan1.4", str(path), "-o", str(target)], check=True)
    subprocess.run(["spirv-val", "--target-env", "vulkan1.4", str(target)], check=True)
    report = {
        "status": "compiled-not-executed", "gpuExecuted": False,
        "productionFilesModified": False, "localSize": 64, "dispatch": "activeWidth x activeHeight x1",
        "rootWorkgroups": 81, "previousRootWorkgroups": 4,
        "sharedReductionBytes": 3072, "reductionOrder": "original sampleY-major sampleX-minor",
        "evaluator": "unchanged EvaluateTarget and opaque samplers from formal World source",
        "limitations": [
            "Does not fix hardware texture filtering numeric discrepancy",
            "No occupancy/register/driver instruction measurements",
            "More workgroups may cost more at mip0; requires measurement before adoption",
        ],
    }
    (output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

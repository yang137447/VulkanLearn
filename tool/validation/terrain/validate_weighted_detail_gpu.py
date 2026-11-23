import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys

from validate_composited_bake_gpu import material_stages, run


def function(source, name):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def normalized(values):
    length = math.sqrt(sum(value * value for value in values))
    return [value / length for value in values]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--validator", default=shutil.which("glslangValidator"))
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    here = Path(__file__).resolve().parent
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    production = root / "shader/glsl/materialFunction/mf_terrainLayerBlend.glsl"
    source = production.read_text(encoding="utf-8")
    preamble = r'''
#version 450
#extension GL_GOOGLE_include_directive : require
#define VL_TERRAIN_TES_STAGE 0
#include "materialFunction/mf_normal.glsl"
#include "materialFunction/terrainCoverage.glsl"
#include "materialFunction/terrainCompositedEvaluation.glsl"
layout(local_size_x=8,local_size_y=8) in;
layout(std430,set=0,binding=1) buffer Output { uint words[]; } outputPage;
layout(set=0,binding=2) uniform sampler2D pageTable;
layout(set=0,binding=3) uniform sampler2D layerIds;
layout(set=0,binding=4) uniform sampler2D layerWeights;
layout(set=0,binding=5) uniform sampler2DArray layerWeightMaps;
layout(set=0,binding=6) uniform sampler2D layerTable;
layout(set=0,binding=7) uniform sampler2DArray layerAlbedo;
layout(set=0,binding=8) uniform sampler2DArray layerNormal;
layout(set=0,binding=12) uniform sampler2DArray layerHeight;
layout(set=0,binding=18) uniform sampler2D terrainLayerSourceMap;
const float u_uvScale=1.0;
const float u_detailUvScale=1.0;
const float u_macroUvScale=0.5;
const float u_terrainWeightMapMode=1.0;
const float u_pageResolution=128.0;
const float u_heightBlendStrength=3.0;
struct TestView { vec3 cameraPosition; };
const TestView uboVP=TestView(vec3(0));
const int Globals[4]=int[4](13,207,31,79);
int TerrainSourceSlice(int layer) {
    for(int index=0;index<4;++index) if(Globals[index]==layer) return index;
    return layer;
}
vec4 TerrainLayerCatalogRowLod0(int layer,int row) {
    return texelFetch(layerTable,ivec2(Globals[TerrainSourceSlice(layer)],row),0);
}
vec4 TerrainLayerCatalogRow(int layer,int row) { return TerrainLayerCatalogRowLod0(layer,row); }
vec4 TerrainLayerExtra(int layer) { return TerrainLayerCatalogRowLod0(layer,1); }
vec4 TerrainLayerExtraForStage(int layer,bool implicitLod) { return TerrainLayerExtra(layer); }
float TerrainSampleHeightAtlasOrSource(vec2 uv,int layer,float lod) {
    return textureLod(layerHeight,vec3(uv,float(TerrainSourceSlice(layer))),lod).r;
}
'''
    shader = output / "weighted-detail.comp"
    shader.write_text(preamble + "\n" + "\n".join(function(source, name) for name in (
        "TerrainResolveTopLayers", "TerrainBlendedLayerDisplacementMeters", "TerrainWeightedDetail"))
        + r'''
void main() {
    const uvec2 pixel=gl_GlobalInvocationID.xy;
    if(pixel.y!=0u || pixel.x>=136u) return;
    const uint query=pixel.x;
    MaterialFunctionContext context;
    context.texCoord=vec2(.37,.61);
    context.texCoord1=vec2(.1);
    context.worldPosition=vec3(0);
    context.worldNormal=normalize(vec3(-.6,1,-.3));
    context.worldTangent=vec4(1,0,0,1);
    int ids[4]=int[4](13,207,31,79);
    float weights[4]=float[4](.4,.3,.2,.1);
    if(query>=128u) {
        float raw[4]; float base[4];
        TerrainResolveTopLayers(context,true,query%2u!=0u,ids,base,raw);
        const uint offset=query*16u;
        for(uint slot=0u;slot<4u;++slot) {
            outputPage.words[offset+slot]=floatBitsToUint(raw[slot]);
            outputPage.words[offset+4u+slot]=floatBitsToUint(base[slot]);
        }
        return;
    }
    const float firstWeight=.5+(float(query/2u)-31.5)*.00001;
    weights[0]=firstWeight*.8; weights[1]=(1-firstWeight)*.8;
    weights[2]=.15; weights[3]=.05;
    if(query%2u!=0u) {
        int tempId=ids[0]; ids[0]=ids[1]; ids[1]=tempId;
        float tempWeight=weights[0]; weights[0]=weights[1]; weights[1]=tempWeight;
    }
    const float derivative=query<64u ? 1.0/64.0 : 1.0/16.0;
    vec3 colour; vec3 normal; vec3 macro;
    TerrainWeightedDetail(context,ids,weights,
        vec2(derivative,0),vec2(0,derivative),colour,normal,macro);
    const float displacement=TerrainBlendedLayerDisplacementMeters(context,ids,weights);
    const uint offset=query*16u;
    for(uint component=0u;component<3u;++component) {
        outputPage.words[offset+component]=floatBitsToUint(colour[component]);
        outputPage.words[offset+3u+component]=floatBitsToUint(normal[component]);
        outputPage.words[offset+6u+component]=floatBitsToUint(macro[component]);
    }
    outputPage.words[offset+9u]=floatBitsToUint(displacement);
}
''', encoding="utf-8")
    executable = output / "weighted-check.exe"
    run([options.compiler, "-std=gnu++17", "-O2", "-Iextern/VulkanSDK/include",
         here / "composited_bake_gpu_check.cpp", "-Lextern/VulkanSDK/lib/Win32",
         "-lvulkan-1", "-o", executable], root)
    run([options.validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl",
         shader, "-o", output / "weighted.spv"], root)
    gpu = run([executable, output / "weighted.spv", "bounded-weighted-detail",
               output / "readback.bin"], root)
    data = (output / "readback.bin").read_bytes()
    measurements = [struct.unpack_from("<16f", data, index * 64) for index in range(136)]
    errors = []
    maximum = 0.0
    slope = normalized([-.6, 1, -.3])
    bitangent = [0, slope[2], -slope[1]]
    for index in range(128):
        first = .5 + (index // 2 - 31.5) * .00001
        weights = [first * .8, (1 - first) * .8, .15, .05]
        derivative = 1 / 64 if index < 64 else 1 / 16
        mips = [max(0, math.log2(derivative * 8 * 2 ** layer)) for layer in range(4)]
        colour = [sum(weights[layer] * value for layer, value in enumerate(channel))
                  for channel in ([.1, .2, .3, .4],
                                  [.1 * (mip + 1) for mip in mips],
                                  [.05 * (layer + mips[layer] + 1) for layer in range(4)])]
        normals = []
        for layer, mip in enumerate(mips):
            normal_ts = [.08 * (layer + 1), .06 * (mip + 1), 1]
            normals.append(normalized([
                normal_ts[0] * float(axis == 0) +
                normal_ts[1] * bitangent[axis] + normal_ts[2] * slope[axis]
                for axis in range(3)]))
        normal = normalized([sum(weights[layer] * normals[layer][axis] for layer in range(4))
                             for axis in range(3)])
        macro = [sum(weights[layer] * value for layer, value in enumerate(channel))
                 for channel in ([.1, .2, .3, .4], [.1] * 4, [.05, .1, .15, .2])]
        displacement = sum(weights[layer] * (.1 * (layer + 3) - .5) *
                           (.1 * (layer + 1)) for layer in range(4))
        reference = colour + normal + macro + [displacement]
        error = max(abs(actual - expected)
                    for actual, expected in zip(measurements[index], reference))
        maximum = max(maximum, error)
        if error > 3e-6:
            errors.append({"query": index, "error": error})
    rank_error = max(abs(measurements[index][channel] - measurements[index + 1][channel])
                     for index in range(0, 128, 2) for channel in range(10))
    raw_error = max(abs(measurements[index][slot] - [.4, .3, .2, .1][slot])
                    for index in range(128, 136) for slot in range(4))
    height_base_change = max(abs(measurements[128][4 + slot] - measurements[129][4 + slot])
                             for slot in range(4))
    gradient_response = max(abs(measurements[0][channel] - measurements[64][channel])
                            for channel in (1, 2, 3, 4, 5))
    path_raw_error = max(abs(measurements[index][slot] - measurements[index + 4][slot])
                         for index in range(128, 132) for slot in range(4))
    continuity = max(abs(measurements[index][channel] - measurements[index + 2][channel])
                     for index in range(0, 126, 2) if index != 62 for channel in range(10))
    displacement_error = max(abs(measurements[index][9] - sum(
        ([((.5 + (index // 2 - 31.5) * .00001) * .8),
          ((.5 - (index // 2 - 31.5) * .00001) * .8), .15, .05][layer]) *
        (.1 * (layer + 3) - .5) * (.1 * (layer + 1))
        for layer in range(4))) for index in range(128))
    legacy_normal_jump = math.sqrt(sum((normals[0][axis] - normals[1][axis]) ** 2
                                      for axis in range(3)))
    material_stages(root, output, options.validator)
    report = {
        "passed": not errors and rank_error < 3e-6 and raw_error < 3e-6 and
        height_base_change > .001 and gradient_response > .001 and
        path_raw_error < 3e-6 and continuity < 2e-5 and legacy_normal_jump > .01,
        "gpu": gpu, "queries": 136, "maxReferenceError": maximum,
        "rankPermutationMaxError": rank_error, "rawCoverageMaxError": raw_error,
        "heightBlendBaseChange": height_base_change, "gradientResponse": gradient_response,
        "directCompositeRawMaxError": path_raw_error,
        "rankCrossingMaxStep": continuity, "rankCrossingStepTolerance": 2e-5,
        "displacementMaxErrorMeters": displacement_error,
        "legacyDominantNormalJumpNegativeControl": legacy_normal_jump,
        "tolerance": 3e-6, "failures": errors,
        "sourceSha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in (production, here / "composited_bake_gpu_check.cpp",
                                      Path(__file__))},
    }
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())

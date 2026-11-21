import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    terrain = root / "shader/glsl/terrain"
    locked = root / "artifacts/terrain-rvt-production/p3-baker-world-repeat-fix/runs/run-003/sources/shader/glsl/terrain/terrainCompositedBakeWorldEvaluator.glsl"
    expected_evaluator = "d977382c566ad4af542554008ec7d850ebe704831889789eea96b3037af3038b"
    if not locked.exists() or digest(locked) != expected_evaluator:
        raise RuntimeError("Prelocked original actual-GPU evaluator identity is missing or changed")
    original = terrain / "terrainCompositedBakeWorld.comp"
    source = original.read_text(encoding="utf-8")
    declaration = """
layout(std430, set = 0, binding = 14) readonly buffer IndependentCpuAddresses
{
    vec4 values[];
} cpuAddresses;
"""
    source = source.replace("void main()", declaration + "\nvoid main()", 1)
    start = source.index("            uint target;", source.index("void main()"))
    end = source.index("            vec3 colour;", start)
    source = source[:start] + """
            const uint sampleIndex = sampleY * samples + sampleX;
            const uint addressOffset = ((pixel.y * evaluationExtent.x + pixel.x) *
                samples * samples + sampleIndex) * 5u;
            const vec4 address = cpuAddresses.values[addressOffset];
            const uint target = uint(address.z);
            const vec2 domainUv = address.xy;
            const vec4 derivative = cpuAddresses.values[addressOffset + 1u];
            const vec2 footprintU = derivative.xy;
            const vec2 footprintV = derivative.zw;
""" + source[end:]
    old = "normal = normalize(mat3(world.surfaces[target].normalToSource) * normal);"
    replacement = """
            normal = normalize(vec3(
                dot(cpuAddresses.values[addressOffset + 2u].xyz, normal),
                dot(cpuAddresses.values[addressOffset + 3u].xyz, normal),
                dot(cpuAddresses.values[addressOffset + 4u].xyz, normal)));
"""
    if source.count(old) != 1:
        raise RuntimeError("Independent normal transform patch no longer matches")
    source = source.replace(old, replacement)

    def expand(path, text):
        def include(match):
            target = (path.parent / match[1]).resolve()
            selected = locked.read_text(encoding="utf-8") \
                if target.name == "terrainCompositedBakeWorldEvaluator.glsl" else target.read_text(encoding="utf-8")
            return expand(target, selected)
        return re.sub(r'#include "([^"]+)"', include, text)
    source = expand(original, source)
    path = output / "independent-original-gpu.comp"
    path.write_text(source, encoding="utf-8")
    for debug in (False, True):
        target = output / ("original.debug.spv" if debug else "original.spv")
        subprocess.run(["glslc", "-O", *(["-g"] if debug else []), "--target-env=vulkan1.4",
                        str(path), "-o", str(target)], check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.4", str(target)], check=True)
    baker = (root / "source/render/resource/terrainCompositedBakeGpu.cpp").read_text(encoding="utf-8")
    baker = '#include "composited_cpu_address_oracle.h"\n' + baker
    baker = baker.replace("taskCount * 5)", "taskCount * 6)")
    baker = baker.replace("std::vector<vk::WriteDescriptorSet> writes(14);",
                          "std::vector<vk::WriteDescriptorSet> writes(15);")
    baker = baker.replace("if (binding < 2 || binding == 13)",
                          "if (binding < 2 || binding == 13 || binding == 14)")
    baker = baker.replace(".setBufferInfo(buffers[binding < 2 ? binding : 2]);",
                          ".setBufferInfo(binding == 14 ? TerrainCpuAddressOracle::buffers.at(params.buffer).descriptor : buffers[binding < 2 ? binding : 2]);")
    baker = baker.replace("backend->UpdateDescriptorSets(writes);",
                          "writes.erase(writes.begin() + 13);\n    backend->UpdateDescriptorSets(writes);")
    call = "WriteDescriptors(slot.descriptors, inputs, parameterBuffer, output, chartBuffer);"
    baker = baker.replace(call, """
        TerrainCpuAddressOracle::Prepare(*backend, slot.params, &params, sizeof(params),
            gpuCharts.data(), gpuCharts.size() * 64, snapshot, sourceId, page,
            params.page[0], params.page[1]);
""" + call)
    baker = baker.replace("for (Slot& slot : slots)\n    {",
                          "for (Slot& slot : slots)\n    {\n        TerrainCpuAddressOracle::Release(*backend, slot.params);")
    baker_path = output / "independentOriginalBaker.cpp"
    baker_path.write_text(baker, encoding="utf-8")
    report = {
        "gpuExecuted": False, "status": "generated-and-shader-validated",
        "inputAuthority": "CPU double graph.Resolve per integral sample; no candidate GPU route",
        "normalAuthority": "independent cofactor inverse-transpose of target then source transpose",
        "layerEvaluator": "original immutable hardware sampler/evaluator formula, no manual filtering",
        "diagnosticOnlyExtraSsbo": "per-sample target,domain,jacobian,normal; not a production resource",
        "shaderSha256": digest(path), "bakerSha256": digest(baker_path),
        "evaluatorSourceSha256": digest(locked),
        "evaluatorPrelockedSource": str(locked),
        "graphSourceSha256": digest(root / "source/terrain/terrainCompositedAdjacency.cpp"),
    }
    (output / "oracle-identity.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

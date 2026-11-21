import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def body(source, name):
    match = re.search(rf"\b(?:void|bool|float)\s+{name}\s*\(", source)
    if not match:
        raise RuntimeError(f"Missing evaluator function {name}")
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def canonical(source):
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    return re.sub(r"\s+", "", source)


def inspect_spirv(path, assembly_path):
    result = subprocess.run(["spirv-dis", "--raw-id", str(path), "-o", str(assembly_path)],
                            check=True)
    text = assembly_path.read_text(encoding="utf-8")
    capabilities = re.findall(r"^\s*OpCapability\s+(\S+)", text, re.M)
    if set(capabilities) - {"Shader", "ImageQuery"} or "NonUniform" in text:
        raise RuntimeError(f"Unexpected SPIR-V capability/decorations: {capabilities}")
    constants = {}
    instructions = {}
    bindings = {}
    sets = {}
    offsets = {}
    strides = {}
    for line in text.splitlines():
        tokens = line.strip().split()
        if len(tokens) > 3 and tokens[1] == "=":
            instructions[tokens[0]] = tokens[2:]
            if tokens[2] == "OpConstant":
                try:
                    constants[tokens[0]] = int(tokens[-1], 0)
                except ValueError:
                    pass
        if len(tokens) >= 4 and tokens[0] == "OpDecorate":
            if tokens[2] == "Binding":
                bindings[tokens[1]] = int(tokens[3])
            if tokens[2] == "DescriptorSet":
                sets[tokens[1]] = int(tokens[3])
            if tokens[2] == "ArrayStride":
                strides[tokens[1]] = int(tokens[3])
        if len(tokens) >= 5 and tokens[0] == "OpMemberDecorate" and tokens[3] == "Offset":
            offsets.setdefault(tokens[1], {})[int(tokens[2])] = int(tokens[4])
    samplers = {variable: binding for variable, binding in bindings.items()
                if sets.get(variable) == 0 and 2 <= binding <= 12}
    if len(samplers) != 11:
        raise RuntimeError("World shader must expose all 11 sampler roles")
    indices = {binding: set() for binding in samplers.values()}
    accesses = 0
    opaque_types = {identity for identity, instruction in instructions.items()
                    if instruction[0] in ("OpTypeSampledImage", "OpTypeImage", "OpTypeSampler")}
    for instruction in instructions.values():
        if instruction[0] in ("OpPhi", "OpSelect") and instruction[1] in opaque_types:
            raise RuntimeError("Opaque image selection escaped the literal switch branches")
    for variable, binding in samplers.items():
        declaration = instructions[variable]
        pointer = instructions[declaration[1]]
        array = instructions[pointer[2]]
        if declaration[0] != "OpVariable" or pointer[0] != "OpTypePointer" or \
                array[0] != "OpTypeArray" or constants[array[2]] != 16:
            raise RuntimeError(f"Sampler binding {binding} must be a fixed 16-element array")
    for instruction in instructions.values():
        if instruction[0] in ("OpAccessChain", "OpInBoundsAccessChain") and instruction[2] in samplers:
            selected = instruction[3:]
            if len(selected) != 1 or selected[0] not in constants or not 0 <= constants[selected[0]] < 16:
                raise RuntimeError(f"Nonliteral sampler descriptor access: {instruction}")
            indices[samplers[instruction[2]]].add(constants[selected[0]])
            accesses += 1
    if any(selected != set(range(16)) for selected in indices.values()):
        raise RuntimeError("Sampler literal access coverage differs from 11 roles x16 surfaces")
    variable = next(variable for variable, binding in bindings.items()
                    if binding == 0 and sets.get(variable) == 0)
    world = instructions[instructions[variable][1]][2]
    if offsets[world] != {0: 0, 1: 16, 2: 32}:
        raise RuntimeError("World params prefix offsets differ from C++ ABI")
    world_type = instructions[world]
    surfaces = world_type[3]
    if strides[surfaces] != 2240 or constants[instructions[surfaces][2]] != 16:
        raise RuntimeError("Surface std430 stride or capacity differs from C++ ABI")
    return {"capabilities": capabilities, "samplerBindings": 11, "samplersPerBinding": 16,
            "samplerAccessChains": accesses, "allSamplerIndicesLiteral": True,
            "surfaceStrideBytes": 2240, "worldSurfaceOffsetBytes": 32,
            "worldParameterBytes": 35872, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference-evaluator", type=Path)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    terrain = root / "shader/glsl/terrain"
    reference = options.reference_evaluator or root / (
        "artifacts/terrain-rvt-production/p3-baker-world-repeat-fix/runs/run-003/"
        "sources/shader/glsl/terrain/terrainCompositedBakeWorldEvaluator.glsl")
    old = reference.read_text(encoding="utf-8")
    new = (terrain / "terrainCompositedBakeWorldEvaluator.glsl").read_text(encoding="utf-8")
    report = {"gpuExecuted": False, "logicalDeviceChanged": False, "evaluatorFunctions": {}, "artifacts": {}}
    for name in ("FiniteValue", "SourceLod", "ResolveLayers", "EvaluateBase"):
        actual = body(new, name).replace(
            "ResolveLayers(TERRAIN_BAKE_PROTOTYPE_ARGUMENTS,\n"
            "        surfaceUv, domainUv, ids, weights, terrainDiagnostic);",
            "ResolveLayers(surfaceUv, domainUv, ids, weights);")
        expected = body(old, name).replace(
            "ResolveLayers(TERRAIN_BAKE_PROTOTYPE_ARGUMENTS,\n"
            "        surfaceUv, domainUv, ids, weights, terrainDiagnostic);",
            "ResolveLayers(surfaceUv, domainUv, ids, weights);")
        if canonical(actual) != canonical(expected):
            raise RuntimeError(f"Evaluator body changed beyond opaque arguments/status plumbing: {name}")
        report["evaluatorFunctions"][name] = "body-identical-after-argument-plumbing"
    for name in ("terrainCompositedBakeWorld", "terrainCompositedBakeWorldPadding"):
        for debug in (False, True):
            target = output / (name + (".debug.spv" if debug else ".spv"))
            command = ["glslc", "-O", "--target-env=vulkan1.4"]
            if debug:
                command.append("-g")
            subprocess.run([*command, str(terrain / (name + ".comp")), "-o", str(target)], check=True)
            subprocess.run(["spirv-val", "--target-env", "vulkan1.4", str(target)], check=True)
            if name == "terrainCompositedBakeWorld":
                report["artifacts"][target.name] = inspect_spirv(target, target.with_suffix(".spvasm"))
    report["status"] = "passed"
    (output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

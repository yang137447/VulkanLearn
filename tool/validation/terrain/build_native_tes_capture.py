"""Compile native helper and two private production mirrors; never apply patches or run Vulkan."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import difflib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
from compile_frozen_cpp import compile_frozen

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent / "native-capture"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--project", type=Path, default=ROOT.parent / "p5s/C010")
    parser.add_argument("--instrumented-tes", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    mirror = output / "private-source"
    shutil.copytree(ROOT/"source", mirror/"source")
    originals = {name: (ROOT/name).read_text(encoding="utf-8")
                 for name in ("source/vulkanManager.cpp", "source/pipeline/graphicsPipeline.cpp")}
    transformed = dict(originals)
    device = transformed["source/vulkanManager.cpp"]
    anchor = "    vk::PhysicalDeviceVulkan12Features deviceVulkan12Features;"
    assert device.count(anchor) == 1
    device = '#include "terrainTesDiagnostic.h"\n' + device
    device = device.replace(anchor, """    if (VL::TerrainTesDiagnostic::Requested())
    {
        if (!supportedFeatures.vertexPipelineStoresAndAtomics || !supportedFeatures.tessellationShader ||
            physicalDevices[kGpuIndex].getProperties().limits.maxBoundDescriptorSets < 5)
            throw std::runtime_error("Explicit TES capture unavailable: vertex stores/tessellation/set4");
        deviceFeatures.setVertexPipelineStoresAndAtomics(VK_TRUE);
    }
""" + anchor, 1)
    anchor = "    device = physicalDevices[kGpuIndex].createDevice(deviceCreateInfo);"
    assert device.count(anchor) == 1
    device = device.replace(anchor, anchor + """
    VL::TerrainTesDiagnostic::DeviceFeatureEnabled(VL::TerrainTesDiagnostic::Requested());""", 1)
    anchor = "    VL::ReleasePresentSwapchainsForDevice(device);"
    assert device.count(anchor) == 1
    device = device.replace(anchor, """    VL::TerrainTesDiagnostic::DeviceFeatureEnabled(false);
""" + anchor, 1)
    transformed["source/vulkanManager.cpp"] = device
    graphics = '#include "terrainTesDiagnostic.h"\n' + transformed["source/pipeline/graphicsPipeline.cpp"]
    anchor = """    descriptorSetLayouts = PipelineLayoutBuilder::CreateDescriptorSetLayouts(
        *rendererBackend,
        descriptorLayoutBindings,
        shaderDisplayName);"""
    assert graphics.count(anchor) == 1
    graphics = graphics.replace(anchor, """    uint32_t diagnosticSetCount = MAX_DESCRIPTOR_SETS;
    for (const auto& binding : descriptorLayoutBindings)
    {
        if (binding.set < MAX_DESCRIPTOR_SETS)
            continue;
        if (!VL::TerrainTesDiagnostic::Requested() || !VL::TerrainTesDiagnostic::DeviceFeatureEnabled() ||
            binding.set != 4 || binding.binding != 0 ||
            binding.type != vk::DescriptorType::eStorageBuffer || binding.descriptorCount != 1 ||
            binding.stageFlags != vk::ShaderStageFlagBits::eTessellationEvaluation ||
            binding.name != "terrainTestTesCapture" ||
            rendererBackend->GetSelectedDeviceLimits().maxBoundDescriptorSets < 5)
            throw std::runtime_error("Unexpected set4 outside explicit TES diagnostic pipeline");
        diagnosticSetCount = 5;
    }
    descriptorSetLayouts = PipelineLayoutBuilder::CreateDescriptorSetLayouts(
        *rendererBackend, descriptorLayoutBindings, shaderDisplayName, diagnosticSetCount);""", 1)
    transformed["source/pipeline/graphicsPipeline.cpp"] = graphics
    patch = []
    for name, text in transformed.items():
        path = mirror/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        patch.extend(difflib.unified_diff(originals[name].splitlines(True), text.splitlines(True),
            fromfile="a/"+name, tofile="b/"+name))
    (output/"production-diagnostic-request.patch").write_text("".join(patch), encoding="utf-8")
    before = {name: digest(ROOT/name) for name in originals}
    cpu_pipeline = output/"terrainTesCapturePipelineCpu.cpp"
    cpu_pipeline.write_text('#define VL_TERRAIN_CAPTURE_CPU_TEST_ONLY 1\n#include "' +
                            (HERE/"terrainTesCapturePipeline.cpp").as_posix() + '"\n', encoding="utf-8")
    sources = [
        HERE/"terrainTesCapture.cpp", HERE/"terrainTesCapturePipeline.cpp", HERE/"terrainTesCaptureCpuTests.cpp",
        HERE/"terrainTesCapturePolicy.cpp", cpu_pipeline,
        ROOT/"source/terrain/terrainRvtBudget.cpp",
        ROOT/"source/pipeline/graphicsShaderVariantArtifact.cpp",
        ROOT/"source/material/compiler/materialShaderComposer.cpp",
        ROOT/"source/pipeline/shaderReflectionService.cpp",
        ROOT/"source/shader/build/atomicFile.cpp", ROOT/"source/core/runtimeFileTrace.cpp",
        ROOT/"source/core/tracingIfstream.cpp", ROOT/"source/shader/build/contentHash.cpp",
        ROOT/"source/shader/build/shaderBuildManifest.cpp", ROOT/"source/shader/shaderAbiSignature.cpp",
        ROOT/"source/shaderCompiler.cpp", ROOT/"source/shaderCompilerApi.cpp",
        ROOT/"source/shaderCompilerCommit.cpp", ROOT/"source/shaderCompilerCache.cpp", ROOT/"source/shaderReflect.cpp"]
    sources = [mirror/path.relative_to(ROOT) if path.is_relative_to(ROOT/"source") else path for path in sources]
    compiler = shutil.which("g++")
    includes = ROOT/"build-vt-release/source/CMakeFiles/vl_runtime.dir/includes_CXX.rsp"
    flags = ["-std=gnu++17", "-O1", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
             "-ffunction-sections", "-fdata-sections", "-ffp-contract=off",
             "-I"+str(HERE), "-I"+str(mirror/"source"), "-I"+str(mirror/"source/pipeline"), "@"+str(includes)]
    records = {}
    def build(item):
        index, path = item
        target = output / ("unit-"+str(index)+".obj")
        record = compile_frozen(compiler, flags, path, target, ROOT)
        return str(path), record
    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(build, item) for item in enumerate(sources)]
        for future in futures:
            name, record = future.result()
            records[name] = record
    for index, name in enumerate(originals):
        path = mirror/name
        target = output / ("mirror-"+str(index)+".obj")
        records[str(path)] = compile_frozen(compiler, flags, path, target, ROOT)
    executable = output/"native-tes-cpu.exe"
    objects = [record["object"] for name, record in records.items()
               if Path(name).name not in ("terrainTesCapture.cpp", "terrainTesCapturePipeline.cpp") and
               Path(name).name not in ("vulkanManager.cpp", "graphicsPipeline.cpp")]
    libraries = [
        ROOT/"extern/Shaderc/lib/libshaderc_shared.dll.a",
        ROOT/"extern/Spirv-Reflect/lib/libspirv-reflect-static.a",
        ROOT/"build-vt-release/extern/BLAKE3/libblake3.a"]
    command = [compiler, *objects, "-Wl,--gc-sections", "-o", str(executable), *map(str, libraries)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    (output/"link.log").write_text(repr(command)+"\n"+result.stdout+result.stderr, encoding="utf-8")
    result.check_returncode()
    environment = os.environ.copy()
    environment["PATH"] = str(ROOT/"build-vt-release/bin")+";"+environment["PATH"]
    command = [str(executable), str(args.project.resolve()), str(args.instrumented_tes.resolve()),
               str(output/"native-cpu-tests.json")]
    result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True)
    (output/"native-cpu-tests.log").write_text(result.stdout+result.stderr, encoding="utf-8")
    result.check_returncode()
    after = {name: digest(ROOT/name) for name in originals}
    if before != after:
        raise RuntimeError("Shared integration source changed during private build; freeze needs re-coordination")
    report = {"status": "native-helper-private-mirrors-compiled-cpu-tested",
        "gpuExecuted": False, "productionFilesWritten": [], "r9BarrierMustNotWait": True,
        "requiresNewSourceLockAndNativeIntegrationBuild": True, "productionSourceBeforeAfter": before,
        "privateProductionPatch": str(output/"production-diagnostic-request.patch"),
        "compilationClosures": records, "executable": str(executable),
        "executableSha256": digest(executable), "librarySha256": {str(path): digest(path) for path in libraries},
        "tests": json.loads((output/"native-cpu-tests.json").read_text(encoding="utf-8")),
        "missing": ["Dirac actual draw callback and submit/abort/fence owner integration",
                    "actual GPU capture window", "independent same-frame scene-lit reference"]}
    (output/"build.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"status": report["status"], "gpuExecuted": False, "productionFilesWritten": [],
        "output": str(output)}))


if __name__ == "__main__":
    main()

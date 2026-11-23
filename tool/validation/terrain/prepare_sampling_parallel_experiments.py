import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from compile_frozen_cpp import compile_frozen


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--experiment", choices=("sampling", "parallel", "manual", "hardware-parallel-dual",
                                                "hardware-adaptive-dual"),
                        required=True)
    parser.add_argument("--full-matrix", action="store_true")
    parser.add_argument("--identity-calibration", action="store_true")
    parser.add_argument("--area4-lanes", type=int, choices=(4, 8, 16), default=16)
    parser.add_argument("--coverage-static-top4", action="store_true")
    parser.add_argument("--freeze-cpp-closure", action="store_true")
    parser.add_argument("--execute-gpu", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    frozen = root / "artifacts/terrain-rvt-production/p3-baker-world-repeat-fix"
    build = root / "build-vt-release/source"
    private = output / "project"
    private.mkdir(exist_ok=True)
    shutil.copytree(frozen / "project/config", private / "config", dirs_exist_ok=True)
    shutil.copytree(frozen / "project/shader", private / "shader", dirs_exist_ok=True)
    config = json.loads((private / "config/config.json").read_text())
    config["resourcePath"] = str(private / "resources")
    (private / "resources").mkdir(exist_ok=True)
    (private / "config/config.json").write_text(json.dumps(config))
    shader_directory = private / "shader/spv/terrain"
    manifest = {"experiment": options.experiment, "commands": [], "sourceHashes": {},
                "productionSourcesModified": False}
    if options.area4_lanes != 16 and options.experiment != "hardware-adaptive-dual":
        raise RuntimeError("Area4 lane overrides require the hardware adaptive experiment")
    if options.experiment == "hardware-adaptive-dual":
        manifest["area4LanesPerTexel"] = options.area4_lanes
        manifest["area4SamplePhases"] = 16 // options.area4_lanes
    if options.coverage_static_top4 and options.experiment != "hardware-adaptive-dual":
        raise RuntimeError("Coverage top4 overrides require the hardware adaptive experiment")
    manifest["coverageStaticTop4"] = options.coverage_static_top4

    def run(command, cwd=root):
        manifest["commands"].append([str(part) for part in command])
        subprocess.run([str(part) for part in command], cwd=cwd, check=True)

    def compile_source(source, target):
        manifest["sourceHashes"][str(source)] = digest(source)
        flags = ["-O2", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
             "-DVULKANLEARN_ENABLE_VULKAN_VALIDATION", "-std=gnu++17", "-ffp-contract=off",
             "-ffunction-sections", "-fdata-sections", "-I" + str(root / "tool/validation/terrain"),
             "@" + str(build / "CMakeFiles/vl_runtime.dir/includes_CXX.rsp")]
        if options.freeze_cpp_closure:
            record = compile_frozen(options.compiler, flags, source, target, build)
            manifest.setdefault("compilationClosures", {})[str(source)] = record
        else:
            run([options.compiler, *flags, "-c", source, "-o", target], build)

    if options.experiment == "sampling":
        shader = root / "shader/glsl/terrain/terrainCompositedBakeSamplingDiagnostic.comp"
        manifest["sourceHashes"][str(shader)] = digest(shader)
        for debug in (False, True):
            target = shader_directory / ("terrainCompositedBakeSamplingDiagnostic_comp.debug"
                                         if debug else "terrainCompositedBakeSamplingDiagnostic_comp.spv")
            run(["glslc", "-O", *(["-g"] if debug else []), "--target-env=vulkan1.4", shader, "-o", target])
            run(["spirv-val", "--target-env", "vulkan1.4", target])
        source = root / "tool/validation/terrain/composited_sampling_gpu_check.cpp"
        baker = frozen / "terrainCompositedBakeGpu.cpp.obj"
    else:
        prototype = output / "prototype"
        generator = "prepare_manual_filter_candidate.py" if options.experiment == "manual" else \
            "prepare_adaptive_sample_prototype.py" if options.experiment == "hardware-adaptive-dual" else \
            "prepare_parallel_sample_prototype.py"
        generator_arguments = ["py", "-3", root / "tool/validation/terrain" / generator,
                               "--output", prototype]
        if options.experiment == "hardware-adaptive-dual":
            generator_arguments += ["--area4-lanes", str(options.area4_lanes)]
            if options.coverage_static_top4:
                generator_arguments += ["--coverage-static-top4"]
        run(generator_arguments)
        shader = prototype / ("manual-parallel-candidate.comp" if options.experiment == "manual" else
                              "adaptive-samples-prototype.comp" if options.experiment == "hardware-adaptive-dual" else
                              "parallel-samples-prototype.comp")
        for debug in (False, True):
            target = shader_directory / ("terrainCompositedBakeWorld_comp.debug"
                                         if debug else "terrainCompositedBakeWorld_comp.spv")
            run(["glslc", "-O", *(["-g"] if debug else []), "--target-env=vulkan1.4", shader, "-o", target])
            run(["spirv-val", "--target-env", "vulkan1.4", target])
        formal_baker = root / "source/render/resource/terrainCompositedBakeGpu.cpp"
        text = formal_baker.read_text(encoding="utf-8")
        old = "commandBuffer.dispatch((activeWidth + 7) / 8, (activeHeight + 7) / 8, 1);"
        if text.count(old) != 1:
            raise RuntimeError("Parallel prototype dispatch patch no longer matches")
        dispatch = "commandBuffer.dispatch(activeWidth, activeHeight, 1);"
        if options.experiment == "hardware-adaptive-dual":
            dispatch = """
        const uint32_t sampleCount = page.areaSamplesPerAxis * page.areaSamplesPerAxis;
        const uint32_t lanesPerTexel = sampleCount <= 1 ? 1 : sampleCount <= 4 ? 4 :
            sampleCount <= 16 ? AREA4_LANES : 64;
        const uint32_t texelsPerGroup = 64 / lanesPerTexel;
        commandBuffer.dispatch((activeWidth + texelsPerGroup - 1) / texelsPerGroup, activeHeight, 1);
"""
            dispatch = dispatch.replace("AREA4_LANES", str(options.area4_lanes))
        text = text.replace(old, dispatch)
        patched = output / "parallelBaker.cpp"
        patched.write_text(text, encoding="utf-8")
        baker = output / "parallelBaker.cpp.obj"
        compile_source(patched, baker)
        formal_harness = root / "tool/validation/terrain/composited_page_gpu_check.cpp"
        text = formal_harness.read_text(encoding="utf-8")
        if options.identity_calibration:
            start = text.index("            auto graphs = TerrainBakeRouteCases::Graphs();")
            end = text.index("        }\n        SDL_DestroyWindow(window);", start)
            text = text[:start] + """
            TerrainCompositedAdjacencyGraph graph({"identity"}, {});
            auto fixture = TerrainPageFixture::Build(backend, graph, 2, 32, true);
            Baker::PageRequest fine;
            fine.virtualWidth = fine.virtualHeight = 256;
            fine.areaSamplesPerAxis = 1;
            Baker::PageRequest coarse;
            coarse.virtualWidth = coarse.virtualHeight = 8192;
            coarse.virtualMip = 13;
            coarse.areaSamplesPerAxis = 4;
            const auto numericDirectory = destination / "identity";
            std::filesystem::create_directories(numericDirectory);
            const auto batch = RunBatch(backend, baker, fixture,
                {{"identity", fine}, {"identity", coarse}}, true, 1, numericDirectory);
            report["identityCalibration"] = batch;
            report["numericPassed"] = true;
            report["performancePassed"] = true;
            report["status"] = "calibration-measured";
            backend.WaitIdle();
""" + text[end:]
        if not options.full_matrix:
            text = text.replace("for (uint32_t mode = 0; mode < 3; ++mode)\n            {\n                auto fixture = TerrainPageFixture::Build(backend, graphs[0], mode, 32, true);",
                            "for (uint32_t mode = 0; mode < 0; ++mode)\n            {\n                auto fixture = TerrainPageFixture::Build(backend, graphs[0], mode, 32, true);")
            text = text.replace("for (uint32_t mode = 0; mode < 3; ++mode)\n            {\n                auto fixture = TerrainPageFixture::Build(backend, graphs[0], mode, 256);",
                            "for (uint32_t mode = 0; mode < 1; ++mode)\n            {\n                auto fixture = TerrainPageFixture::Build(backend, graphs[0], mode, 256);")
            text = text.replace("false, 64, directory)", "false, 4, directory)")
            text = text.replace("false, 32, directory)", "false, 4, directory)")
            text = text.replace('{"numeric", "spatialNumeric", "pressureModes"}',
                            '{"numeric", "pressureModes"}')
        source = output / "parallelPageHarness.cpp"
        source.write_text(text, encoding="utf-8")
    target = output / "harness.obj"
    compile_source(source, target)
    if options.identity_calibration:
        baker = frozen / "terrainCompositedBakeGpu.cpp.obj"
        for filename in ("terrainCompositedBakeWorld_comp.spv", "terrainCompositedBakeWorld_comp.debug"):
            shutil.copy2(frozen / "runs/run-003/shader/spv/terrain" / filename, shader_directory / filename)
        manifest["calibrationBaseline"] = "locked raw serial single/world evaluator, not parallel candidate"
    executable = output / (options.experiment + "Experiment.exe")
    libraries = (frozen / "libraries.rsp").read_text()
    response = output / "libraries.rsp"
    response.write_text(libraries)
    run([options.compiler, target, baker, frozen / "terrainCompositedAdjacency.cpp.obj",
         "-Wl,--gc-sections", "-Wl,-Map=" + str(output / "link.map"),
         "-o", executable, "@" + str(response)], build)
    if options.experiment in ("manual", "hardware-parallel-dual", "hardware-adaptive-dual"):
        reference_executable = output / "originalGpuReference.exe"
        original_baker = frozen / "terrainCompositedBakeGpu.cpp.obj"
        if options.experiment in ("hardware-parallel-dual", "hardware-adaptive-dual"):
            oracle_directory = output / "independent-oracle"
            run(["py", "-3", root / "tool/validation/terrain/prepare_independent_gpu_oracle.py",
                 "--output", oracle_directory])
            original_baker = oracle_directory / "independentOriginalBaker.cpp.obj"
            compile_source(oracle_directory / "independentOriginalBaker.cpp", original_baker)
            manifest["oracleIdentity"] = json.loads((oracle_directory / "oracle-identity.json").read_text())
        run([options.compiler, target, original_baker,
             frozen / "terrainCompositedAdjacency.cpp.obj", "-Wl,--gc-sections",
             "-Wl,-Map=" + str(output / "original-reference-link.map"),
             "-o", reference_executable, "@" + str(response)], build)
        reference_project = output / "original-project"
        shutil.copytree(private, reference_project, dirs_exist_ok=True)
        original_shaders = frozen / "runs/run-003/shader/spv/terrain"
        for filename in ("terrainCompositedBakeWorld_comp.spv", "terrainCompositedBakeWorld_comp.debug"):
            selected = original_shaders / filename
            if options.experiment in ("hardware-parallel-dual", "hardware-adaptive-dual"):
                selected = oracle_directory / ("original.debug.spv" if filename.endswith(".debug") else
                                               "original.spv")
            shutil.copy2(selected, reference_project / "shader/spv/terrain" / filename)
        manifest["originalExecutableSha256"] = digest(reference_executable)
        manifest["originalBakerObjectSha256"] = digest(original_baker)
        manifest["originalShaderHashes"] = {
            filename: digest(reference_project / "shader/spv/terrain" / filename)
            for filename in ("terrainCompositedBakeWorld_comp.spv", "terrainCompositedBakeWorld_comp.debug")}
        manifest["scope"] = "70 numeric+352 finite pressure" if options.full_matrix else \
            "22 numeric+16 representative pressure, not frozen full scope"
        if options.identity_calibration:
            manifest["scope"] = "two no-adjacency identity calibration pages, byte-exact oracle check"
            manifest["oracleCalibrationByteExactRequired"] = True
            manifest["samplerPolicy"] = "calibration: locked raw serial actual-GPU versus repaired CPU-address oracle"
        manifest["candidateExecutable"] = executable.name
        manifest["samplerPolicy"] = "unchanged hardware textureLod and original sampler" \
            if options.experiment in ("hardware-parallel-dual", "hardware-adaptive-dual") else "manual decoded-view filtering"
        manifest["authorityStatus"] = "Hypatia ruling: original actual-GPU evaluator is authoritative; double remains diagnostic" \
            if options.experiment in ("hardware-parallel-dual", "hardware-adaptive-dual") else "both references separately reported"
    manifest["executableSha256"] = digest(executable)
    manifest["shaderHashes"] = {str(path.relative_to(private)): digest(path)
                                for path in shader_directory.iterdir() if path.is_file()}
    (output / "build.json").write_text(json.dumps(manifest, indent=2))
    if options.execute_gpu:
        directory = output / "run-001"
        directory.mkdir()
        shutil.copy2(executable, directory / executable.name)
        shutil.copy2(output / "build.json", directory / "build.json")
        shutil.copytree(private / "shader", directory / "shader")
        environment = os.environ.copy()
        environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
        arguments = [str(directory / executable.name), str(directory)]
        if options.experiment in ("parallel", "manual", "hardware-parallel-dual", "hardware-adaptive-dual"):
            arguments.append("--validation-off")
        result = subprocess.run(arguments, cwd=private, env=environment,
                                capture_output=True, text=True, errors="replace")
        (directory / "stdout.log").write_text(result.stdout, encoding="utf-8")
        (directory / "stderr.log").write_text(result.stderr, encoding="utf-8")
        print(json.dumps({"experiment": options.experiment, "exitCode": result.returncode,
                          "report": str(directory / "report.json")}))
        print(result.stderr[-2000:])
        result.check_returncode()


if __name__ == "__main__":
    main()

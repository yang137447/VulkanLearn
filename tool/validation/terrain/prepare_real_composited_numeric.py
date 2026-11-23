import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

from compile_frozen_cpp import compile_frozen
from real_numeric_evidence import digest, inventory, publication_shaders


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--coverage-manifest", type=Path, required=True)
    parser.add_argument("--local-manifest", type=Path)
    parser.add_argument("--reuse-objects", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    real = root / "artifacts/terrain-rvt-production/p3-real10936"
    calibration = root / "artifacts/terrain-rvt-production/p3-oracle-compiled-closure-calibration"
    promotion = root / "artifacts/terrain-rvt-production/p3-controlled-promotion"
    previous = json.loads((real / "build.json").read_text())
    calibrated = json.loads((calibration / "build.json").read_text())
    calibration_result = json.loads(
        (calibration / "runs/run-001/dual-reference-report.json").read_text())
    if not calibration_result["referenceValid"] or not calibration_result["identityCalibrationByteExact"]:
        raise RuntimeError("New complete-header-closure original reference has not passed calibration")
    compiler = previous["compiler"]
    build = root / "build-vt-release/source"
    flags = ["-O2", "-DNDEBUG", "-DIMATH_DLL", "-DOPENEXR_DLL",
             "-DVULKANLEARN_ENABLE_VULKAN_VALIDATION", "-std=gnu++17", "-ffp-contract=off",
             "-ffunction-sections", "-fdata-sections",
             "@" + str(build / "CMakeFiles/vl_runtime.dir/includes_CXX.rsp")]
    manifest = {
        "status": "preparing", "gpuExecuted": False, "performanceAcceptance": False,
        "scope": "192 real Full16 pages per published mode; all 3 channels and border/padding",
        "sources": [inventory(options.coverage_manifest)], "compilationClosures": {},
        "productionSourcesModified": False,
        "independentOracle": {
            "authority": "unchanged original actual GPU evaluator with independent CPU double graph/normal",
            "evaluatorSha256": "d977382c566ad4af542554008ec7d850ebe704831889789eea96b3037af3038b",
            "twoPageCalibrationReport": str(calibration / "runs/run-001/dual-reference-report.json"),
            "twoPageCalibrationReportSha256": digest(
                calibration / "runs/run-001/dual-reference-report.json"),
            "compiledSourceClosures": calibrated["compilationClosures"],
            "gpuShaderIdentity": json.loads((calibration / "independent-oracle/oracle-identity.json").read_text()),
        },
    }
    if manifest["sources"][0]["weightSource"] != 0:
        raise RuntimeError("Coverage argument is not a real Coverage publication")
    published_shaders = publication_shaders(options.coverage_manifest.resolve())
    manifest["candidateActualPublicationShaders"] = published_shaders
    document = json.loads(options.coverage_manifest.read_text(encoding="utf-8"))
    frozen_sources = [
        root / "shader/glsl/terrain" / name for name in (
            "terrainCompositedBakeWorld.comp", "terrainCompositedBakeWorldPadding.comp",
            "terrainCompositedBakeWorldTypes.glsl", "terrainCompositedBakeWorldEvaluator.glsl",
            "terrainCompositedBakeCoverage.glsl", "terrainCompositedBakeRoute.glsl")]
    frozen_sources.append(root / "shader/glsl/materialFunction/terrainCompositedEvaluation.glsl")
    published_ids = {document["production"]["generator"]["computeSource"],
                     *document["production"]["generator"]["includeSources"]}
    published_source_bytes = [
        (options.coverage_manifest.resolve().parent / item["path"]).read_bytes()
        for item in document["production"]["objects"] if item["objectId"] in published_ids]
    if len(published_source_bytes) != 7 or \
            any(path.read_bytes() not in published_source_bytes for path in frozen_sources):
        raise RuntimeError("C008 source closure differs from the frozen formal production sources")
    manifest["candidatePublicationSourceSha256"] = {
        str(path.relative_to(root)): digest(path) for path in frozen_sources}
    if options.local_manifest:
        local = inventory(options.local_manifest)
        if local["weightSource"] != 1:
            raise RuntimeError("Local argument is not an actual Local publication")
        manifest["sources"].append(local)
    candidate_object = output / "candidateBaker.obj"
    candidate_record = compile_frozen(compiler, flags, root / "source/render/resource/terrainCompositedBakeGpu.cpp",
                                      candidate_object, build)
    manifest["compilationClosures"]["candidateBaker"] = candidate_record
    original_object = output / "originalBaker.obj"
    oracle_closure = next(record for name, record in calibrated["compilationClosures"].items()
                          if name.endswith("independentOriginalBaker.cpp"))
    if digest(Path(oracle_closure["compiledInput"])) != oracle_closure["compiledInputSha256"] or \
            digest(Path(oracle_closure["object"])) != oracle_closure["objectSha256"]:
        raise RuntimeError("Calibrated independent original native compiled input/object changed")
    shutil.copy2(oracle_closure["object"], original_object)
    manifest["originalObjectSha256"] = digest(original_object)
    source_names = list(previous["compilationClosures"])
    source_names.remove("tool/validation/terrain/composited_real_pressure_check.cpp")
    source_names.append("tool/validation/terrain/composited_real_numeric_check.cpp")
    objects = []
    for name in source_names:
        print("Freezing numeric dependency " + name, flush=True)
        target = output / (Path(name).name + ".obj")
        record = previous["compilationClosures"].get(name)
        reusable = options.reuse_objects and record and record["flags"] == flags and \
            Path(record["object"]).exists() and digest(Path(record["object"])) == record["objectSha256"] and \
            all(Path(path).exists() and digest(Path(path)) == expected
                for path, expected in record["filesBeforeAndAfterPreprocessing"].items())
        if reusable:
            shutil.copy2(record["object"], target)
        else:
            record = compile_frozen(compiler, flags, root / name, target, build)
        manifest["compilationClosures"][name] = record
        objects.append(target)
        (output / "build-in-progress.json").write_text(json.dumps(manifest, indent=2))
    support = output / "runtime-support.a"
    shutil.copy2(real / "runtime-support.a", support)
    libraries = (real / "libraries.rsp").read_text().replace(
        (real / "runtime-support.a").as_posix(), support.as_posix(), 1)
    response = output / "libraries.rsp"
    response.write_text(libraries)
    manifest["runtimeSupportSha256"] = digest(support)
    for role, baker in (("candidate", candidate_object), ("original", original_object)):
        executable = output / (role + "RealNumeric.exe")
        subprocess.run([compiler, *map(str, objects), str(baker), "-Wl,--gc-sections",
                        "-Wl,-Map=" + str(output / (role + ".link.map")),
                        "-o", str(executable), "@" + str(response)], cwd=build, check=True)
        manifest[role + "Executable"] = str(executable)
        manifest[role + "ExecutableSha256"] = digest(executable)
        private = output / (role + "-project")
        shutil.copytree(real / "project", private, dirs_exist_ok=True)
        shaders = private / "shader/spv/terrain"
        for filename, item in published_shaders.items():
            selected = Path(item["path"])
            if role == "original" and filename.startswith("terrainCompositedBakeWorld_comp."):
                selected = calibration / "original-project/shader/spv/terrain" / filename
            shutil.copy2(selected, shaders / filename)
        manifest[role + "ShaderHashes"] = {
            str(path.relative_to(private)): digest(path) for path in (private / "shader").rglob("*")
            if path.is_file()}
    manifest["status"] = "ready-not-executed"
    manifest["availableModes"] = [item["weightSource"] for item in manifest["sources"]]
    manifest["localModePolicy"] = "only actual L008 published inputs; absent Local never synthesized"
    manifest["toolFilesSha256"] = {
        str(path.relative_to(root)): digest(path) for path in [
            Path(__file__), Path(__file__).with_name("run_real_composited_numeric.py"),
            Path(__file__).with_name("real_numeric_evidence.py"),
            Path(__file__).with_name("composited_real_numeric_check.cpp"),
            Path(__file__).with_name("composited_real_pressure_check.cpp")]}
    (output / "build.json").write_text(json.dumps(manifest, indent=2))
    print(json.dumps({"status": manifest["status"], "gpuExecuted": False,
                      "modes": manifest["availableModes"], "pagesPerMode": 192}))


if __name__ == "__main__":
    main()

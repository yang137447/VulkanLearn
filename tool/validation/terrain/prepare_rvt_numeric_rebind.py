"""Bind existing frozen native GPU384 tools to new actual publications; CPU only."""
import argparse
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
from real_numeric_evidence import digest, inventory, publication_shaders

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--frozen-build", type=Path, default=ROOT / "artifacts/terrain-rvt-production/p3-real-numeric-c008")
    parser.add_argument("--coverage-manifest", type=Path, required=True)
    parser.add_argument("--local-manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    directory = args.frozen_build.resolve()
    original_path = directory / "build.json"
    original = json.loads(original_path.read_text(encoding="utf-8"))
    result = copy.deepcopy(original)
    verified = {}
    for role in ("original", "candidate"):
        executable = Path(original[role+"Executable"])
        if digest(executable) != original[role+"ExecutableSha256"]:
            raise ValueError("Frozen native executable changed: "+role)
        verified[str(executable)] = digest(executable)
        for name, expected in original[role+"ShaderHashes"].items():
            path = directory / (role+"-project") / name
            if digest(path) != expected:
                raise ValueError("Frozen native shader closure changed: "+str(path))
            verified[str(path)] = expected
    for record in original["compilationClosures"].values():
        for field, hash_field in (("compiledInput", "compiledInputSha256"), ("object", "objectSha256")):
            path = Path(record[field])
            if digest(path) != record[hash_field]:
                raise ValueError("Frozen compilation input/object changed")
            verified[str(path)] = record[hash_field]
    calibration = original["independentOracle"]
    calibration_path = Path(calibration["twoPageCalibrationReport"])
    if digest(calibration_path) != calibration["twoPageCalibrationReportSha256"]:
        raise ValueError("Independent original evaluator calibration receipt changed")
    calibrated = json.loads(calibration_path.read_text(encoding="utf-8"))
    if not calibrated["referenceValid"] or not calibrated["identityCalibrationByteExact"]:
        raise ValueError("Reference calibration invalid")
    for name, expected in original["toolFilesSha256"].items():
        if digest(ROOT / name) != expected:
            if Path(name).name != "run_real_composited_numeric.py":
                raise ValueError("Frozen numeric comparator/native source changed: "+name)
    active_tools = {str(ROOT / "tool/validation/terrain" / name): digest(ROOT / "tool/validation/terrain" / name)
                    for name in ("run_rvt_numeric_bound.py", "real_numeric_evidence.py", "prepare_rvt_numeric_rebind.py")}
    dependencies = {str(path): digest(path) for path in (ROOT / "build-vt-release/bin").glob("*.dll")}
    if not dependencies:
        raise ValueError("Native runtime DLL closure missing")
    manifests = [args.coverage_manifest.resolve()]
    if args.local_manifest:
        manifests.append(args.local_manifest.resolve())
    result["sources"] = []
    args.output.parent.mkdir(parents=True, exist_ok=True)
    debug_checks = []
    current_kernels = None
    for index, manifest in enumerate(manifests):
        source = inventory(manifest)
        if source["weightSource"] != index:
            raise ValueError("Expected actual Coverage then actual Local publication")
        kernels = publication_shaders(manifest)
        if index == 0:
            current_kernels = kernels
        for name, obj in kernels.items():
            previous = original["candidateActualPublicationShaders"][name]
            if obj["sha256"] == previous["sha256"]:
                continue
            if not name.endswith(".debug"):
                raise ValueError("Current runtime kernel differs from frozen numeric tool: "+name)
            stripped = []
            for label, item in (("historical", previous), ("current", obj)):
                target = args.output.parent / (str(index)+"-"+label+"-"+name+".stripped.spv")
                subprocess.run(["spirv-opt", "--strip-debug", "--compact-ids", item["path"],
                                "-o", str(target)], check=True)
                stripped.append(target)
            if digest(stripped[0]) != digest(stripped[1]):
                raise ValueError("Debug artifact differs beyond stripped debug/ID metadata: "+name)
            debug_checks.append({"name": name, "mode": index, "historicalSha256": previous["sha256"],
                "currentSha256": obj["sha256"], "stripDebugCompactIdsEqual": True,
                "strippedSha256": digest(stripped[0])})
        result["sources"].append(source)
    private_projects = {}
    for role in ("original", "candidate"):
        private = args.output.parent / (role+"-project")
        if not private.exists():
            shutil.copytree(directory / (role+"-project"), private)
        if role == "candidate":
            for name, item in current_kernels.items():
                shutil.copy2(item["path"], private / "shader/spv/terrain" / name)
        private_projects[role] = str(private.resolve())
        result[role+"ShaderHashes"] = {str(path.relative_to(private)): digest(path)
                                     for path in (private/"shader").rglob("*") if path.is_file()}
        verified.update({str(private/path): expected for path, expected in result[role+"ShaderHashes"].items()})
    result["status"] = "ready-not-executed-rebound-actual-publication"
    result["gpuExecuted"] = False
    result["availableModes"] = [source["weightSource"] for source in result["sources"]]
    result["inputRebinding"] = {
        "oldBuild": str(original_path), "oldBuildSha256": digest(original_path),
        "executableAndFrozenCompilerClosureUnchanged": True,
        "historicalRunnerNotUsed": True,
        "privateProjects": private_projects, "debugArtifactDifferences": debug_checks,
        "currentCandidatePublicationKernels": current_kernels,
        "historicalRunnerSourceMismatch": [{"path": name, "expected": expected, "current": digest(ROOT/name)}
            for name, expected in original["toolFilesSha256"].items() if digest(ROOT/name) != expected],
        "activeTools": active_tools, "runtimeDlls": dependencies,
        "source": "Existing native executables accept the new manifest argument and restore those actual inputs at execution; no shader source or author hash is relabeled",
        "verifiedFiles": verified,
        "scope": "192 page keys per actual mode, not lit/geometry/TES or wholeP5"}
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    command = [sys.executable, "-B", str(ROOT / "tool/validation/terrain/run_rvt_numeric_bound.py"),
               "--binding", str(args.output.resolve()), "--output", "{freshNumericRun}", "--execute-gpu"]
    args.output.with_suffix(".command.json").write_text(json.dumps({
        "gpuExecuted": False, "argv": command, "missingActualLocalPublication": args.local_manifest is None}, indent=2), encoding="utf-8")
    print(json.dumps({"status": result["status"], "modes": result["availableModes"],
                      "gpuExecuted": False, "command": command}, indent=2))


if __name__ == "__main__":
    main()

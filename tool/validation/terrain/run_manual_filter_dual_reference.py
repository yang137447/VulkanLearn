import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare(candidate, reference):
    maximum = [0, 0, 0]
    above_one = 0
    compared = 0
    examples = []
    for offset, (actual, expected) in enumerate(zip(candidate[:221952], reference[:221952])):
        error = abs(actual - expected)
        maximum[offset // 73984] = max(maximum[offset // 73984], error)
        above_one += error > 1
        compared += 1
        if error > 1 and len(examples) < 8:
            texel = (offset % 73984) // 4
            examples.append({"stored": [texel % 136, texel // 136],
                             "channel": offset // 73984, "component": offset % 4,
                             "candidate": actual, "reference": expected})
    return {"bytesCompared": compared, "maximumChannelError": maximum,
            "bytesAboveOne": above_one, "examples": examples}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--execute-gpu", action="store_true")
    parser.add_argument("--validation-off", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = options.build_directory.resolve()
    manifest = json.loads((output / "build.json").read_text())
    candidate_executable = manifest.get("candidateExecutable", "manualExperiment.exe")
    for filename, key in ((candidate_executable, "executableSha256"),
                          ("originalGpuReference.exe", "originalExecutableSha256")):
        if digest(output / filename) != manifest[key]:
            raise RuntimeError("Frozen executable hash differs: " + filename)
    for name, expected in manifest["shaderHashes"].items():
        if digest(output / "project" / name) != expected:
            raise RuntimeError("Candidate shader changed: " + name)
    for name, expected in manifest["originalShaderHashes"].items():
        if digest(output / "original-project/shader/spv/terrain" / name) != expected:
            raise RuntimeError("Original actual-GPU shader changed: " + name)
    if not options.execute_gpu:
        print(json.dumps({"status": "ready-not-executed", "gpuExecuted": False,
                          "scope": manifest["scope"],
                          "references": ["independent double", "unchanged original actual-GPU"],
                          "candidate": manifest.get("samplerPolicy", "manual decoded-view filtering"),
                          "authorityStatus": manifest.get("authorityStatus", "both references gated"),
                          "tolerancePerReference": 1}))
        return
    runs = output / "runs"
    runs.mkdir(exist_ok=True)
    directory = runs / f"run-{len(list(runs.iterdir())) + 1:03}"
    directory.mkdir()
    shutil.copy2(output / "build.json", directory / "build.json")
    environment = os.environ.copy()
    environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
    summary = {"scope": manifest["scope"], "gpuExecuted": True,
               "validationEnabled": not options.validation_off, "runs": {}, "comparisons": [],
               "candidate": manifest.get("samplerPolicy", "manual decoded-view filtering"),
               "authorityStatus": manifest.get("authorityStatus", "both references gated")}
    for role, binary, project in (
        ("original", "originalGpuReference.exe", "original-project"),
        ("candidate", candidate_executable, "project"),
    ):
        selected = directory / role
        selected.mkdir()
        shutil.copy2(output / binary, selected / binary)
        shutil.copytree(output / project / "shader", selected / "shader")
        command = [str(selected / binary), str(selected)]
        if options.validation_off:
            command.append("--validation-off")
        result = subprocess.run(command, cwd=output / project, env=environment,
                                capture_output=True, text=True, errors="replace")
        (selected / "stdout.log").write_text(result.stdout, encoding="utf-8")
        (selected / "stderr.log").write_text(result.stderr, encoding="utf-8")
        summary["runs"][role] = {"exitCode": result.returncode,
                                 "report": json.loads((selected / "report.json").read_text())}
    for path in sorted((directory / "candidate").rglob("atlas-*.bin")):
        relative = path.relative_to(directory / "candidate")
        original = directory / "original" / relative
        cpu = path.with_name(path.name.replace("atlas-", "reference-"))
        if not cpu.exists():
            continue
        summary["comparisons"].append({
            "page": str(relative), "candidateVsDouble": compare(path.read_bytes(), cpu.read_bytes()),
            "candidateVsOriginalGpu": compare(path.read_bytes(), original.read_bytes()) if original.exists() else None,
            "originalGpuVsDouble": compare(original.read_bytes(), cpu.read_bytes()) if original.exists() else None,
        })
    if not summary["comparisons"]:
        raise RuntimeError("No saved numeric pages were compared; dual-reference gate cannot pass empty")
    summary["candidateDoublePassed"] = all(
        page["candidateVsDouble"]["bytesAboveOne"] == 0 for page in summary["comparisons"])
    original_report = summary["runs"]["original"]["report"]
    original_logs = (directory / "original/stdout.log").read_text(encoding="utf-8") + \
        (directory / "original/stderr.log").read_text(encoding="utf-8")
    summary["referenceValid"] = not original_report.get("error") and \
        "Validation Error:" not in original_logs and \
        all(page["candidateVsOriginalGpu"] is not None for page in summary["comparisons"])
    summary["referenceValidityReasons"] = {
        "error": original_report.get("error"),
        "validationErrors": original_logs.count("Validation Error:"),
        "missingReferencePages": sum(page["candidateVsOriginalGpu"] is None for page in summary["comparisons"]),
    }
    summary["candidateOriginalGpuPassed"] = all(
        page["candidateVsOriginalGpu"]["bytesAboveOne"] == 0
        for page in summary["comparisons"]) if summary["referenceValid"] else None
    if manifest.get("oracleCalibrationByteExactRequired"):
        summary["identityCalibrationByteExact"] = summary["referenceValid"] and all(
            page["candidateVsOriginalGpu"]["maximumChannelError"] == [0, 0, 0]
            for page in summary["comparisons"])
    summary["fileSha256"] = {
        str(path.relative_to(directory)): digest(path) for path in directory.rglob("*.bin")}
    candidate_report = summary["runs"]["candidate"]["report"]
    summary["candidatePerformancePassed"] = candidate_report.get("performancePassed", False)
    hardware_authoritative = manifest.get("experiment") in ("hardware-parallel-dual", "hardware-adaptive-dual")
    summary["status"] = "passed" if (
        summary["candidateOriginalGpuPassed"] and summary["candidatePerformancePassed"] and
        (hardware_authoritative or summary["candidateDoublePassed"])) else "failed"
    if not summary["referenceValid"]:
        summary["status"] = "invalid-reference"
    elif manifest.get("oracleCalibrationByteExactRequired") and not summary["identityCalibrationByteExact"]:
        summary["status"] = "failed-identity-calibration"
    summary["referenceAuthorityAdjudicated"] = hardware_authoritative
    summary["doubleIsDiagnostic"] = hardware_authoritative
    (directory / "dual-reference-report.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({"status": summary["status"], "report": str(directory / "dual-reference-report.json"),
                      "pagesCompared": len(summary["comparisons"]),
                      "candidateDoublePassed": summary["candidateDoublePassed"],
                      "candidateOriginalGpuPassed": summary["candidateOriginalGpuPassed"]}))
    if summary["status"] != "passed":
        raise SystemExit(1)


if __name__ == "__main__":
    main()

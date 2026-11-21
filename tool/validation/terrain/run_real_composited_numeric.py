import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

from real_numeric_evidence import compare_roles, digest, inventory, read_role, publication_shaders


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--local-manifest", type=Path)
    parser.add_argument("--local-only", action="store_true")
    parser.add_argument("--evidence-manifest", type=Path)
    parser.add_argument("--execute-gpu", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = options.build_directory.resolve()
    evidence = options.evidence_manifest.resolve() if options.evidence_manifest else output / "build.json"
    manifest = json.loads(evidence.read_text())
    sources = list(manifest["sources"])
    if options.local_manifest:
        local = inventory(options.local_manifest)
        if local["weightSource"] != 1:
            raise RuntimeError("Local numeric requires the actual Local-mode World publication")
        if any(item["weightSource"] == 1 for item in sources):
            raise RuntimeError("Local mode already frozen; do not replace its publication silently")
        local_shaders = publication_shaders(options.local_manifest.resolve())
        for name, item in local_shaders.items():
            if name.endswith(".spv") and item["sha256"] != \
                    manifest["candidateActualPublicationShaders"][name]["sha256"]:
                raise RuntimeError("Local publication runtime kernel differs from the frozen candidate")
        sources.append(local)
    if options.local_only:
        sources = [source for source in sources if source["weightSource"] == 1]
        if len(sources) != 1:
            raise RuntimeError("--local-only requires exactly one actual Local publication")
    for name, expected in manifest["toolFilesSha256"].items():
        if digest(root / name) != expected:
            raise RuntimeError("Frozen numeric tool source changed: " + name)
    for role in ("original", "candidate"):
        if digest(Path(manifest[role + "Executable"])) != manifest[role + "ExecutableSha256"]:
            raise RuntimeError("Frozen real numeric executable changed")
        for name, expected in manifest[role + "ShaderHashes"].items():
            if digest(output / (role + "-project") / name) != expected:
                raise RuntimeError("Frozen reference/candidate shader changed: " + name)
    for source in sources:
        for name, expected in source["filesSha256"].items():
            if digest(Path(name)) != expected:
                raise RuntimeError("Frozen actual publication object changed: " + name)
    if not options.execute_gpu:
        print(json.dumps({"status": "ready-not-executed", "gpuExecuted": False,
                          "modes": [source["weightSource"] for source in sources],
                          "pagesPerMode": 192, "performanceMatrixExpanded": False}))
        return
    runs = output / "runs"
    runs.mkdir(exist_ok=True)
    directory = runs / f"run-{len(list(runs.iterdir())) + 1:03}"
    directory.mkdir()
    shutil.copy2(evidence, directory / "build.json")
    (directory / "actual-input-inventories.json").write_text(json.dumps(sources, indent=2))
    environment = os.environ.copy()
    environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
    summary = {"gpuExecuted": True, "validationEnabled": True, "performanceAcceptance": False,
               "referenceAuthority": "locked independent original actual-GPU evaluator",
               "productionSourceModified": False, "localOnly": options.local_only,
               "coverageReusedOrRerun": False, "modes": []}
    for source in sources:
        mode = source["weightSource"]
        selected = directory / ("mode-" + str(mode))
        selected.mkdir()
        try:
            for role in ("original", "candidate"):
                destination = selected / role
                destination.mkdir()
                command = [manifest[role + "Executable"], source["manifest"], str(mode), str(destination)]
                with (destination / "stdout.log").open("w", encoding="utf-8") as stdout, \
                        (destination / "stderr.log").open("w", encoding="utf-8") as stderr:
                    process = subprocess.Popen(command, cwd=output / (role + "-project"),
                                               env=environment, stdout=stdout, stderr=stderr)
                    (destination / "launch.json").write_text(
                        json.dumps({"pid": process.pid, "command": command}, indent=2))
                    code = process.wait()
                if code:
                    raise RuntimeError(f"{role} exited {code}; missing/status/validation failures invalidate comparison")
                read_role(destination, source)
            result = compare_roles(selected, source)
            result["weightSource"] = mode
            result["status"] = "passed-real-numeric" if result["bytesAboveOne"] == 0 else "failed-numeric"
            summary["modes"].append(result)
        except Exception as error:
            summary["modes"].append({"weightSource": mode, "status": "invalid-reference-or-output",
                                     "referenceValid": False, "error": str(error)})
            break
    complete = len(summary["modes"]) == len(sources)
    passed = complete and all(item["status"] == "passed-real-numeric" for item in summary["modes"])
    summary["status"] = "passed-available-real-modes" if passed else "failed-or-incomplete"
    summary["bothActualModesValidated"] = passed and {source["weightSource"] for source in sources} == {0, 1}
    summary["pagesCompared"] = sum(item.get("pagesCompared", 0) for item in summary["modes"])
    (directory / "report.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps({"status": summary["status"], "pagesCompared": summary["pagesCompared"],
                      "bothActualModesValidated": summary["bothActualModesValidated"],
                      "report": str(directory / "report.json")}))
    if not passed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()

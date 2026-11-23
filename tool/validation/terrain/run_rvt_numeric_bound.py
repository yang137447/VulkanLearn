"""Run unchanged original/candidate GPU384 binaries with explicitly rebound actual inputs."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from real_numeric_evidence import compare_roles, digest, read_role


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binding", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--execute-gpu", action="store_true")
    args = parser.parse_args()
    binding = json.loads(args.binding.read_text(encoding="utf-8"))
    frozen = binding["inputRebinding"]
    for group in ("verifiedFiles", "activeTools", "runtimeDlls"):
        for name, expected in frozen[group].items():
            if digest(Path(name)) != expected:
                raise ValueError("Bound native/reference/tool/DLL changed: "+name)
    for source in binding["sources"]:
        for name, expected in source["filesSha256"].items():
            if digest(Path(name)) != expected:
                raise ValueError("Actual publication input changed: "+name)
    if not args.execute_gpu:
        print(json.dumps({"status": "ready-not-executed", "gpuExecuted": False,
                          "modes": binding["availableModes"], "pagesPerMode": 192,
                          "historicalRunnerNotUsed": True}))
        return
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output/"binding.json").write_text(json.dumps(binding, indent=2), encoding="utf-8")
    build = Path(frozen["oldBuild"]).parent
    env = os.environ.copy()
    dll_directory = str(Path(next(iter(frozen["runtimeDlls"]))).parent)
    env["PATH"] = dll_directory+";"+env["PATH"]
    report = {"gpuExecuted": True, "scope": "Actual page channels only; not scene-lit, TES, dynamics or P5",
              "modes": [], "overallP5Passed": False}
    try:
        for source in binding["sources"]:
            mode = source["weightSource"]
            selected = output / ("mode-"+str(mode))
            for role in ("original", "candidate"):
                destination = selected / role
                destination.mkdir(parents=True)
                command = [binding[role+"Executable"], source["manifest"], str(mode), str(destination)]
                with (destination/"stdout.log").open("wb") as stdout, (destination/"stderr.log").open("wb") as stderr:
                    process = subprocess.Popen(command, cwd=Path(frozen["privateProjects"][role]), env=env,
                        stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
                    (destination/"launch.json").write_text(json.dumps({"argv": command, "pid": process.pid}), encoding="utf-8")
                    code = process.wait()
                if code:
                    raise RuntimeError(role+" native numeric failed: "+str(code))
                read_role(destination, source)
            result = compare_roles(selected, source)
            result["weightSource"] = mode
            result["passed"] = result["bytesAboveOne"] == 0
            report["modes"].append(result)
            if not result["passed"]:
                raise RuntimeError("Base-channel >1 byte gate failed")
        report["status"] = "passed-available-page-modes"
        report["bothActualModesValidated"] = {source["weightSource"] for source in binding["sources"]} == {0, 1}
    except Exception as error:
        report["status"] = "failed-or-incomplete"
        report["error"] = str(error)
    (output/"report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report["status"] == "passed-available-page-modes" else 2)


if __name__ == "__main__":
    main()

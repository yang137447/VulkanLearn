import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--execute-gpu", action="store_true")
    parser.add_argument("--validation-off", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    directory = options.build_directory.resolve()
    build = json.loads((directory / "build.json").read_text())
    executable = directory / "actualBakerPageCheck.exe"
    if digest(executable) != build["executableSha256"]:
        raise RuntimeError("Prepared executable identity changed")
    changed = [name for name, expected in build["freshSources"].items()
               if digest(root / name) != expected]
    if changed:
        raise RuntimeError("Prepared source identity changed: " + ", ".join(changed))
    if not options.execute_gpu:
        print(json.dumps({"status": "ready-not-executed", "executable": str(executable),
                          "executableSha256": build["executableSha256"],
                          "numericPages": 70, "finitePressurePages": 352,
                          "gpuExecuted": False}))
        return
    runs = directory / "runs"
    runs.mkdir(exist_ok=True)
    run = runs / f"run-{len(list(runs.iterdir())) + 1:03}"
    run.mkdir()
    shutil.copy2(executable, run / executable.name)
    shutil.copy2(directory / "build.json", run / "build.json")
    shutil.copy2(directory / "link.map", run / "link.map")
    shutil.copytree(directory / "project/shader", run / "shader")
    for name in build["freshSources"]:
        target = run / "sources" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, target)
    environment = os.environ.copy()
    environment["PATH"] = str(root / "build-vt-release/bin") + ";" + environment["PATH"]
    command = [str(run / executable.name), str(run)]
    if options.validation_off:
        command.append("--validation-off")
    result = subprocess.run(command, cwd=directory / "project", env=environment,
                            capture_output=True, text=True, errors="replace")
    (run / "stdout.log").write_text(result.stdout, encoding="utf-8")
    (run / "stderr.log").write_text(result.stderr, encoding="utf-8")
    print(json.dumps({"run": str(run), "exitCode": result.returncode, "gpuExecuted": True}))
    print(result.stdout[-4000:])
    print(result.stderr[-4000:])
    result.check_returncode()


if __name__ == "__main__":
    main()

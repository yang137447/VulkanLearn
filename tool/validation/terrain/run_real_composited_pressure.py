import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--execute-gpu", action="store_true")
    options = parser.parse_args()
    output = options.build_directory.resolve()
    record = json.loads((output / "build.json").read_text())
    config = json.loads((output / "project/config/config.json").read_text())
    command = [
        sys.executable, str(Path(__file__).with_name("prepare_real_composited_pressure.py")),
        "--output", str(output), "--candidate-build", record["candidateBuild"],
        "--coverage-manifest", record["coverageManifest"], "--resource-root", config["resourcePath"],
        "--local-terrain", record["localFreezeReceipt"]["terrain"],
        "--compiler", record["compiler"], "--reuse-ready",
    ]
    if options.execute_gpu:
        command.append("--execute-gpu")
    subprocess.run(command, check=True)


if __name__ == "__main__":
    main()

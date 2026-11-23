"""Reproduce the Full16 V1 baseline with fixed poses and fresh process caches."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--first-frame-evidence", action="store_true")
    options = parser.parse_args()
    template, stage = options.template.resolve(), options.stage.resolve()
    output = options.output.resolve()
    if options.prepare:
        stage.mkdir(parents=True, exist_ok=False)
        for folder in ("bin", "config", "shader", "ui"):
            shutil.copytree(template / folder, stage / folder)
        shutil.copy2(options.binary, stage / "bin/main_game.exe")
        configuration = json.loads((stage / "config/config.json").read_text(encoding="utf-8-sig"))
        configuration["ui"]["enabled"] = False
        (stage / "config/config.json").write_text(json.dumps(configuration, indent=2), encoding="utf-8")
        inputs = {}
        for folder in ("config", "shader/glsl"):
            for path in sorted((stage / folder).rglob("*")):
                if path.is_file():
                    inputs[path.relative_to(stage).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
        inputs["bin/main_game.exe"] = hashlib.sha256((stage / "bin/main_game.exe").read_bytes()).hexdigest()
        (stage / "p0-input-hashes.json").write_text(json.dumps(inputs, indent=2), encoding="utf-8")
        return
    import sys
    command = [sys.executable, str(Path(__file__).with_name("run_full_runtime.py")),
               "--stage", str(stage), "--output", str(output), "--execute", "--no-validation"]
    if options.first_frame_evidence:
        command.append("--first-frame-evidence")
    subprocess.run(command,
                   cwd=Path(__file__).resolve().parents[3], check=True)


if __name__ == "__main__":
    main()

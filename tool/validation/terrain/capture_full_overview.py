"""Capture the real Full16 overview from an isolated Cooked stage."""

import argparse
import json
from pathlib import Path
import re
import subprocess

from PIL import Image


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--execute", action="store_true")
    options = parser.parse_args()
    stage, output = options.stage.resolve(), options.output.resolve()
    config = read(stage / "config/config.json")
    resources = Path(config["resourcePath"])
    if not resources.is_absolute():
        resources = stage / resources
    scene = read(resources / config["initScene"])
    placements = [entry for entry in scene["objects"] if entry["type"] == "terrain"]
    assert len(placements) == 16
    for entry in placements:
        terrain = read(resources / entry["terrainPath"])
        assert (resources / terrain["compositedVT"]["packagePath"]).is_file(), entry["name"]
    camera = next(entry for entry in scene["objects"] if entry["type"] == "camera")
    commands = [
        "camera pose " + " ".join(str(value) for value in camera["position"] + camera["look_at"]),
        "camera get", "terrainvt stats", "terrainchunk stats",
        f"screenshot {(output / 'full-island-far.bmp').as_posix()}",
        "terrainvt stats", "terrainchunk stats",
    ]
    argv = [
        str(stage / "bin/main_game.exe"), "--worker-thread-count", "2",
        "--frame-limit", "60", "--exit-after-frames", "300", "--no-dev-ui", "--validation",
        "--console-command-delay", "120",
    ]
    for command in commands:
        argv += ["--console-command", command]
    if not options.execute:
        print(json.dumps({"argv": argv, "cwd": str(stage), "rendererStarted": False}, indent=2))
        return
    output.mkdir(parents=True, exist_ok=False)
    (output / "command.json").write_text(json.dumps({"argv": argv, "cwd": str(stage)}, indent=2), encoding="utf-8")
    with (output / "runtime.log").open("wb") as log:
        process = subprocess.Popen(argv, cwd=stage, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        (output / "runtime.pid").write_text(str(process.pid))
        print(f"Full16 overview runtime PID {process.pid}; log={output / 'runtime.log'}", flush=True)
        try:
            exit_code = process.wait(timeout=1800)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise
    text = (output / "runtime.log").read_text(encoding="utf-8", errors="replace")
    report = {
        "exitCode": exit_code, "camera": camera, "scene": config["initScene"],
        "surfaces16": bool(re.search(r"TerrainVT generation=\d+ surfaces=16\b", text)),
        "cleanTeardown": "EngineLoop teardown complete" in text,
        "actualLod": re.findall(r"\[TerrainChunk\] drawnByLevel=.*", text),
        "worldTerrainStats": [line for line in text.splitlines() if line.startswith("TerrainVT generation=")],
        "shaderBuild": re.findall(r"Shader build:.*", text),
        "errors": re.findall(r"EngineLoop init failed:.*|Validation Error.*|VUID-\S+|THREADING.*", text),
    }
    screenshot = output / "full-island-far.bmp"
    if screenshot.is_file():
        Image.open(screenshot).save(output / "full-island-far.png")
    report["passed"] = exit_code == 0 and report["surfaces16"] and report["cleanTeardown"] and not report["errors"] and screenshot.is_file()
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    if not report["passed"]:
        raise RuntimeError("Full16 overview failed; keep original runtime log")


if __name__ == "__main__":
    main()

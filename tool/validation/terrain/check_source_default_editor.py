"""Run the actual source-root default Editor scene without a scene override."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

from PIL import Image


def digest(path):
    if not path.is_file():
        return None
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--execute", action="store_true")
    options = parser.parse_args()
    root, output = options.root.resolve(), options.output.resolve()
    executable = root / "build-vt-release/bin/main_editor.exe"
    config_path = root / "config/config.json"
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))
    assert config["initScene"].endswith("Full4x4_R8192_Default/SC_composited_vt_4x4.json")
    screenshot = output / "source-default-full.bmp"
    argv = [
        str(executable), "--worker-thread-count", "2", "--no-dev-ui",
        "--frame-limit", "60", "--exit-after-frames", "150", "--validation",
        "--console-command-delay", "90",
    ]
    for command in (
        "camera get", "terrainvt stats", "terrainchunk stats",
        f"screenshot {screenshot.as_posix()}", "terrainvt stats",
    ):
        argv += ["--console-command", command]
    if not options.execute:
        print(json.dumps({"argv": argv, "cwd": str(root)}, indent=2))
        return
    output.mkdir(parents=True, exist_ok=False)
    cache = root / "shader/spv/shader-build-cache.json"
    report = {
        "argv": argv, "cwd": str(root), "defaultScene": config["initScene"],
        "initialSceneOverride": False, "binarySha256": digest(executable),
        "configSha256Before": digest(config_path), "cacheSha256Before": digest(cache),
    }
    (output / "command.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    started = time.monotonic()
    with (output / "runtime.log").open("wb") as log:
        process = subprocess.Popen(argv, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        (output / "runtime.pid").write_text(str(process.pid))
        print(f"Source default Editor PID {process.pid}; log={output / 'runtime.log'}", flush=True)
        exit_code = process.wait(timeout=1800)
    text = (output / "runtime.log").read_text(encoding="utf-8", errors="replace")
    report.update({
        "exitCode": exit_code, "elapsedSeconds": time.monotonic() - started,
        "surfaces16": bool(re.search(r"TerrainVT generation=\d+ surfaces=16\b", text)),
        "cleanTeardown": "EngineLoop teardown complete" in text,
        "worldCommitted": "World/graph transaction committed:" in text,
        "shaderBuild": re.findall(r"Shader build:.*", text),
        "terrainStats": [line for line in text.splitlines() if line.startswith("TerrainVT generation=")],
        "actualLod": re.findall(r"\[TerrainChunk\] drawnByLevel=.*", text),
        "errors": re.findall(r"EngineLoop init failed:.*|Validation Error.*|VUID-\S+|THREADING.*", text),
        "configSha256After": digest(config_path), "cacheSha256After": digest(cache),
        "screenshot": str(screenshot),
    })
    if screenshot.is_file():
        Image.open(screenshot).save(output / "source-default-full.png")
    report["passed"] = (
        exit_code == 0 and report["surfaces16"] and report["cleanTeardown"]
        and report["worldCommitted"] and not report["errors"] and screenshot.is_file()
        and report["configSha256Before"] == report["configSha256After"]
    )
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2), flush=True)
    if not report["passed"]:
        raise RuntimeError("Source default Editor proof failed; preserve runtime.log")


if __name__ == "__main__":
    main()

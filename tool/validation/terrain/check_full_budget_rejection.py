"""Exercise GPU admission rejection in private Full inputs without baking pages."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def metadata_snapshot(resources):
    return {path.relative_to(resources).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (resources / "VT").glob("*/*.json")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--execute", action="store_true")
    options = parser.parse_args()
    stage, output = options.stage.resolve(), options.output.resolve()
    config = read(stage / "config/config.json")
    resources = Path(config["resourcePath"])
    scene = read(resources / config["initScene"])
    first = next(item for item in scene["objects"] if item["type"] == "terrain")
    target = resources / first["terrainPath"]
    original = target.read_bytes()
    changed = json.loads(original)
    changed["compositedVT"]["residentPages"] = 16777216
    report = {"scope": "Private first-TR GPU admission test before Capture/Bake; no source authors modified",
              "residentPagesFault": 16777216, "runs": {}, "rendererStarted": False}
    if not options.execute:
        print(json.dumps(report, indent=2))
        return
    output.mkdir(parents=True, exist_ok=False)
    before = metadata_snapshot(resources)
    try:
        target.write_text(json.dumps(changed, indent=2), encoding="utf-8")
        for worker in (1, 2):
            argv = [str(stage / "bin/main_editor.exe"), "--terrain-vt-cook", "--exit-after-frames", "2",
                    "--worker-thread-count", str(worker), "--validation", "--no-dev-ui"]
            with (output / f"worker{worker}.log").open("wb") as log:
                process = subprocess.run(argv, cwd=stage, stdout=log, stderr=subprocess.STDOUT,
                                         timeout=300, creationflags=subprocess.CREATE_NO_WINDOW)
            text = (output / f"worker{worker}.log").read_text(encoding="utf-8", errors="replace")
            result = {
                "exitCode": process.returncode, "explicitGpuAdmissionFailure": "surface exceeds World GPU admission budget" in text,
                "noWorldCommit": "World/graph transaction committed:" not in text,
                "noPageBake": "[TerrainVT Cook] surface=" not in text,
                "cleanTeardown": "EngineLoop teardown complete" in text,
                "validationClean": not re.search(r"VUID|Validation Error|THREADING", text),
            }
            report["runs"][str(worker)] = result
            assert process.returncode == 1 and all(value for key, value in result.items() if key != "exitCode"), result
    finally:
        target.write_bytes(original)
        report["privateTrRestored"] = target.read_bytes() == original
        report["cookMetadataUnchanged"] = metadata_snapshot(resources) == before
        report["rendererStarted"] = True
        (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    assert report["privateTrRestored"] and report["cookMetadataUnchanged"]
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

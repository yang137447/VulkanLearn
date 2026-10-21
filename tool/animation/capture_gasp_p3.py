"""Capture the formal textured level through normal loading and debug commands."""
import argparse
import copy
import json
from pathlib import Path

from build_gasp_level import write_json
from capture_gasp_p2 import capture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--resources", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    fixtures = args.resources.resolve() / "Generated/Validation" / output.name
    if output.exists() or fixtures.exists():
        parser.error("Choose new output and validation directories")
    output.mkdir(parents=True)
    scene = json.loads((args.resources / "Maps/SC_gasp/SC_gasp_playground.json").read_text(encoding="utf-8"))
    cases = [
        ("overview", None, None, []),
        ("logo", [0, 900, 1200], [0, 0, 0], []),
        ("detail", [2600, 450, 2200], [2600, 100, 0], []),
        ("baseColor", [2600, 450, 2200], [2600, 100, 0], ["debugview 1"]),
        ("normal", [2600, 450, 2200], [2600, 100, 0], ["debugview 3"]),
        ("roughness", [2600, 450, 2200], [2600, 100, 0], ["debugview 4"]),
    ]
    report = {"sourceScene": "Maps/SC_gasp/SC_gasp_playground.json",
              "processClosure": "owned capture processes terminated; not graceful teardown evidence",
              "cases": []}
    for name, position, target, commands in cases:
        fixture = copy.deepcopy(scene)
        # 相机在正式加载前固定，避免移动相机与截图同帧时的 GT/RT 延迟干扰。
        for obj in fixture["objects"]:
            if obj["type"] == "camera" and position is not None:
                obj["position"], obj["look_at"] = position, target
        fixture["description"] = "GASP 正式贴图场景的固定相机工程截图夹具，非论文验证；复用正式模型、材质与贴图。"
        relative = f"Generated/Validation/{output.name}/SC_{name}.json"
        write_json(args.resources / relative, fixture)
        capture(root / "build/bin/main.exe", root, output, name, relative, commands, 2, 120, False)
        report["cases"].append({"name": name, "scene": relative, "commands": commands})
    write_json(output / "capture.json", report)


if __name__ == "__main__":
    main()

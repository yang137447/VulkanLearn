"""Capture the real Female bind and two GASP Walk poses through production paths."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "animation"))
from capture_gasp_p2 import capture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    executable = root / "build/bin/main.exe"
    scene = "Maps/SC_tressfx_female/SC_tressfx_female_"
    display = ["tonemap 3", "bloom strength 0"]
    images = {}
    config_path = root / "config/config.json"
    original = config_path.read_bytes()
    report = {"completed": False,
              "processClosure": "Owned processes terminated; not graceful teardown evidence"}
    try:
        images["bind"] = capture(executable, root, output, "bind", scene + "bind.json",
                                 display, delay=3)
        images["idle"] = capture(executable, root, output, "idle", scene + "skinned.json",
                                 ["animation seek 0.25", "animation get", *display], delay=3)
        config = json.loads(original)
        # 仅截图会话自动 Play；正式编辑关卡保持用户控制 Play/Pause/Stop，finally 恢复原始字节。
        config["scripts"] = {"enabled": True, "autoPlay": True,
                             "runtimeDirectory": str(root / "build/bin/managed"),
                             "assembly": str(root / "build/bin/managed/Gameplay.dll")}
        config_path.write_text(json.dumps(config, indent=2), encoding="utf-8")
        for time in (0.25, 0.75):
            name = f"walk{int(time * 100):03}"
            images[name] = capture(
                executable, root, output, name, scene + "skinned_blueprint.json",
                [f"animation seek {time}", "animation get", *display], delay=3)
        for name, image in images.items():
            Image.fromarray(image.astype(np.uint8)).save(output / f"{name}.png")
        # 去掉 HUD，仅对照中央角色；差异是烟测，不代替足底接触或重定向质量验收。
        delta = np.abs(images["walk025"][140:630, 400:880] -
                       images["walk075"][140:630, 400:880])
        report["walkPoseChangedPixelsOver4"] = int((delta.max(axis=2) > 4).sum())
        if report["walkPoseChangedPixelsOver4"] < 1000:
            raise RuntimeError("Walk poses did not visibly change")
        for name in ("walk025", "walk075"):
            log = (output / f"{name}.log").read_text(encoding="utf-8")
            if "id=M_Neutral_Walk_Loop_F" not in log or "playing=0" not in log:
                raise RuntimeError(f"{name}: missing real paused Walk playback evidence")
            if "Animation control rejected" in log:
                raise RuntimeError(f"{name}: animation command rejected")
        report["rootMode"] = "Production Blueprint requests Animation.Play(inPlace: true)"
        report["completed"] = True
    finally:
        config_path.write_bytes(original)
        report["configRestored"] = config_path.read_bytes() == original
        report["configSha256"] = hashlib.sha256(original).hexdigest()
        (output / "capture.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

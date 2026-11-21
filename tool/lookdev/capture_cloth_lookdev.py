"""Review cloth in the existing article lookdev HDRI and fixed display preset."""

import argparse
import json
from pathlib import Path
import subprocess
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT.parent / "VukanLearnResources"
OUTPUT = ROOT / "artifacts/cloth-lookdev"
BASE = "Maps/SC_lookdev_article/SC_lookdev_article.json"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sun-lux", type=float, default=0.0)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    base = json.loads((ASSETS / BASE).read_text(encoding="utf-8"))
    if args.sun_lux > 0:
        base["objects"].append({
            "name": "ClothLookdevSun", "type": "directionalLight",
            "position": [-660, 700, -355], "rotation": [-35, -125, 0],
            "color": [1, 1, 1], "intensity": args.sun_lux, "intensityUnit": "lux",
            "shadow": {"dynamicShadowDistance": 3000},
        })
        target = ASSETS / "Generated/ClothLookdevSun/SC_baseline.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(base, ensure_ascii=False, indent=2), encoding="utf-8")
        baseline = target.relative_to(ASSETS).as_posix()
    else:
        baseline = BASE
    scenes = {"baseline": baseline}
    for variant in ["satin", "velvet", "slantedVelvet", "pbr"]:
        scene = json.loads(json.dumps(base))
        scene["name"] = f"Cloth Lookdev - {variant}"
        scene["description"] = (
            "复用正式 lookdev 的 HDRI、强度、曝光与显示预设审阅布料；"
            "服装原尺寸不变，仅调整放置变换，非认证色卡或论文数值校准。"
        )
        scene["objects"].append({
            "name": "ClothReview", "type": "mesh",
            "modelPath": f"Maps/SC_cloth_showcase/Meshes/SM_cloth_3d66Dress_{variant}.json",
            "position": [10, 0, -125], "rotation": [0, -125, 0],
            "scale": [100, 100, 100],
        })
        folder = "ClothLookdevSun" if args.sun_lux > 0 else "ClothLookdev"
        target = ASSETS / f"Generated/{folder}/SC_{variant}.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(scene, ensure_ascii=False, indent=2), encoding="utf-8")
        scenes[variant] = target.relative_to(ASSETS).as_posix()
    records = {}
    for name, scene in scenes.items():
        screenshot = output / f"{name}.bmp"
        with (output / f"{name}.log").open("wb") as log:
            subprocess.run([
                str(ROOT / "build-cloth-csharp/bin/main_game.exe"),
                "--initial-scene", scene, "--no-dev-ui", "--worker-thread-count", "1",
                "--frame-limit", "60", "--exit-after-frames", "300",
                "--console-command", "tonemap 3",
                "--console-command", "bloom strength 0",
                "--console-command", f"screenshot {screenshot}",
                "--console-command-delay", "240",
            ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        image = Image.open(screenshot)
        image.save(output / f"{name}.png")
        text = (output / f"{name}.log").read_text(encoding="utf-8", errors="replace")
        records[name] = {"scene": scene, "sunLux": args.sun_lux, "ev100": 0, "hdriIntensity": 1, "errors": [
            line for line in text.splitlines()
            if "Validation Error:" in line or "[Diagnostics][Error]" in line]}
        print(name, flush=True)
    sheet = Image.new("RGB", (1280, 760), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for index, name in enumerate(["satin", "velvet", "slantedVelvet", "pbr"]):
        x, y = (index % 2) * 640, (index // 2) * 380
        sheet.paste(Image.open(output / f"{name}.png").resize((640, 360)), (x, y + 20))
        draw.text((x + 10, y + 4), name, fill="white")
    sheet.save(output / "comparison.png")
    (output / "results.json").write_text(json.dumps(records, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()

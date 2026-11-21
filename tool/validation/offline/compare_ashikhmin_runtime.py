"""Generate and render matched CPU/GPU cloth cases with one camera/display contract."""

import argparse
import json
from pathlib import Path
import subprocess
import sys

from PIL import Image, ImageDraw
from render_ashikhmin_paper_cases import CASES, ROOT


ASSETS = ROOT.parent / "VukanLearnResources"
DESTINATION = ASSETS / "Generated/ClothParity"


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")


def reference(path):
    return path.relative_to(ASSETS).as_posix()


def generate(name, variant, scene_name):
    scene = read(ASSETS / f"Maps/{scene_name}/{scene_name}.json")
    scene["name"] = f"Cloth CPU/GPU parity - {name}"
    scene["description"] = (
        "Ashikhmin 2000 布料 CPU/GPU 同条件对照：同模型、相机、单灯、反射率，"
        "EV100 9、补偿 0、ACES；无环境/贴图/间接光，非论文原图像素复现。"
    )
    offline = read(ASSETS / f"Maps/{scene_name}/{scene_name}.json")
    for entry in scene["objects"]:
        if entry["type"] == "camera":
            entry["exposure"]["ev100"] = 9.0
            entry["exposure"]["compensation"] = 0.0
        if entry["type"] != "mesh":
            continue
        mesh = read(ASSETS / entry["modelPath"])
        cpu_mesh = read(ASSETS / entry["modelPath"])
        for index, slot in enumerate(mesh["materialSlots"]):
            cpu_mi = read(ASSETS / slot["materialInstancePath"])
            mother = read(ROOT / cpu_mi["material"])
            model = mother["offlineModel"]
            params = {key: value["default"] for key, value in mother["parameters"].items()}
            params.update(cpu_mi.get("parameters", {}))
            if model == "Lambert":
                runtime_material = "shader/glsl/M_sharedLambert.json"
                runtime_params = {"u_baseColor": params["u_baseColor"]}
            else:
                suffix = {"satin": "Satin", "velvet": "Velvet",
                          "slantedVelvet": "SlantedVelvet"}[variant]
                runtime_material = f"shader/glsl/M_clothAshikhmin{suffix}.json"
                runtime_params = {"u_baseColor": params["u_baseColor"]}
                if "u_fiberRotationDegrees" in params:
                    runtime_params["u_fiberRotationDegrees"] = params["u_fiberRotationDegrees"]
                if model in ("AshikhminSatin", "AshikhminSlantedVelvet"):
                    cpu_mi.setdefault("parameters", {})["u_useImportedTangents"] = 1.0
            defaults = read(ROOT / runtime_material)["parameters"]
            runtime_params = {key: value for key, value in runtime_params.items()
                              if value != defaults[key]["default"]}
            runtime_mi = {
                "name": f"MI_{name}_{entry['name']}_{index}", "type": "materialInstance",
                "material": runtime_material,
            }
            if runtime_params:
                runtime_mi["parameters"] = runtime_params
            if model != "Lambert":
                runtime_mi["textures"] = {"clothLut": "Common/Textures/T_clothAshikhminLut.json"}
            target = DESTINATION / name / f"MI_gpu_{entry['name']}_{index}.json"
            write(target, runtime_mi)
            slot["materialInstancePath"] = reference(target)
            cpu_target = DESTINATION / name / f"MI_cpu_{entry['name']}_{index}.json"
            write(cpu_target, cpu_mi)
            cpu_mesh["materialSlots"][index]["materialInstancePath"] = reference(cpu_target)
        target = DESTINATION / name / f"SM_gpu_{entry['name']}.json"
        write(target, mesh)
        entry["modelPath"] = reference(target)
        cpu_target = DESTINATION / name / f"SM_cpu_{entry['name']}.json"
        write(cpu_target, cpu_mesh)
        next(item for item in offline["objects"] if item["name"] == entry["name"])["modelPath"] = reference(cpu_target)
    for entry in offline["objects"]:
        if entry["type"] == "camera":
            entry["exposure"]["ev100"] = 9.0
            entry["exposure"]["compensation"] = 0.0
    offline["description"] = scene["description"]
    gpu_path = DESTINATION / name / "SC_gpu.json"
    cpu_path = DESTINATION / name / "SC_cpu.json"
    write(gpu_path, scene)
    write(cpu_path, offline)
    return reference(gpu_path), reference(cpu_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=int, default=64)
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--output", type=Path, default=ROOT / "artifacts/cloth-parity")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = {}
    sheet = Image.new("RGB", (880 * min(2, len(args.cases)),
                              555 * ((len(args.cases) + 1) // 2)), (235, 235, 235))
    draw = ImageDraw.Draw(sheet)
    for case_index, name in enumerate(args.cases):
        variant, scene_name = CASES[name]
        gpu_scene, cpu_scene = generate(name, variant, scene_name)
        output = args.output / name
        output.mkdir(exist_ok=True)
        screenshot = output / "runtime.bmp"
        with (output / "runtime_capture.log").open("wb") as log:
            subprocess.run([
                str(ROOT / "build-cloth-csharp/bin/main_game.exe"),
                "--initial-scene", gpu_scene, "--worker-thread-count", "1", "--no-dev-ui",
                "--frame-limit", "60", "--exit-after-frames", "160",
                "--console-command", "bloom strength 0", "--console-command", "tonemap 3",
                "--console-command", f"screenshot {screenshot.resolve()}",
                "--console-command-delay", "100",
            ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        subprocess.run([
            sys.executable, str(ROOT / "tool/cloth-intake/render_offline_showcase.py"),
            "--variants", variant, "--scene", cpu_scene,
            "--samples", str(args.samples), "--bounces", "0",
            "--display-exposure", "1", "--output", str(output),
        ], cwd=ROOT, check=True)
        result = read(output / "results.json")[0]
        manifest = read(Path(result["export"]) / "manifest.json")
        expected_exposure = 2 ** -9 / 1.2
        if abs(manifest["display"]["exposure"] - expected_exposure) > 1e-9:
            raise ValueError("CPU display exposure differs from EV100 9")
        if manifest["display"]["mapping"] != "ACES fitted + sRGB":
            raise ValueError("CPU tone mapping differs from runtime ACES")
        images = [Image.open(screenshot).convert("RGB"),
                  Image.open(Path(result["export"]) / "combined.ppm").convert("RGB")]
        crop = (420, 170, 860, 690)
        error_sum = 0
        mask_count = 0
        backgrounds = [image.getpixel((0, 0)) for image in images]
        errors = []
        for left, right in zip(images[0].crop(crop).getdata(), images[1].crop(crop).getdata()):
            if all(max(abs(channel - background) for channel, background in zip(pixel, backdrop)) > 3
                   for pixel, backdrop in zip([left, right], backgrounds)):
                channel_errors = [abs(first - second) for first, second in zip(left, right)]
                errors.extend(channel_errors)
                error_sum += sum(channel_errors)
                mask_count += 1
        errors.sort()
        records[name] = {
            "export": result["export"], "gpuScene": gpu_scene, "cpuScene": cpu_scene,
            "display": manifest["display"], "clothMaskMeanAbsoluteRgb8Error":
                error_sum / max(1, mask_count * 3),
            "clothMaskP95AbsoluteRgb8Error": errors[int((len(errors) - 1) * 0.95)] if errors else None,
            "maskPixels": mask_count,
            "maskNote": "Shared crop; intersection of pixels differing from each image's background by >3. Appearance diagnostic, not a BRDF tolerance.",
            "runtimeValidationMessages": [
                line for line in (output / "runtime_capture.log").read_text(
                    encoding="utf-8", errors="replace").splitlines()
                if "Validation Error:" in line or "[Diagnostics][Error]" in line
            ],
        }
        for side, image in enumerate(images):
            image.crop(crop).save(output / ("runtime.png" if side == 0 else "offline.png"))
            x = (case_index % 2) * 880 + side * 440
            y = (case_index // 2) * 555
            sheet.paste(image.crop(crop), (x, y + 35))
            draw.text((x + 8, y + 10), f"{name} / {'runtime' if side == 0 else 'offline'}", fill="black")
        print(json.dumps({name: records[name]}), flush=True)
    sheet.save(args.output / "comparison.png")
    write(args.output / "comparison.json", records)


if __name__ == "__main__":
    main()

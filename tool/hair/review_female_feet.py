"""Build and capture real Female retargeted feet through production VL playback."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "animation"))
from capture_gasp_p2 import capture


MAP = Path("Maps/SC_tressfx_female")
SCENE = MAP / "SC_tressfx_female_feet.json"
CAMERAS = {
    "oblique": ([240, 48, 195], [0, 38, 0]),
    "side": ([310, 48, 0], [0, 38, 0]),
    "front": ([0, 48, 260], [0, 38, 0]),
}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=4) + "\n", encoding="utf-8")


def grid_obj():
    # 只有 Y=0 上的细条，无实体地板/碰撞；穿到零线下的鞋仍可见，不被平面遮住。
    vertices, faces = [], []
    for axis in (0, 2):
        for coordinate in range(-80, 81, 10):
            first = len(vertices) + 1
            for along, across in ((-80, -0.04), (80, -0.04), (80, 0.04), (-80, 0.04)):
                point = [0.0, 0.0, 0.0]
                point[axis] = along
                point[2 - axis] = coordinate + across
                vertices.append(point)
            faces.append((first, first + 1, first + 2, first + 3))
    lines = ["o FeetReferenceGrid", "usemtl grid"]
    lines.extend("v " + " ".join(str(value) for value in point) for point in vertices)
    lines.extend("f " + " ".join(str(index) for index in face) for face in faces)
    return "\n".join(lines) + "\n"


def build(resource):
    scene = json.loads((resource / MAP / "SC_tressfx_female_skinned_blueprint.json")
                       .read_text(encoding="utf-8"))
    scene["name"] = "Female retarget feet - 10 cm grid"
    scene["description"] = (
        "Female/GASP 运行时重定向真实蒙皮脚部审阅；Play 后原地 Walk。"
        "10 cm 细线网格在 Y=0，仅为厘米视觉参考，无碰撞/足底 IK/接触修正。"
        "检查膝踝、鞋尖、鞋底及穿地；学习与资产审阅，不是论文或 UE 等价验收。")
    camera = next(obj for obj in scene["objects"] if obj["type"] == "camera")
    camera["name"] = "FeetReviewCamera"
    camera["position"], camera["look_at"] = CAMERAS["oblique"]
    camera["fov"] = 42
    prefix = MAP.as_posix()
    scene["objects"].append({
        "name": "ReferenceGrid10cm", "type": "mesh",
        "modelPath": f"{prefix}/Meshes/SM_female_feet_grid.json",
        "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1],
    })
    scene["objects"].append({
        "name": "FeetReviewFill", "type": "pointLight", "position": [160, 75, 90],
        "rotation": [0, 0, 0], "color": [1, 1, 1], "intensity": 6, "intensityUnit": "cd",
    })
    outputs = {
        SCENE: scene,
        MAP / "Meshes/SM_female_feet_grid.json": {
            "name": "Female feet 10 cm reference grid", "type": "mesh",
            "modelDataPath": f"{prefix}/Source/female_feet_grid.obj",
            "materialSlots": [{"name": "grid", "materialInstancePath":
                              f"{prefix}/Materials/MI_female_feet_grid.json"}],
        },
        MAP / "Materials/MI_female_feet_grid.json": {
            "name": "Feet reference grid", "type": "materialInstance",
            "material": "shader/glsl/M_unlit.json",
            "renderStateOverrides": {"cullMode": "None"},
            "parameters": {"u_tintColor": [0.06, 0.22, 0.18, 1]},
        },
    }
    source = MAP / "Source/female_feet_grid.obj"
    # 新建资产前一次检查所有路径；重复运行不覆盖作者后来调整过的审阅关卡。
    for relative in [*outputs, source]:
        if (resource / relative).exists():
            raise ValueError(f"Review output already exists: {relative}")
    for relative, value in outputs.items():
        write_json(resource / relative, value)
    (resource / source).write_text(grid_obj(), encoding="ascii")
    return scene


def labelled(image, text):
    result = Image.fromarray(image.astype(np.uint8))
    draw = ImageDraw.Draw(result)
    draw.rectangle((0, 0, result.width, 42), fill=(22, 24, 26))
    draw.text((12, 12), text, fill=(245, 245, 245))
    return result


def capture_review(root, resource, output):
    output.mkdir(parents=True, exist_ok=False)
    original = (root / "config/config.json").read_bytes()
    # Ctrl+C/宿主终止可能跳过 finally；持久备份允许按字节恢复截图会话前的配置。
    (output / "config-original.bin").write_bytes(original)
    profile_path = resource / MAP / "Retarget/RT_Female_From_GASP.json"
    report = {
        "completed": False, "scene": SCENE.as_posix(),
        "sceneSha256": hashlib.sha256((resource / SCENE).read_bytes()).hexdigest(),
        "retargetSha256": hashlib.sha256(profile_path.read_bytes()).hexdigest(),
        "reference": "Y=0 cm, grid 10 cm; visual lines only, no floor collision or IK",
        "sampleScope": "First 0.875 seconds of the 4-second Walk clip; not full-clip contact validation",
        "processClosure": "Owned capture processes terminated; not graceful teardown evidence",
        "frames": [],
    }
    try:
        config = json.loads(original)
        config["ui"]["enabled"] = False
        # 截图会话独占脚本自动 Play；普通 mesh 的原根会走出近景，因此使用正式 BP 的 inPlace 请求。
        config["scripts"] = {
            "enabled": True, "autoPlay": True,
            "runtimeDirectory": str(root / "build/bin/managed"),
            "assembly": str(root / "build/bin/managed/Gameplay.dll"),
        }
        write_json(root / "config/config.json", config)
        frames = []
        times = np.arange(8) / 8
        for view, sample_times in (("side", times), ("front", (0.25, 0.75))):
            position, target = CAMERAS[view]
            camera = "camera pose " + " ".join(str(value) for value in [*position, *target])
            for time in sample_times:
                name = f"{view}{round(time * 1000):04}"
                image = capture(
                    root / "build/bin/main.exe", root, output, name, SCENE.as_posix(),
                    [camera, f"animation seek {time}", "animation get",
                     "tonemap 3", "bloom strength 0"], delay=3)
                log = (output / f"{name}.log").read_text(encoding="utf-8")
                if "id=M_Neutral_Walk_Loop_F" not in log or "playing=0" not in log or \
                        "Animation control rejected" in log:
                    raise RuntimeError(f"Missing paused production Walk evidence: {name}")
                frame = labelled(image, f"Female / GASP Walk | {view} | t={time:.3f}s | Y=0 grid, 10 cm | no IK")
                frame.save(output / f"{name}.png")
                report["frames"].append({"name": name, "view": view, "timeSeconds": float(time)})
                if view == "side":
                    frames.append(frame)
        # GIF 为真实暂停采样帧的等间隔回放，不是录屏，也不插值或生成中间姿态。
        frames[0].save(output / "feet-side.gif", save_all=True, append_images=frames[1:],
                       duration=125, loop=0, optimize=False)
        sheet = Image.new("RGB", (1280, 720), (22, 24, 26))
        for index in range(4):
            sheet.paste(frames[index * 2].resize((640, 360)), ((index % 2) * 640, (index // 2) * 360))
        sheet.save(output / "feet-contact-sheet.png")
        report["sampleIntervalSeconds"] = 0.125
        report["completed"] = True
    finally:
        (root / "config/config.json").write_bytes(original)
        report["configRestored"] = (root / "config/config.json").read_bytes() == original
        write_json(output / "capture.json", report)
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Create new formal feet review assets")
    parser.add_argument("--capture", type=Path, help="Capture into a new directory")
    args = parser.parse_args()
    if not args.build and args.capture is None:
        parser.error("Choose --build and/or --capture")
    root = Path(__file__).resolve().parents[2]
    config = json.loads((root / "config/config.json").read_text(encoding="utf-8"))
    resource = (root / config["resourcePath"]).resolve()
    if args.build:
        build(resource)
        print(f"Created {SCENE.as_posix()}", flush=True)
    if args.capture is not None:
        capture_review(root, resource, args.capture.resolve())


if __name__ == "__main__":
    main()

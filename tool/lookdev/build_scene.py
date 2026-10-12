"""Author the opt-in lookdev scene and its deterministic neutral studio assets.

Only Python's standard library is required. Binary/geometry products live in
resourcePath/Generated/Lookdev; scene/MI/SM JSON live in Maps/SC_lookdev.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path
import struct
import zlib


ROOT = Path(__file__).resolve().parents[2]
SCENE = "Maps/SC_lookdev"
GENERATED = "Generated/Lookdev"
# GPU SH9/cubemap calibration, measured with the fixed EV0/ACES view.
# This gain belongs to this studio asset, not to the renderer's exposure formula.
STUDIO_CALIBRATION_GAIN = 0.967593067655343

# Fixed display-sRGB reference swatches, not spectral measurements of a
# certified chart. Convert exactly once to linear material reflectance.
PATCHES = [
    ("dark_skin", (115, 82, 68)), ("light_skin", (194, 150, 130)),
    ("blue_sky", (98, 122, 157)), ("foliage", (87, 108, 67)),
    ("blue_flower", (133, 128, 177)), ("bluish_green", (103, 189, 170)),
    ("orange", (214, 126, 44)), ("purplish_blue", (80, 91, 166)),
    ("moderate_red", (193, 90, 99)), ("purple", (94, 60, 108)),
    ("yellow_green", (157, 188, 64)), ("orange_yellow", (224, 163, 46)),
    ("blue", (56, 61, 150)), ("green", (70, 148, 73)),
    ("red", (175, 54, 60)), ("yellow", (231, 199, 31)),
    ("magenta", (187, 86, 149)), ("cyan", (8, 133, 161)),
    ("white", (243, 243, 243)), ("neutral_8", (200, 200, 200)),
    ("neutral_65", (160, 160, 160)), ("neutral_5", (122, 122, 122)),
    ("neutral_35", (85, 85, 85)), ("black", (52, 52, 52)),
]


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def srgb_to_linear(value: float) -> float:
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def linear_to_srgb(value: float) -> float:
    return 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055


def aces(value: float) -> float:
    return value * (2.51 * value + 0.03) / (value * (2.43 * value + 0.59) + 0.14)


def inverse_aces(value: float) -> float:
    low, high = 0.0, 8.0
    for _ in range(60):
        middle = (low + high) * 0.5
        if aces(middle) < value:
            low = middle
        else:
            high = middle
    return (low + high) * 0.5


def write_studio(path: Path, gain: float) -> dict:
    width, height = 1024, 512
    values = []
    front_irradiance = 0.0
    solid_angle_step = (2 * math.pi / width) * (math.pi / height)
    for row in range(height):
        theta = math.pi * (row + 0.5) / height
        sin_theta, y = math.sin(theta), math.cos(theta)
        for column in range(width):
            phi = 2 * math.pi * ((column + 0.5) / width - 0.5)
            x, z = sin_theta * math.cos(phi), sin_theta * math.sin(phi)
            value = 0.08 + 0.10 * max(y, 0)
            if z > 0:
                px, py = x / z, y / z
                # Neutral rectangular reflectors are baked into the HDRI, not
                # realtime area lights. Keep lighting and reflections identical.
                if -1.8 < px < -0.30 and 0.25 < py < 1.65:
                    value += 5.0
                if 0.65 < px < 1.20 and -0.1 < py < 1.3:
                    value += 1.3
            if y > 0.90:
                value += 0.65
            if y < -0.12:
                floor_x, floor_z = 2.0 * x / -y, 2.0 * z / -y
                checker = (math.floor(floor_x * 1.5) + math.floor(floor_z * 1.5)) % 2
                value = 0.05 if checker else 0.075
            values.append(value)
            front_irradiance += value * max(z, 0) * sin_theta * solid_angle_step
    # At EV0 the renderer applies 1/1.2. Calibrate a front-facing Lambert
    # 18% card to display-linear 0.18 after the project's ACES fitted curve.
    scale = inverse_aces(0.18) * 1.2 * math.pi / (0.18 * front_irradiance) * gain
    data = bytearray(f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode())
    for row in range(height):
        channels = [bytearray() for _ in range(4)]
        for value in values[row * width:(row + 1) * width]:
            mantissa, exponent = math.frexp(value * scale)
            component = int(mantissa * 256)
            for channel in channels[:3]:
                channel.append(component)
            channels[3].append(exponent + 128)
        data.extend(bytes((2, 2, width >> 8, width & 255)))
        for channel in channels:
            for start in range(0, width, 128):
                block = channel[start:start + 128]
                data.append(len(block))
                data.extend(block)
    path.write_bytes(data)
    return {"width": width, "height": height, "scale": scale,
            "frontLambertIrradianceOverPi": front_irradiance * scale / math.pi,
            "gpuCalibrationGain": gain}


def write_sphere(path: Path) -> None:
    segments, rings = 128, 64
    lines = ["o LookdevSphere"]
    for ring in range(rings + 1):
        theta = math.pi * ring / rings
        for segment in range(segments + 1):
            phi = 2 * math.pi * segment / segments
            x, y, z = math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi)
            lines += [f"v {x:.9f} {y:.9f} {z:.9f}",
                      f"vt {segment / segments:.9f} {1 - ring / rings:.9f}",
                      f"vn {x:.9f} {y:.9f} {z:.9f}"]
    for ring in range(rings):
        for segment in range(segments):
            a = ring * (segments + 1) + segment + 1
            b, c, d = a + 1, a + segments + 1, a + segments + 2
            if ring > 0:
                lines.append("f " + " ".join(f"{i}/{i}/{i}" for i in (a, b, c)))
            if ring < rings - 1:
                lines.append("f " + " ".join(f"{i}/{i}/{i}" for i in (b, d, c)))
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def write_gradient(path: Path) -> None:
    width, height = 2048, 8
    row = bytearray()
    for x in range(width):
        reflectance = 0.9 + (0.003 - 0.9) * x / (width - 1)
        value = round(linear_to_srgb(reflectance) * 255)
        row.extend((value, value, value))
    raw = (b"\0" + row) * height

    def chunk(name: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def build(resource: Path, gain: float) -> None:
    scene_root = resource / SCENE
    generated = resource / GENERATED
    generated.mkdir(parents=True, exist_ok=True)
    environment = write_studio(generated / "studio_neutral.hdr", gain)
    write_sphere(generated / "sphere.obj")
    write_gradient(generated / "reflectance_ramp.png")
    (generated / "quad.obj").write_text(
        "o LookdevQuad\nv -0.5 -0.5 0\nv 0.5 -0.5 0\nv 0.5 0.5 0\nv -0.5 0.5 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n", encoding="ascii")
    objects = []
    material_definitions = {
        name: json.loads((ROOT / f"shader/glsl/M_{name}.json").read_text(encoding="utf-8"))
        for name in ("pbr", "unlit")
    }

    def add(name: str, position: list, scale: list, color: list,
            roughness: float = 1.0, metallic: float = 0.0,
            sphere: bool = False, unlit: bool = False, gradient: bool = False) -> None:
        material = {"name": name, "type": "materialInstance",
                    "material": f"shader/glsl/M_{'unlit' if unlit else 'pbr'}.json",
                    "parameters": {"u_tintColor": [*color, 1]}}
        if not unlit:
            material["parameters"]["u_pbrFactors"] = [roughness, metallic, 1, 0]
            # Mirror metal retains its own F0; all dielectric chart surfaces are diffuse-only.
            material["parameters"]["u_specular"] = 0.0
        if gradient:
            material["macros"] = {"USE_ALBEDO_MAP": 1}
            material["textures"] = {"albedoMap": f"{SCENE}/Textures/T_reflectance_ramp.json"}
        defaults = material_definitions["unlit" if unlit else "pbr"]["parameters"]
        material["parameters"] = {
            key: value for key, value in material["parameters"].items()
            if value != defaults[key]["default"]
        }
        if not material["parameters"]:
            del material["parameters"]
        write_json(scene_root / f"Materials/MI_{name}.json", material)
        write_json(scene_root / f"Meshes/SM_{name}.json", {
            "name": name, "type": "mesh",
            "modelDataPath": f"{GENERATED}/{'sphere' if sphere else 'quad'}.obj",
            "materialSlots": [{"name": "Default",
                               "materialInstancePath": f"{SCENE}/Materials/MI_{name}.json"}]})
        objects.append({"name": name, "type": "mesh",
                        "modelPath": f"{SCENE}/Meshes/SM_{name}.json",
                        "position": position, "rotation": [0, 0, 0], "scale": scale})

    add("mirror_ball", [-49, 68, 2], [43, 43, 43], [0.95] * 3,
        roughness=0.025, metallic=1, sphere=True)
    add("gray_ball", [49, 68, 2], [43, 43, 43], [0.18] * 3, sphere=True)
    add("chart_frame", [0, -64, -1], [204, 180, 1], [0.003] * 3)
    for index, (name, rgb) in enumerate(PATCHES):
        column, row = index % 6, index // 6
        add(f"patch_{index + 1:02}_{name}", [-82.5 + 33 * column, 7 - 32 * row, 0],
            [30, 29, 1], [srgb_to_linear(c / 255) for c in rgb])
    add("reflectance_ramp", [0, -130, 0], [195, 37, 1], [1, 1, 1], gradient=True)
    # A small central meter below the two spheres keeps the 0.18 anchor explicit.
    for index, roughness in enumerate((0.05, 0.5, 1.0)):
        add(f"gray_meter_{index}", [-7 + 7 * index, 32, 1], [6, 6, 1],
            [0.18] * 3, roughness=roughness)
    backdrop = inverse_aces(0.04) * 1.2
    add("neutral_backdrop", [0, 0, -65], [3000, 3000, 1], [backdrop] * 3, unlit=True)
    objects += [{
        "name": "LookdevCamera", "type": "camera", "fov": 44,
        "near_clip": 1, "far_clip": 3000,
        "position": [0, -20, 630], "look_at": [0, -20, 0],
        "rotation": [0, 0, 0], "scale": [1, 1, 1],
        "exposure": {"mode": "ev100", "ev100": 0, "aperture": 4,
                     "shutterSeconds": 0.008, "iso": 100, "compensation": 0}},
        {"name": "NeutralStudio", "type": "environment",
         "environment": {"type": "hdri", "hdrPath": f"{GENERATED}/studio_neutral.hdr",
                         "cubeSize": 512, "intensity": 1}}]
    write_json(scene_root / "Textures/T_reflectance_ramp.json", {
        "name": "Lookdev linear reflectance ramp (sRGB encoded)", "type": "texture",
        "source": f"{GENERATED}/reflectance_ramp.png",
        "colorSpace": "srgb", "mipmaps": True, "filter": "linear", "wrapMode": "clamp"})
    scene = {
        "name": "Lookdev - Neutral Studio", "type": "scene", "worldUnit": "cm",
        "description": "标准资产审阅基线：参考李文磊《 关于建立资产统一审查预览环境的一些说明 》的固定曝光、18%灰板与中性IBL方法；镜面球、漫反射灰球、24色参考卡与连续反射率灰阶。EV100=0，启动脚本固定ACES fitted并关闭Bloom。自建中性棚HDR，不是UE截图或认证实物色卡的色度复刻；不含TAA/FSR。",
        "objects": objects}
    write_json(scene_root / "SC_lookdev.json", scene)
    # External verification fixture: same authored materials, only illumination
    # changes. This is not an engine runtime test or a second material pipeline.
    direct_scene = copy.deepcopy(scene)
    direct_scene["name"] = "Lookdev - Direct Calibration Probe"
    direct_scene["description"] = "外部截图验证：Lambert 余弦定律的正面方向光灰板，EV0/ACES fitted 下目标显示线性0.18；验证Specular=0直接光不随粗糙度变化，不作为正式lookdev预设。"
    direct_scene["objects"][-1]["environment"] = {"type": "none"}
    direct_scene["objects"].append({
        "name": "CalibrationDirectional", "type": "directionalLight",
        "position": [0, 0, 500], "rotation": [0, 0, 0], "color": [1, 1, 1],
        "intensity": inverse_aces(0.18) * 1.2 * math.pi / 0.18,
        "intensityUnit": "lux", "shadow": {"dynamicShadowDistance": 1500}})
    write_json(generated / "SC_lookdev_direct.json", direct_scene)
    write_json(generated / "authoring_manifest.json", {
        "scene": f"{SCENE}/SC_lookdev.json", "environment": environment,
        "display": {"ev100": 0, "compensation": 0, "toneMapping": 3, "bloomStrength": 0},
        "reference": {"grayReflectance": 0.18, "targetDisplayLinearGray": 0.18,
                      "backgroundDisplayLinear": 0.04, "paletteEncoding": "sRGB",
                      "palette": [{"name": name, "rgb8": rgb} for name, rgb in PATCHES]},
        "limitations": ["Synthetic HDRI; not measured absolute luminance",
                        "Reference palette; not a certified spectral ColorChecker",
                        "GPU SH projection and RGBE quantization require screenshot verification"]})
    print(f"Authored {len(objects)} objects: {scene_root / 'SC_lookdev.json'}")
    print(f"Studio calibration: {environment}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path)
    parser.add_argument("--gain", type=float, default=STUDIO_CALIBRATION_GAIN)
    args = parser.parse_args()
    resource = args.resource_root or Path(json.loads((ROOT / "config/config.json").read_text())["resourcePath"])
    if not math.isfinite(args.gain) or args.gain <= 0:
        parser.error("--gain must be finite and positive")
    build(resource, args.gain)

"""把 UE 导出的 Megascans 贴图打包成 VulkanLearn 的地形层纹理。

输入（export_megascans*.py 的产物）：

    T_<Name>_A.png  albedo
    T_<Name>_N.png  normal
    T_<Name>_R.png  roughness
    T_<Name>_H.png  高度（16-bit 优先；没有时尝试 _D displacement）
    T_<Name>_D.png  displacement（Megascans 的另一套命名）

输出（每个层四张，尺寸统一到 ``--size``）：

    <key>_albedo.png   RGBA8 sRGB：RGB=albedo，A=255
    <key>_normal.png   RGBA8 linear：RG=normal.xy，B/A=255
    <key>_orm.png      RGBA8 linear：R=AO，G=roughness，B=metallic(0)，A=255
    <key>_height16.png R16：16-bit 高度，均值归一化到 0.5

规范见 ``documents/plan/rendering/terrain-texture-packing.md``。
没有真实高度数据的层写平坦高度（32768）、AO=255，不拿 albedo 亮度冒充。
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image


DEFAULT_MATERIALS = (
    "gravel=T_FlatgroundGravel1",
    "rock=T_RockGround2",
    "debris=T_ErodedGround",
    "silt=T_SoilMud",
    "sand=T_SmoothSand_02",
)

# 每层位移的峰峰值（米）。没有真实高度的层由 height 探测自动降到 0。
DEFAULT_DISPLACEMENT_M = {
    "gravel": 0.03,
    "rock": 0.20,
    "debris": 0.08,
    "silt": 0.04,
    "sand": 0.015,
}

# 这套 Megascans 来自沙漠峡谷样本，直接使用会让飞马岛变成沙地。
# 只保留 albedo 的亮度细节，按层重新染色（Pegasus 的植被/岩石配色）。
TINTS = {
    "gravel": (0.40, 0.50, 0.30),
    "rock": (0.52, 0.52, 0.50),
    "debris": (0.46, 0.42, 0.33),
    "silt": (0.34, 0.31, 0.26),
    "sand": (0.66, 0.62, 0.48),
}


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument(
        "--repeat-size-m",
        type=float,
        default=252.0,
        help="world size of one texture repeat (tile 1008 m / uvScale 4)",
    )
    parser.add_argument(
        "--material",
        action="append",
        default=None,
        help="key=T_AssetName，可重复；默认内置一组")
    parser.add_argument(
        "--displacement",
        action="append",
        default=None,
        help="key=meters（峰峰值），可重复；覆盖内置默认")
    parser.add_argument("--no-ao", action="store_true", help="跳过 AO 烘焙")
    parser.add_argument("--ao-radius-m", type=float, default=8.0)
    parser.add_argument("--ao-fine-radius-m", type=float, default=1.0)
    parser.add_argument("--ao-directions", type=int, default=16)
    parser.add_argument("--ao-steps", type=int, default=12)
    parser.add_argument("--ao-max-slope-deg", type=float, default=60.0)
    parser.add_argument(
        "--ao-height-scale",
        type=float,
        default=1.0,
        help="height multiplier used only while baking AO; texture relief is "
        "often exaggerated relative to the world displacement scale",
    )
    return parser.parse_args()


def _tint(albedo: Image.Image, key: str) -> Image.Image:
    tint = TINTS.get(key)
    if tint is None:
        return albedo
    rgb = np.asarray(albedo.convert("RGB"), dtype=np.float64) / 255.0
    luma = rgb @ np.array([0.2126, 0.7152, 0.0722])
    mean = float(luma.mean())
    if mean > 1.0e-6:
        luma = luma * (0.45 / mean)
    luma = np.clip(luma, 0.15, 1.0)[..., None]
    colored = luma * np.array(tint)[None, None, :] * 1.9
    return Image.fromarray(
        (np.clip(colored, 0.0, 1.0) * 255.0).astype(np.uint8), mode="RGB")


def _load(path: Path, size: int, resample: Image.Resampling) -> Image.Image:
    with Image.open(path) as image:
        converted = image.convert("RGBA")
    if converted.size != (size, size):
        converted = converted.resize((size, size), resample)
    return converted


def _load_scalar(path: Path, size: int) -> np.ndarray:
    """Load a single-channel map at full precision, box-filtered."""
    with Image.open(path) as image:
        is_16_bit = image.mode.startswith("I;16") or image.mode == "I"
        if image.mode not in ("I;16", "I;16B", "I", "L"):
            image = image.convert("L")
        values = np.asarray(image, dtype=np.float32)
        if values.ndim == 3:
            values = values[..., 0]
        if is_16_bit:
            values = values / 65535.0
        elif values.size and values.max() <= 1.0:
            pass
        else:
            values = values / 255.0
        side = size
        if values.shape != (side, side):
            box = Image.fromarray(
                (np.clip(values, 0.0, 1.0) * 65535.0).astype(np.uint16))
            box = box.resize((side, side), Image.Resampling.BOX)
            values = np.asarray(box, dtype=np.float32) / 65535.0
    return values


def _normalize_mean(values: np.ndarray) -> np.ndarray:
    """Full-contrast normalize, then shift so the mean lands on 0.5."""
    low = float(values.min())
    high = float(values.max())
    normalized = (values - low) / max(high - low, 1.0e-6)
    return np.clip(0.5 + (normalized - float(normalized.mean())), 0.0, 1.0)


def heightfield_ao(
    height: np.ndarray,
    displacement_m: float,
    texel_m: float,
    radius_m: float,
    directions: int,
    steps: int,
    max_slope_deg: float,
) -> np.ndarray:
    """Horizon-based AO over a tileable height field (wrap-around sampling)."""
    if displacement_m <= 0.0:
        return np.ones_like(height, dtype=np.float32)
    world = ((height.astype(np.float32) - 0.5) * displacement_m)
    tan_max = math.tan(math.radians(max_slope_deg))
    max_step = max(1, int(round(radius_m / max(texel_m, 1.0e-6))))
    occlusion = np.zeros_like(world)
    for direction in range(directions):
        angle = 2.0 * math.pi * direction / directions
        offset_x = math.cos(angle)
        offset_z = math.sin(angle)
        horizon = np.full_like(world, -1.0e9)
        for step in range(1, steps + 1):
            distance = max_step * (step / steps)
            shift_x = int(round(offset_x * distance))
            shift_z = int(round(offset_z * distance))
            if shift_x == 0 and shift_z == 0:
                continue
            shifted = np.roll(world, (-shift_z, -shift_x), axis=(0, 1))
            world_distance = math.hypot(shift_x, shift_z) * texel_m
            slope = (shifted - world) / max(world_distance, 1.0e-6)
            np.maximum(horizon, slope, out=horizon)
        occlusion += np.clip(horizon / tan_max, 0.0, 1.0)
    return np.clip(1.0 - occlusion / directions, 0.0, 1.0)


def _bake_ao(
    height: np.ndarray,
    displacement_m: float,
    height_scale: float,
    repeat_size_m: float,
    args: argparse.Namespace,
) -> np.ndarray:
    texel_m = repeat_size_m / float(height.shape[0])
    ao_displacement_m = displacement_m * height_scale
    coarse = heightfield_ao(
        height, ao_displacement_m, texel_m, args.ao_radius_m,
        args.ao_directions, args.ao_steps, args.ao_max_slope_deg)
    fine = heightfield_ao(
        height, ao_displacement_m, texel_m, args.ao_fine_radius_m,
        args.ao_directions, args.ao_steps, args.ao_max_slope_deg)
    return np.sqrt(np.clip(coarse * fine, 0.0, 1.0))


def _find_height(source: Path, asset: str) -> Path | None:
    for suffix in ("_H.png", "_D.png", "_Displacement.png"):
        candidate = source / f"{asset}{suffix}"
        if candidate.is_file():
            return candidate
    return None


def main() -> int:
    args = _parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    pairs = args.material or list(DEFAULT_MATERIALS)

    displacement = dict(DEFAULT_DISPLACEMENT_M)
    for entry in args.displacement or ():
        key, value = entry.split("=", 1)
        displacement[key] = float(value)

    summary = {}
    for entry in pairs:
        key, asset = entry.split("=", 1)
        albedo = _load(source / f"{asset}_A.png", args.size,
                       Image.Resampling.LANCZOS)
        normal = _load(source / f"{asset}_N.png", args.size,
                       Image.Resampling.BOX)
        roughness_path = source / f"{asset}_R.png"
        if roughness_path.is_file():
            roughness = _load_scalar(roughness_path, args.size)
        else:
            roughness = np.full((args.size, args.size), 200.0 / 255.0,
                                dtype=np.float32)

        height_path = _find_height(source, asset)
        has_height = height_path is not None
        if has_height:
            height = _normalize_mean(_load_scalar(height_path, args.size))
        else:
            height = np.full((args.size, args.size), 0.5, dtype=np.float32)

        displacement_m = displacement.get(key, 0.0) if has_height else 0.0
        if args.no_ao or displacement_m <= 0.0:
            ao = np.ones_like(height, dtype=np.float32)
        else:
            ao = _bake_ao(
                height, displacement_m, args.ao_height_scale,
                args.repeat_size_m, args)

        orm = np.zeros((args.size, args.size, 4), dtype=np.uint8)
        orm[:, :, 0] = np.clip(ao * 255.0, 0.0, 255.0).astype(np.uint8)
        orm[:, :, 1] = np.clip(roughness * 255.0, 0.0, 255.0).astype(np.uint8)
        orm[:, :, 2] = 0
        orm[:, :, 3] = 255

        _tint(albedo, key).convert("RGBA").save(output / f"{key}_albedo.png")
        normal.save(output / f"{key}_normal.png")
        Image.fromarray(orm, mode="RGBA").save(output / f"{key}_orm.png")
        Image.fromarray(
            (height * 65535.0).astype(np.uint16)).save(
                output / f"{key}_height16.png")

        summary[key] = {
            "asset": asset,
            "hasHeight": has_height,
            "heightSource": height_path.name if has_height else None,
            "displacementM": displacement_m,
            "roughnessMean": round(float(roughness.mean()), 4),
            "aoMean": round(float(ao.mean()), 4),
            "aoMin": round(float(ao.min()), 4),
        }
        print(f"{key}: {asset} height={has_height} "
              f"disp={displacement_m} ao={float(ao.mean()):.3f}")

    (output / "layers.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    print("output", output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

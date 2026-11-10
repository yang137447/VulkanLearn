"""把 UE 导出的 Megascans 贴图打包成 VulkanLearn 的层纹理。

输入（export_megascans.py 的产物）：

    T_<Name>_A.png  albedo
    T_<Name>_N.png  normal
    T_<Name>_R.png  roughness
    T_<Name>_H.png  height（可选）

输出（每个层四张，尺寸统一到 ``--size``）：

    <key>_albedo.png    RGB
    <key>_normal.png    RGB（保持 UE 的 green 方向，材质里用 invertNormalGreen）
    <key>_pbr.png       RGBA：R=roughness, G=metallic(0), B=AO(255), A=255
    <key>_height16.png  16-bit 灰度
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument(
        "--material",
        action="append",
        default=None,
        help="key=T_AssetName，可重复；默认内置一组")
    return parser.parse_args()


DEFAULT_MATERIALS = (
    "gravel=T_NaturalGravel",
    "rock=T_RockGround2",
    "debris=T_ErodedGround",
    "silt=T_SoilMud",
    "sand=T_SmoothSand_02",
)

# 这套 Megascans 来自沙漠峡谷样本，直接使用会让飞马岛变成沙地。
# 只保留 albedo 的亮度细节，按层重新染色（Pegasus 的植被/岩石配色）。
TINTS = {
    "gravel": (0.40, 0.50, 0.30),
    "rock": (0.52, 0.52, 0.50),
    "debris": (0.46, 0.42, 0.33),
    "silt": (0.34, 0.31, 0.26),
    "sand": (0.66, 0.62, 0.48),
}


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


def _load(path: Path, size: int) -> Image.Image:
    with Image.open(path) as image:
        converted = image.convert("RGBA")
    if converted.size != (size, size):
        converted = converted.resize((size, size), Image.Resampling.LANCZOS)
    return converted


def main() -> int:
    args = _parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    pairs = args.material or list(DEFAULT_MATERIALS)

    for entry in pairs:
        key, asset = entry.split("=", 1)
        albedo = _load(source / f"{asset}_A.png", args.size)
        normal = _load(source / f"{asset}_N.png", args.size)
        roughness_path = source / f"{asset}_R.png"
        if roughness_path.is_file():
            roughness = np.asarray(_load(roughness_path, args.size),
                                   dtype=np.float64)[:, :, 0]
        else:
            roughness = np.full((args.size, args.size), 200.0)

        pbr = np.zeros((args.size, args.size, 4), dtype=np.uint8)
        pbr[:, :, 0] = np.clip(roughness, 0, 255).astype(np.uint8)
        pbr[:, :, 1] = 0
        pbr[:, :, 2] = 255
        pbr[:, :, 3] = 255

        height_path = source / f"{asset}_H.png"
        if height_path.is_file():
            height = np.asarray(_load(height_path, args.size),
                                dtype=np.float64)[:, :, 0]
        else:
            height = np.asarray(albedo.convert("L"), dtype=np.float64)

        _tint(albedo, key).save(output / f"{key}_albedo.png")
        normal.convert("RGB").save(output / f"{key}_normal.png")
        Image.fromarray(pbr, mode="RGBA").save(output / f"{key}_pbr.png")
        height16 = (np.clip(height / 255.0, 0.0, 1.0)
                    * 65535.0).astype(np.uint16)
        Image.fromarray(height16).save(output / f"{key}_height16.png")
        print(f"{key}: {asset} roughness="
              f"{float(np.mean(roughness)):.0f}")

    print("output", output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

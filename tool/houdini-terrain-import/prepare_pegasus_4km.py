"""把 Pegasus 4 km Houdini 导出整理成可切片的 heightfield 资产。

输入是 ``Pegasus/decoded/_ref4km`` 这类导出目录：

```text
manifest.json                 # sizeM / samples / stats(before, after)
global/height_after_height.png    # 最终高度（米，按 stats 反归一化）
global/height_after_water.png     # 水深（米，0..~1.5）
global/height_after_debris.png    # 松散碎屑厚度（米，0..~1.05）
global/height_after_sediment.png  # 沉积厚度（米，0..~0.92）
global/height_after_bedrock.png   # 基岩面高度（米）
```

``prepare_pegasus_import.py`` 只接受已经归一化、且逐 texel 求和约 255 的
g_layer 掩膜，所以这里先把米制的水/碎屑/沉积层 remap 成权重，并补一层
坡度导出的岩石权重与一层兜底地表，做归一化后写出 ``Layer_*.png``，
再调用它生成 manifest 供 ``split_pegasus_tiles.py`` 切 4x4。

这不是“再用 Blender 复刻一遍”：高度和水系完全来自 Houdini 导出，本脚本
只做单位换算、权重归一化和层绑定。
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] /
                       "blender-terrain-import"))

from png16_io import read_gray16, write_gray16  # noqa: E402
import terrain_metrics  # noqa: E402


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export-root", type=Path, required=True,
                        help="目录，含 manifest.json 与 global/height_after_*.png")
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--output-relative",
                        default="Generated/HoudiniPegasus4km")
    parser.add_argument(
        "--water-full-m", type=float, default=0.35,
        help="水深 remap 到 1.0 的参考值（导出 p99 约 0.36 m）")
    parser.add_argument(
        "--debris-full-m", type=float, default=0.25,
        help="碎屑厚度 remap 到 1.0 的参考值（p99 约 0.22 m）")
    parser.add_argument(
        "--sediment-full-m", type=float, default=0.15,
        help="沉积厚度 remap 到 1.0 的参考值（p99 约 0.13 m）")
    parser.add_argument(
        "--rock-slope-low", type=float, default=None,
        help="绝对角度下限；省略时用坡度百分位")
    parser.add_argument(
        "--rock-slope-high", type=float, default=None,
        help="绝对角度上限；省略时用坡度百分位")
    parser.add_argument("--rock-percentile-low", type=float, default=90.0)
    parser.add_argument("--rock-percentile-high", type=float, default=99.0)
    parser.add_argument("--material-uv-scale", type=float, default=16.0)
    parser.add_argument(
        "--beach-floor-m",
        type=float,
        default=14.0,
        help="land floor of this landscape (its cliff top); the beach ring "
        "starts here",
    )
    parser.add_argument(
        "--beach-width-m",
        type=float,
        default=30.0,
        help="width of the coastal sand ring measured in plan view",
    )
    parser.add_argument(
        "--terrain-texture-root", type=Path, default=None,
        help="Megascans 打包目录；默认 <res>/Generated/Megascans/Textures/Source")
    return parser.parse_args()


def _load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def _layer(export: Path, name: str, low: float, high: float) -> np.ndarray:
    raw = read_gray16(export / f"height_after_{name}.png").astype(
        np.float64) / 65535.0
    return low + raw * (high - low)


def _smoothstep(value: np.ndarray, low: float, high: float) -> np.ndarray:
    t = np.clip((value - low) / max(high - low, 1e-9), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _write_gray(path: Path, values: np.ndarray) -> None:
    write_gray16(path, (np.clip(values, 0.0, 1.0) * 65535.0).astype(np.uint16))


def main() -> int:
    args = _parse_args()
    export = args.export_root.resolve()
    resource_root = args.resource_root.resolve()
    manifest_path = export / "manifest.json"
    if not manifest_path.is_file():
        raise SystemExit(f"missing Houdini export manifest: {manifest_path}")

    summary = _load_json(manifest_path)
    stats = summary["stats"]["after"]
    height_low = float(stats["height"]["min"])
    height_high = float(stats["height"]["max"])
    samples = int(summary["samples"])
    size_m = float(summary["sizeM"])
    cell = size_m / max(samples - 1, 1)

    source_dir = export / "global"
    height = _layer(source_dir, "height", height_low, height_high)
    water = _layer(source_dir, "water", 0.0, float(stats["water"]["max"]))
    debris = _layer(source_dir, "debris", 0.0, float(stats["debris"]["max"]))
    sediment = _layer(source_dir, "sediment", 0.0,
                      float(stats["sediment"]["max"]))

    gy, gx = np.gradient(terrain_metrics._smooth3(height, 1), cell, cell,
                         edge_order=1)
    slope = np.degrees(np.arctan(np.hypot(gx, gy)))

    if args.rock_slope_low is None or args.rock_slope_high is None:
        # 4 km 参考是高差 147 m 的缓地形，绝对 25..45° 永远不会触发；
        # 用坡度百分位取“最陡的一小部分”才符合 Houdini 的岩石露头分布。
        slope_low = float(np.percentile(slope, args.rock_percentile_low))
        slope_high = float(np.percentile(slope, args.rock_percentile_high))
    else:
        slope_low = float(args.rock_slope_low)
        slope_high = float(args.rock_slope_high)
    rock = _smoothstep(slope, slope_low, max(slope_high, slope_low + 0.5))
    water_weight = np.clip(water / max(args.water_full_m, 1e-6), 0.0, 1.0)
    debris_weight = np.clip(debris / max(args.debris_full_m, 1e-6), 0.0, 1.0)
    sediment_weight = np.clip(
        sediment / max(args.sediment_full_m, 1e-6), 0.0, 1.0)

    # 岛外的平地是 y≈0 的海面；单独一层，否则会按地形纹理渲染成陆地。
    sea_weight = 1.0 - _smoothstep(height, 0.0, 3.0)
    # 海滩：这块 landscape 没有 0.3–10 m 的缓坡带——岛面在 14 m 处直接被
    # 切断成 60°+ 的崖壁。因此沙层不是按绝对高度取带，而是沿“陆地边缘”
    # 向内取固定宽度（默认 30 m）的环带，再滤掉陡坡，得到一条等宽海岸线。
    # 崖顶高度随资产变化，`--beach-floor-m` 需要按新地形重新标定。
    land = height > args.beach_floor_m
    beach_cells = max(1, int(round(args.beach_width_m / max(cell, 1.0e-6))))
    inner = land.copy()
    for _ in range(beach_cells):
        inner = (inner & np.roll(inner, 1, axis=0) &
                 np.roll(inner, -1, axis=0) &
                 np.roll(inner, 1, axis=1) &
                 np.roll(inner, -1, axis=1))
    beach_weight = (
        (land & ~inner).astype(np.float64) *
        (1.0 - _smoothstep(slope, 20.0, 40.0)) *
        (1.0 - sea_weight))
    stack = np.stack([rock, water_weight, debris_weight, sediment_weight,
                      sea_weight, beach_weight])
    covered = np.clip(stack.sum(axis=0), 0.0, 1.0)
    base = 1.0 - covered
    weights = np.stack([base] + [row for row in stack])
    total = weights.sum(axis=0)
    weights = weights / np.maximum(total, 1e-9)[None, :, :]

    output_root = (resource_root / args.output_relative).resolve()
    source_out = output_root / "Source"
    source_out.mkdir(parents=True, exist_ok=True)

    height_path = source_out / "heightmap.png"
    normalized = (height - height_low) / max(height_high - height_low, 1e-9)
    _write_gray(height_path, normalized)

    names = ("base", "rock", "water", "debris", "sediment", "sea", "sand")
    mask_paths = {}
    for name, values in zip(names, weights):
        path = source_out / f"Layer_g_{name}.png"
        _write_gray(path, values)
        mask_paths[name] = path

    water_root = resource_root / "Generated/PegasusWater/Textures/Source"
    layer_specs = [
        ("Gravel Ground", "base", "gravel", [0.0, 1.0], [0.0, 90.0], 4.0),
        ("Rock Face", "rock", "rock", [0.0, 1.0], [0.0, 90.0], 3.5),
        ("River Water", "water", None, [0.0, 1.0], [0.0, 90.0], 4.0),
        ("Scree Debris", "debris", "debris", [0.0, 1.0], [0.0, 90.0], 5.0),
        ("River Silt", "sediment", "silt", [0.0, 1.0], [0.0, 90.0], 6.0),
        ("Sea", "sea", None, [0.0, 1.0], [0.0, 90.0], 2.0),
        ("Beach Sand", "sand", "sand", [0.0, 1.0], [0.0, 90.0], 2.0),
    ]
    megascans_root = (
        args.terrain_texture_root.resolve()
        if args.terrain_texture_root is not None
        else resource_root / "Generated/TerrainLayers/Textures/Source")
    if not megascans_root.is_dir():
        # 旧资产仍在 Megascans 目录；新管线输出到 TerrainLayers。
        legacy = resource_root / "Generated/Megascans/Textures/Source"
        if legacy.is_dir():
            megascans_root = legacy
    layer_meta_path = megascans_root / "layers.json"
    layer_meta = (_load_json(layer_meta_path)
                  if layer_meta_path.is_file() else {})

    def _pbr_name(root: Path, prefix: str) -> str:
        # ORM 是当前规范；旧资产仍可回落到 _pbr.png。
        orm = f"{prefix}_orm.png"
        return orm if (root / orm).is_file() else f"{prefix}_pbr.png"

    layers = []
    for name, key, prefix, height_range, slope_range, uv_scale in layer_specs:
        if key in ("water", "sea"):
            texture_root = water_root
            albedo, normal, pbr, height_map = (
                "water_albedo.png", "water_normal.png",
                _pbr_name(texture_root, "water"), "water_height16.png")
        elif (megascans_root / f"{prefix}_albedo.png").is_file():
            texture_root = megascans_root
            albedo = f"{prefix}_albedo.png"
            normal = f"{prefix}_normal.png"
            pbr = _pbr_name(texture_root, prefix)
            height_map = f"{prefix}_height16.png"
        else:
            texture_root = resource_root / "Generated/KiteDemo/Textures/Source"
            albedo = f"{prefix}_albedo.png"
            normal = f"{prefix}_normal.png"
            pbr = _pbr_name(texture_root, prefix)
            height_map = f"{prefix}_height16.png"
        meta = layer_meta.get(prefix, {})
        layers.append({
            "name": name,
            "mask": mask_paths[key].name,
            "textures": {
                "albedo": str((texture_root / albedo).resolve()),
                "normal": str((texture_root / normal).resolve()),
                "pbr": str((texture_root / pbr).resolve()),
                "height": str((texture_root / height_map).resolve()),
            },
            "heightRange": height_range,
            "slopeRange": slope_range,
            "uvScale": uv_scale,
            "uvRotation": 0.0,
            "noise": 0.03,
            "displacementM": float(meta.get("displacementM", 0.0)),
            "hasHeight": bool(meta.get("hasHeight", False)),
        })

    source_descriptor = {
        "assetName": "houdini_pegasus_4km",
        "outputRelative": args.output_relative,
        "description": (
            "Pegasus 4 km Houdini 导出直连：高度、water/debris/sediment 层"
            "全部来自 Houdini heightfield 烘焙，本管线只做单位换算与权重"
            "归一化，不再用 Blender 几何节点复刻。"),
        "sceneDescription": (
            "Pegasus 4×4 Houdini 直连场景：4 km 全局高度与四类地表层切成 "
            "16 个 1 km tile，用于与 Houdini 原始地貌对齐验收。"),
        "source": {
            "project": "Pegasus LandscapeWorkflow 4 km export",
            # prepare_pegasus_import 以 exportRoot 解析 heightmap 与 mask；
            # 这里指到本脚本写出的 Source 目录，原始导出路径单独记录。
            "exportRoot": str(source_out.as_posix()),
            "originalExportRoot": str(export.as_posix()),
            "heightmap": "heightmap.png",
            "bgeoResolution": [samples, samples],
            "units": "m",
        },
        "sizeMeters": [size_m, size_m],
        "heightRangeMeters": [height_low, height_high],
        "textureResolution": 1024,
        "paintResolution": 2048,
        "materialUvScale": args.material_uv_scale,
        "invertNormalGreen": True,
        "flipVertical": False,
        "terrainPosition": [0.0, -height_low * 100.0, 0.0],
        "cameraPosition": [0.0, size_m * 25.0, size_m * 80.0],
        "cameraRotation": [-15.0, 0.0, 0.0],
        "layers": layers,
    }
    descriptor_path = output_root / "houdini_pegasus_4km_source.json"
    descriptor_path.write_text(
        json.dumps(source_descriptor, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")

    prepare = Path(__file__).with_name("prepare_pegasus_import.py")
    manifest_out = output_root / "houdini_pegasus_import.json"
    subprocess.run([
        sys.executable, str(prepare),
        "--source", str(descriptor_path),
        "--resource-root", str(resource_root),
        "--output", str(manifest_out),
    ], check=True)

    print(json.dumps({
        "manifest": str(manifest_out),
        "descriptor": str(descriptor_path),
        "heightmap": str(height_path),
        "masks": {name: str(path) for name, path in mask_paths.items()},
        "sizeMeters": size_m,
        "samples": samples,
        "heightRangeMeters": [round(height_low, 2), round(height_high, 2)],
        "coverage": {
            "rock": round(float((rock > 0.5).mean()), 4),
            "water": round(float((water_weight > 0.5).mean()), 4),
            "debris": round(float((debris_weight > 0.5).mean()), 4),
            "sediment": round(float((sediment_weight > 0.5).mean()), 4),
            "base": round(float((base > 0.5).mean()), 4),
        },
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Blender headless terrain producer for VulkanLearn.

Run inside Blender:

    blender --background --python export_terrain.py -- \
        --heightmap <heightmap.png> --output-dir <dir> --manifest <json>

The script is deliberately a producer, not an engine dependency. It reads a
16-bit heightmap, builds a Blender inspection mesh, derives four deterministic
layer masks, writes PNG16 images, and emits a manifest consumable by
``tool/terrain-ue-import/build_vulkanlearn_assets.py``.

Blender image pixels are bottom-up. VulkanLearn heightmap sampling is top-down
(row 0 is v=0), so the script flips once when reading and writes top-down images.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

import bpy
import numpy as np


PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def _write_gray16_png(path: Path, values: np.ndarray) -> None:
    """Write a true 16-bit grayscale PNG without Pillow/OpenCV."""
    if values.ndim != 2:
        raise ValueError(f"expected a 2D array, got {values.shape}")
    height, width = values.shape
    clamped = np.clip(values, 0, 65535).astype(">u2", copy=False)
    rows = np.zeros((height, 1 + width * 2), dtype=np.uint8)
    rows[:, 0] = 0  # filter type None
    rows[:, 1:] = clamped.view(np.uint8).reshape(height, width * 2)
    header = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)
    payload = (
        PNG_MAGIC
        + _png_chunk(b"IHDR", header)
        + _png_chunk(b"IDAT", zlib.compress(rows.tobytes(), 6))
        + _png_chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def _smoothstep(edge0: float, edge1: float, value: np.ndarray) -> np.ndarray:
    if edge0 == edge1:
        return (value >= edge1).astype(np.float32)
    t = np.clip((value - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _load_heightmap(path: Path, resolution: int) -> np.ndarray:
    image = bpy.data.images.load(str(path), check_existing=False)
    image.colorspace_settings.name = "Non-Color"
    if resolution > 0 and tuple(image.size) != (resolution, resolution):
        image.scale(resolution, resolution)
    width, height = image.size
    pixels = np.asarray(image.pixels[:], dtype=np.float32)
    if pixels.size != width * height * 4:
        raise RuntimeError(f"unexpected Blender image layout for {path}")
    # Blender image rows start at the bottom; VulkanLearn expects row 0 at v=0.
    return pixels.reshape(height, width, 4)[::-1, :, 0]


def _derive_masks(
    height: np.ndarray,
    size_m: float,
    height_range_m: float,
) -> dict[str, np.ndarray]:
    # 物理间距：掩码必须按米计算坡度，不能把 texel 当成长度单位。
    spacing = size_m / float(max(1, height.shape[1] - 1))
    height_m = height * height_range_m
    dy, dx = np.gradient(height_m, spacing, spacing)
    slope = np.sqrt(dx * dx + dy * dy)
    slope_norm = np.clip(slope / 1.5, 0.0, 1.0)
    laplacian = np.abs(
        np.roll(height_m, 1, axis=0) + np.roll(height_m, -1, axis=0) +
        np.roll(height_m, 1, axis=1) + np.roll(height_m, -1, axis=1) -
        4.0 * height_m)
    curvature_norm = np.clip(laplacian / 0.02, 0.0, 1.0)

    lowland = _smoothstep(0.62, 0.10, height) * (1.0 - 0.45 * slope_norm)
    slope_mask = _smoothstep(0.12, 0.72, slope_norm) * (1.0 - 0.25 * height)
    ridge = _smoothstep(0.18, 0.85, curvature_norm) * _smoothstep(0.42, 0.92, height)
    highland = _smoothstep(0.48, 0.95, height) * (1.0 - 0.35 * slope_norm)

    masks = {
        "lowland": lowland,
        "slope": slope_mask,
        "ridge": ridge,
        "highland": highland,
    }
    total = sum(masks.values())
    normalized = {}
    for name, mask in masks.items():
        # 全零区域保持全零，由 TR 的 height/slope 规则兜底；非零区域归一化。
        normalized[name] = np.where(total > 1.0e-6, mask / np.maximum(total, 1.0e-6), 0.0)
    return {
        "lowland": normalized["lowland"],
        "slope": normalized["slope"],
        "ridge": normalized["ridge"],
        "highland": normalized["highland"],
        "slope_raw": slope_norm,
        "curvature_raw": curvature_norm,
    }


def _build_inspection_mesh(name: str, height: np.ndarray, size_m: float, z_range_m: float) -> None:
    # 预览网格只取 512²，避免把 4K 源直接变成数百万 Blender 顶点。
    step = max(1, height.shape[0] // 512)
    sampled = height[::step, ::step]
    rows, cols = sampled.shape
    vertices = []
    faces = []
    for row in range(rows):
        for col in range(cols):
            u = col / float(max(1, cols - 1))
            v = row / float(max(1, rows - 1))
            vertices.append(((u - 0.5) * size_m, (v - 0.5) * size_m,
                             float(sampled[row, col]) * z_range_m))
    for row in range(rows - 1):
        for col in range(cols - 1):
            a = row * cols + col
            faces.append((a, a + 1, a + cols + 1, a + cols))
    mesh = bpy.data.meshes.new(name + ".mesh")
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)


def _parse_args() -> argparse.Namespace:
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else []
    parser = argparse.ArgumentParser(description="Export a Blender terrain source")
    parser.add_argument("--heightmap", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--asset-name", default="blender_pegasus")
    parser.add_argument("--output-relative", default="Generated/BlenderPegasus")
    parser.add_argument("--size-m", type=float, required=True)
    parser.add_argument("--height-min-m", type=float, required=True)
    parser.add_argument("--height-max-m", type=float, required=True)
    parser.add_argument("--resolution", type=int, default=1024)
    parser.add_argument("--texture-resolution", type=int, default=1024)
    parser.add_argument("--paint-resolution", type=int, default=2048)
    parser.add_argument("--material-uv-scale", type=float, default=8.0)
    parser.add_argument("--layer-texture-root", type=Path, required=True)
    parser.add_argument("--grid-resolution", type=int, default=1024)
    parser.add_argument("--chunk-quads", type=int, default=256)
    parser.add_argument("--lod-levels", type=int, default=3)
    parser.add_argument("--camera-z-cm", type=float, default=90000.0)
    parser.add_argument("--camera-y-cm", type=float, default=35000.0)
    return parser.parse_args(argv)


def main() -> int:
    args = _parse_args()
    if args.resolution < 2 or args.resolution > 4096:
        raise ValueError("resolution must be in [2, 4096]")
    if args.height_max_m <= args.height_min_m:
        raise ValueError("height-max-m must be greater than height-min-m")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)

    height = _load_heightmap(args.heightmap.resolve(), args.resolution)
    masks = _derive_masks(height, args.size_m, args.height_max_m - args.height_min_m)
    height16 = np.clip(height, 0.0, 1.0) * 65535.0
    height_path = args.output_dir / "heightmap_blender.png"
    _write_gray16_png(height_path, height16)

    mask_paths = {}
    for name in ("lowland", "slope", "ridge", "highland"):
        mask_path = args.output_dir / f"mask_{name}.png"
        _write_gray16_png(mask_path, masks[name] * 65535.0)
        mask_paths[name] = mask_path

    layer_specs = [
        ("Gravel Ground", "lowland", 0, [0.0, 0.45], [0.0, 30.0], 16.0),
        ("Slope Rock", "slope", 7, [0.0, 1.0], [18.0, 90.0], 14.0),
        ("Ridge Rock", "ridge", 2, [0.35, 1.0], [20.0, 90.0], 12.0),
        ("Highland Volcanic", "highland", 2, [0.45, 1.0], [0.0, 90.0], 12.0),
    ]
    layers = []
    for name, mask_name, texture_index, height_range, slope_range, uv_scale in layer_specs:
        texture_base = args.layer_texture_root.resolve()
        layers.append({
            "name": name,
            "weightmapPath": str(mask_paths[mask_name].resolve()),
            "textures": {
                "albedo": str((texture_base / f"layer_{texture_index:02d}_albedo.png").resolve()),
                "normal": str((texture_base / f"layer_{texture_index:02d}_normal.png").resolve()),
                "pbr": str((texture_base / f"layer_{texture_index:02d}_pbr.png").resolve()),
                "height": str((texture_base / f"layer_{texture_index:02d}_height16.png").resolve()),
            },
            "heightRange": height_range,
            "slopeRange": slope_range,
            "uvScale": uv_scale,
            "uvRotation": 0.0,
            "noise": 0.03,
        })

    height_scale_cm = (args.height_max_m - args.height_min_m) * 100.0
    height_offset_cm = args.height_min_m * 100.0
    manifest = {
        "producer": "blender",
        "sourceHeightmap": str(args.heightmap.resolve()),
        "assetName": args.asset_name,
        "outputRelative": args.output_relative,
        "heightmapPath": str(height_path.resolve()),
        "size": [args.size_m * 100.0, args.size_m * 100.0],
        "heightScale": height_scale_cm,
        "heightOffset": height_offset_cm,
        "textureResolution": args.texture_resolution,
        "paintResolution": args.paint_resolution,
        "materialUvScale": args.material_uv_scale,
        "invertNormalGreen": True,
        "flipVertical": False,
        "description": (
            "Blender 5.2 headless 生成的 Pegasus 风格地形源：高度图来自外部 16-bit PNG，"
            "坡度/曲率/低地/高地掩码由 Blender 脚本确定性推导；材质纹理使用 KiteDemo PBR 占位。"
        ),
        "sceneDescription": (
            "Blender terrain producer 验收场景：真实 16-bit 高度场、四层 Blender mask "
            "烘焙为 VLP1 权重、KiteDemo 地表纹理数组；用于管线验证。"
        ),
        "terrainPosition": [0.0, -height_offset_cm, 0.0],
        "cameraPosition": [0.0, args.camera_y_cm, args.camera_z_cm],
        "cameraRotation": [-20.0, 0.0, 0.0],
        "gridResolution": args.grid_resolution,
        "chunkQuads": args.chunk_quads,
        "lodLevels": args.lod_levels,
        "lodDistances": [250000.0, 500000.0],
        "chunkResidencyDistances": [275000.0, 550000.0],
        "layers": layers,
    }
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    bpy.ops.wm.read_factory_settings(use_empty=True)
    _build_inspection_mesh(
        args.asset_name, height, args.size_m,
        args.height_max_m - args.height_min_m)
    bpy.ops.wm.save_as_mainfile(filepath=str((args.output_dir / "blender_terrain_source.blend").resolve()))

    metadata = {
        "producer": "blender",
        "blenderVersion": bpy.app.version_string,
        "sourceHeightmap": str(args.heightmap.resolve()),
        "resolution": args.resolution,
        "sizeMeters": args.size_m,
        "heightRangeMeters": [args.height_min_m, args.height_max_m],
        "maskFormula": {
            "lowland": "smoothstep(0.62,0.10,h) * (1 - 0.45 * slopeNorm)",
            "slope": "smoothstep(0.12,0.72,slopeNorm) * (1 - 0.25 * h)",
            "ridge": "smoothstep(0.18,0.85,curvatureNorm) * smoothstep(0.42,0.92,h)",
            "highland": "smoothstep(0.48,0.95,h) * (1 - 0.35 * slopeNorm)",
        },
        "manifest": str(args.manifest.resolve()),
    }
    (args.output_dir / "blender_terrain_metadata.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

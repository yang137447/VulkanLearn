"""Split a global Pegasus GE source into a seamless 4x4 terrain tile set.

This script intentionally stays outside Blender's dependency graph: it reads
the PNG16 height/mask outputs and writes per-tile PNG16 inputs plus import
manifests.  Tiles overlap by a configurable heightmap margin; the runtime maps
mesh UV to the inner samples, so edge normals read the neighbouring tile's
height while adjacent meshes still share exactly the same boundary height.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import numpy as np

from png16_io import read_gray16, write_gray16


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Split a global Pegasus terrain into tiles")
    parser.add_argument("--source-manifest", type=Path, required=True)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--tile-grid", type=int, default=4)
    parser.add_argument("--tile-resolution", type=int, default=256)
    parser.add_argument("--heightmap-margin", type=int, default=1)
    parser.add_argument("--tile-size-m", type=float, default=1000.0)
    parser.add_argument("--lod-levels", type=int, default=3)
    parser.add_argument(
        "--lod-distances-m", type=float, nargs="+", default=None,
        metavar="M")
    parser.add_argument("--page-tiles-per-side", type=int, default=8)
    parser.add_argument("--asset-prefix", default="blender_gn_pegasus_tile")
    parser.add_argument("--texture-resolution", type=int, default=1024)
    parser.add_argument("--paint-resolution", type=int, default=512)
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON root must be an object: {path}")
    return value


def _resample(values: np.ndarray, target: int) -> np.ndarray:
    source = np.asarray(values, dtype=np.float64)
    if source.ndim != 2:
        raise ValueError("resample input must be 2D")
    if target < 2:
        raise ValueError("target must be at least 2")
    if source.shape == (target, target):
        return source
    rows, columns = source.shape
    x = np.linspace(0.0, columns - 1.0, target)
    y = np.linspace(0.0, rows - 1.0, target)
    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    x1 = np.minimum(x0 + 1, columns - 1)
    y1 = np.minimum(y0 + 1, rows - 1)
    fx = x - x0
    fy = y - y0
    top = (
        source[y0[:, None], x0[None, :]] * (1.0 - fx)[None, :]
        + source[y0[:, None], x1[None, :]] * fx[None, :]
    )
    bottom = (
        source[y1[:, None], x0[None, :]] * (1.0 - fx)[None, :]
        + source[y1[:, None], x1[None, :]] * fx[None, :]
    )
    return top * (1.0 - fy)[:, None] + bottom * fy[:, None]


def _resample_with_margin(
    values: np.ndarray,
    target: int,
    margin: int,
) -> np.ndarray:
    if margin > 0:
        values = np.pad(values, margin, mode="edge")
    return _resample(values, target)


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _relative_posix(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError as error:
        raise RuntimeError(f"output path is outside resource root: {path}") from error


def main() -> int:
    args = _parse_args()
    if args.tile_grid < 2:
        raise RuntimeError("tile-grid must be at least 2")
    if args.tile_resolution < 2:
        raise RuntimeError("tile-resolution must be at least 2")
    if not 0 <= args.heightmap_margin <= 16:
        raise RuntimeError("heightmap-margin must be in [0, 16]")
    if args.heightmap_margin >= args.tile_resolution:
        raise RuntimeError(
            "heightmap-margin must be smaller than tile-resolution")
    if args.tile_size_m <= 0.0:
        raise RuntimeError("tile-size-m must be positive")
    if not 1 <= args.lod_levels <= 4:
        raise RuntimeError("lod-levels must be in [1, 4]")
    coarsest_step = 1 << (args.lod_levels - 1)
    if args.tile_resolution % coarsest_step != 0:
        raise RuntimeError(
            "tile-resolution must be divisible by the coarsest LOD step")
    if args.lod_levels <= 1:
        lod_distances_m: list[float] = []
    elif args.lod_distances_m is None:
        lod_distances_m = [
            1250.0 * float(index + 1)
            for index in range(args.lod_levels - 1)
        ]
    else:
        lod_distances_m = [float(value) for value in args.lod_distances_m]
    if len(lod_distances_m) != args.lod_levels - 1:
        raise RuntimeError("lod-distances-m requires lodLevels-1 values")
    if any(value <= 0.0 for value in lod_distances_m):
        raise RuntimeError("lod-distances-m values must be positive")
    if any(
            lod_distances_m[index] >= lod_distances_m[index + 1]
            for index in range(len(lod_distances_m) - 1)):
        raise RuntimeError("lod-distances-m values must be strictly ascending")
    if not 1 <= args.page_tiles_per_side <= 32:
        raise RuntimeError("page-tiles-per-side must be in [1, 32]")
    if args.tile_grid * args.page_tiles_per_side > 32:
        raise RuntimeError(
            "tile-grid * page-tiles-per-side exceeds the 32x32 page table")

    source_path = args.source_manifest.resolve()
    source = _load_json(source_path)
    source_root = source_path.parent
    resource_root = args.resource_root.resolve()
    output_root = args.output_root.resolve()

    height_path = Path(source["heightmapPath"])
    if not height_path.is_absolute():
        height_path = source_root / height_path
    height = read_gray16(height_path).astype(np.float64) / 65535.0

    layers = source.get("layers")
    if not isinstance(layers, list) or not layers:
        raise RuntimeError("source manifest has no layers")
    layer_masks: list[np.ndarray] = []
    for index, layer in enumerate(layers):
        mask_value = layer.get("weightmapPath") if isinstance(layer, dict) else None
        if not isinstance(mask_value, str) or not mask_value:
            raise RuntimeError(f"layer {index} has no weightmapPath")
        mask_path = Path(mask_value)
        if not mask_path.is_absolute():
            mask_path = source_root / mask_path
        layer_masks.append(
            read_gray16(mask_path).astype(np.float64) / 65535.0)

    tile_samples = args.tile_resolution + args.heightmap_margin * 2 + 1
    sample_grid = (
        args.tile_grid * args.tile_resolution +
        args.heightmap_margin * 2 + 1)
    height_global = _resample_with_margin(
        height, sample_grid, args.heightmap_margin)
    masks_global = [
        _resample_with_margin(
            mask, sample_grid, args.heightmap_margin)
        for mask in layer_masks
    ]

    size_cm = [float(value) for value in source["size"]]
    tile_size_cm = args.tile_size_m * 100.0
    expected_size_cm = args.tile_grid * tile_size_cm
    if abs(max(size_cm) - expected_size_cm) > 1.0:
        raise RuntimeError(
            f"global size {size_cm} does not match tile grid {args.tile_grid} "
            f"x {args.tile_size_m} m")

    output_root.mkdir(parents=True, exist_ok=True)
    material_uv_scale = float(source.get("materialUvScale", 8.0))
    base_uv_scale_divisor = float(args.tile_grid)
    tiles: list[dict[str, Any]] = []

    for grid_z in range(args.tile_grid):
        for grid_x in range(args.tile_grid):
            tile_index = grid_z * args.tile_grid + grid_x
            tile_name = f"tile_{tile_index:02d}"
            tile_root = output_root / tile_name
            source_dir = tile_root / "Source"
            source_dir.mkdir(parents=True, exist_ok=True)

            start_x = grid_x * args.tile_resolution
            start_z = grid_z * args.tile_resolution
            end_x = start_x + tile_samples
            end_z = start_z + tile_samples
            tile_height = height_global[start_z:end_z, start_x:end_x]
            tile_height_path = source_dir / "heightmap_gn_tile.png"
            write_gray16(
                tile_height_path,
                np.clip(tile_height, 0.0, 1.0) * 65535.0)

            tile_layers: list[dict[str, Any]] = []
            for layer_index, (layer, mask_global) in enumerate(
                    zip(layers, masks_global)):
                tile_mask = mask_global[start_z:end_z, start_x:end_x]
                mask_name = f"mask_gn_{layer.get('name', layer_index)}_{layer_index:02d}.png"
                mask_path = source_dir / mask_name
                write_gray16(mask_path, np.clip(tile_mask, 0.0, 1.0) * 65535.0)
                tile_layer = dict(layer)
                tile_layer["weightmapPath"] = str(mask_path.resolve())
                tile_layer["uvScale"] = (
                    float(layer.get("uvScale", 1.0)) / base_uv_scale_divisor)
                tile_layers.append(tile_layer)

            position = [
                (grid_x - (args.tile_grid - 1) * 0.5) * tile_size_cm,
                0.0,
                (grid_z - (args.tile_grid - 1) * 0.5) * tile_size_cm,
            ]
            asset_name = f"{args.asset_prefix}_{tile_index:02d}"
            output_relative = _relative_posix(tile_root, resource_root)
            manifest: dict[str, Any] = {
                "producer": "blender-geometry-nodes-tiled",
                "assetName": asset_name,
                "outputRelative": output_relative,
                "heightmapPath": str(tile_height_path.resolve()),
                "size": [tile_size_cm, tile_size_cm],
                "heightScale": source["heightScale"],
                "heightOffset": source.get("heightOffset", 0.0),
                "textureResolution": args.texture_resolution,
                "paintResolution": args.paint_resolution,
                "materialUvScale": material_uv_scale / base_uv_scale_divisor,
                # 多 tile 场景必须走层 page table / atlas 的虚拟纹理路径：
                # mode 0 的 legacy 数组直采只用 globalId，16 块 tile 全部挤在
                # 同一组数组里，terrainFeedback 不会产生有效反馈（实测
                # visibleLayers=0 / resident=4/512）。这里随 manifest 透传。
                "terrainLayerMode": source.get("terrainLayerMode", 1.0),
                "invertNormalGreen": source.get("invertNormalGreen", True),
                "flipVertical": source.get("flipVertical", False),
                "description": source.get("description", ""),
                "sceneDescription": source.get("sceneDescription", ""),
                "environment": source.get("environment"),
                "terrainPosition": position,
                "cameraPosition": source.get("cameraPosition"),
                "cameraRotation": source.get("cameraRotation"),
                "gridResolution": args.tile_resolution,
                "chunkQuads": max(1, args.tile_resolution // 4),
                "lodLevels": args.lod_levels,
                "lodDistances": [
                    float(value) * 100.0 for value in lod_distances_m],
                "heightmapMargin": args.heightmap_margin,
                "pageOrigin": [
                    grid_x * args.page_tiles_per_side,
                    grid_z * args.page_tiles_per_side,
                ],
                "pageTilesPerSide": args.page_tiles_per_side,
                "layers": tile_layers,
            }
            manifest_path = tile_root / "blender_gn_import.json"
            _write_json(manifest_path, manifest)
            tiles.append({
                "index": tile_index,
                "gridX": grid_x,
                "gridZ": grid_z,
                "name": tile_name,
                "manifest": str(manifest_path.resolve()),
                "assetName": asset_name,
                "position": position,
                "pageOrigin": manifest["pageOrigin"],
                "pageTilesPerSide": args.page_tiles_per_side,
            })

    layout = {
        "version": 1,
        "producer": "pegasus-tiled-split",
        "sourceManifest": str(source_path),
        "tileGrid": args.tile_grid,
        "tileResolution": args.tile_resolution,
        "tileSamples": tile_samples,
        "heightmapMargin": args.heightmap_margin,
        "lodLevels": args.lod_levels,
        "lodDistancesCm": [
            float(value) * 100.0 for value in lod_distances_m],
        "tileSizeCm": tile_size_cm,
        "pageTilesPerSide": args.page_tiles_per_side,
        "cameraPosition": source.get("cameraPosition"),
        "cameraRotation": source.get("cameraRotation"),
        "environment": source.get("environment"),
        "tiles": tiles,
    }
    layout_path = output_root / "tile_layout.json"
    _write_json(layout_path, layout)
    print(json.dumps({
        "layout": str(layout_path),
        "tiles": len(tiles),
        "sampleGrid": sample_grid,
        "tileSamples": tile_samples,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

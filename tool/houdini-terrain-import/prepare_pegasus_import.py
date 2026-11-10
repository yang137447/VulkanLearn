"""把 Houdini heightfield 导出整理成 build_vulkanlearn_assets.py 的 manifest。

这个脚本只做离线数据校验与清单生成，不调用 Houdini/HAPI，也不依赖许可证。
真实高度/权重来自 Houdini 导出的 16-bit PNG；世界尺寸与高度范围来自同目录
bgeo.sc 的 gstat 元数据，写在 houdini_pegasus_source.json 里，脚本会核对
PNG 分辨率和 bgeoResolution 一致后才生成清单。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None


DEFAULT_RESOURCE_ROOT = Path(
    r"D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources"
)
DEFAULT_SOURCE = Path(__file__).with_name("houdini_pegasus_source.json")


class PreparationError(RuntimeError):
    pass


def _fail(message: str) -> None:
    raise PreparationError(message)


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        _fail(f"cannot read {path}: {error}")
    if not isinstance(value, dict):
        _fail(f"JSON root must be an object: {path}")
    return value


def _require_png_metadata(path: Path) -> tuple[int, int, int, int]:
    if Image is None:
        _fail("Pillow is required to validate Houdini PNG exports")
    if not path.is_file():
        _fail(f"missing PNG: {path}")
    with path.open("rb") as stream:
        header = stream.read(26)
    if len(header) < 26 or header[:8] != b"\x89PNG\r\n\x1a\n":
        _fail(f"not a PNG: {path}")
    if header[12:16] != b"IHDR":
        _fail(f"PNG is missing IHDR: {path}")
    width = int.from_bytes(header[16:20], "big")
    height = int.from_bytes(header[20:24], "big")
    bit_depth = header[24]
    color_type = header[25]
    return width, height, bit_depth, color_type


def _validate_heightmap(path: Path, expected: int) -> tuple[int, int, int, int]:
    width, height, bit_depth, color_type = _require_png_metadata(path)
    if (width, height) != (expected, expected):
        _fail(
            f"heightmap size {width}x{height} does not match bgeo resolution "
            f"{expected}x{expected}")
    if bit_depth != 16 or color_type != 0:
        _fail(
            f"heightmap must be 16-bit grayscale PNG; got bitDepth={bit_depth}, "
            f"colorType={color_type}")
    return width, height, bit_depth, color_type


def _validate_mask(path: Path, expected: int) -> None:
    width, height, bit_depth, color_type = _require_png_metadata(path)
    if (width, height) != (expected, expected):
        _fail(
            f"mask {path.name} size {width}x{height} does not match "
            f"{expected}x{expected}")
    if bit_depth != 16 or color_type != 0:
        _fail(
            f"mask {path.name} must be 16-bit grayscale PNG; got "
            f"bitDepth={bit_depth}, colorType={color_type}")


def _sample_mask_sum(paths: list[Path], expected: int) -> float:
    total = 0.0
    sample_side = min(256, expected)
    for path in paths:
        with Image.open(path) as opened:
            image = opened.point(lambda value: value / 257.0).convert("L")
            image = image.resize((sample_side, sample_side), Image.Resampling.BILINEAR)
            getter = getattr(image, "get_flattened_data", None)
            total += sum(getter() if getter is not None else image.getdata())
    return total / float(sample_side * sample_side)


def _require_number(value: Any, context: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        _fail(f"{context} must be a number")
    return float(value)


def _require_pair(value: Any, context: str) -> tuple[float, float]:
    if not isinstance(value, list) or len(value) != 2:
        _fail(f"{context} must be a two-number array")
    return _require_number(value[0], f"{context}[0]"), _require_number(
        value[1], f"{context}[1]")


def _require_vec3(value: Any, context: str) -> tuple[float, float, float]:
    if not isinstance(value, list) or len(value) != 3:
        _fail(f"{context} must be a three-number array")
    return tuple(
        _require_number(item, f"{context}[{index}]")
        for index, item in enumerate(value)
    )


def _absolute(base: Path, value: Any, context: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        _fail(f"{context} must be a non-empty path string")
    path = Path(value)
    if not path.is_absolute():
        path = base / path
    return path.resolve()


def _build_manifest(source: dict[str, Any], resource_root: Path) -> dict[str, Any]:
    source_info = source.get("source")
    if not isinstance(source_info, dict):
        _fail("source must be an object")
    export_root = _absolute(
        Path.cwd(), source_info.get("exportRoot"), "source.exportRoot")
    heightmap_name = source_info.get("heightmap")
    if not isinstance(heightmap_name, str) or not heightmap_name:
        _fail("source.heightmap must be a non-empty string")
    heightmap_path = (export_root / heightmap_name).resolve()

    bgeo_resolution = source_info.get("bgeoResolution")
    if not isinstance(bgeo_resolution, list) or len(bgeo_resolution) != 2:
        _fail("source.bgeoResolution must be [width, height]")
    expected = int(_require_number(bgeo_resolution[0], "source.bgeoResolution[0]"))
    if expected != int(_require_number(
            bgeo_resolution[1], "source.bgeoResolution[1]")):
        _fail("source.bgeoResolution must be square")
    width, height, _, _ = _validate_heightmap(heightmap_path, expected)

    size_x_m, size_z_m = _require_pair(source.get("sizeMeters"), "sizeMeters")
    height_min_m, height_max_m = _require_pair(
        source.get("heightRangeMeters"), "heightRangeMeters")
    if size_x_m <= 0.0 or size_z_m <= 0.0:
        _fail("sizeMeters must be positive")
    if height_max_m <= height_min_m:
        _fail("heightRangeMeters must be ascending")

    raw_layers = source.get("layers")
    if not isinstance(raw_layers, list) or not raw_layers:
        _fail("layers must be a non-empty array")
    masks: list[Path] = []
    layers: list[dict[str, Any]] = []
    for index, raw_layer in enumerate(raw_layers):
        if not isinstance(raw_layer, dict):
            _fail(f"layers[{index}] must be an object")
        name = raw_layer.get("name")
        mask_name = raw_layer.get("mask")
        if not isinstance(name, str) or not name.strip():
            _fail(f"layers[{index}].name must be a non-empty string")
        if not isinstance(mask_name, str) or not mask_name.strip():
            _fail(f"layers[{index}].mask must be a non-empty string")
        mask_path = (export_root / mask_name).resolve()
        _validate_mask(mask_path, expected)
        masks.append(mask_path)

        textures = raw_layer.get("textures")
        if not isinstance(textures, dict):
            _fail(f"layers[{index}].textures must be an object")
        resolved_textures: dict[str, str] = {}
        for role in ("albedo", "normal", "pbr", "height"):
            asset_path = _absolute(
                resource_root, textures.get(role), f"layers[{index}].textures.{role}")
            if not asset_path.is_file():
                _fail(f"missing layer texture: {asset_path}")
            resolved_textures[role] = str(asset_path)

        height_range = raw_layer.get("heightRange", [0.0, 1.0])
        slope_range = raw_layer.get("slopeRange", [0.0, 90.0])
        layers.append({
            "name": name.strip(),
            "weightmapPath": str(mask_path),
            "textures": resolved_textures,
            "heightRange": list(_require_pair(
                height_range, f"layers[{index}].heightRange")),
            "slopeRange": list(_require_pair(
                slope_range, f"layers[{index}].slopeRange")),
            "uvScale": _require_number(
                raw_layer.get("uvScale", 1.0), f"layers[{index}].uvScale"),
            "uvRotation": _require_number(
                raw_layer.get("uvRotation", 0.0), f"layers[{index}].uvRotation"),
            "noise": _require_number(
                raw_layer.get("noise", 0.0), f"layers[{index}].noise"),
            # 置换规范字段：峰峰值（米）与是否存在真实 16-bit 高度。
            "displacementM": _require_number(
                raw_layer.get("displacementM", 0.0),
                f"layers[{index}].displacementM"),
            "hasHeight": bool(raw_layer.get("hasHeight", False)),
        })

    mask_sum = _sample_mask_sum(masks, expected)
    if not 250.0 <= mask_sum <= 260.0:
        _fail(
            f"g_layer masks do not form a normalized set; sampled mean sum is "
            f"{mask_sum:.2f} (expected ~255)")

    asset_name = source.get("assetName")
    output_relative = source.get("outputRelative")
    if not isinstance(asset_name, str) or not asset_name:
        _fail("assetName must be a non-empty string")
    if not isinstance(output_relative, str) or not output_relative:
        _fail("outputRelative must be a non-empty string")

    height_scale_cm = (height_max_m - height_min_m) * 100.0
    height_offset_cm = height_min_m * 100.0
    terrain_position = _require_vec3(
        source.get("terrainPosition", [0.0, -height_offset_cm, 0.0]),
        "terrainPosition")
    camera_position = _require_vec3(
        source.get("cameraPosition", [0.0, 35000.0, 90000.0]),
        "cameraPosition")
    camera_rotation = _require_vec3(
        source.get("cameraRotation", [-20.0, 0.0, 0.0]),
        "cameraRotation")

    manifest: dict[str, Any] = {
        "assetName": asset_name,
        "outputRelative": output_relative,
        "heightmapPath": str(heightmap_path),
        "size": [size_x_m * 100.0, size_z_m * 100.0],
        "heightScale": height_scale_cm,
        "heightOffset": height_offset_cm,
        "textureResolution": int(source.get("textureResolution", 1024)),
        "paintResolution": int(source.get("paintResolution", 2048)),
        "materialUvScale": _require_number(
            source.get("materialUvScale", 8.0), "materialUvScale"),
        "invertNormalGreen": bool(source.get("invertNormalGreen", True)),
        "flipVertical": bool(source.get("flipVertical", False)),
        "description": source.get("description", ""),
        "sceneDescription": source.get("sceneDescription", ""),
        "terrainPosition": list(terrain_position),
        "cameraPosition": list(camera_position),
        "cameraRotation": list(camera_rotation),
        "layers": layers,
    }
    for key in ("gridResolution", "chunkQuads", "lodLevels",
                "lodDistances", "chunkResidencyDistances"):
        if key in source:
            manifest[key] = source[key]
    return {
        "manifest": manifest,
        "heightmapSize": (width, height),
        "maskSum": mask_sum,
        "sourceInfo": source_info,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Prepare a Houdini heightfield export for VulkanLearn assets.")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument(
        "--resource-root", type=Path, default=DEFAULT_RESOURCE_ROOT)
    parser.add_argument(
        "--output", type=Path,
        help="manifest output path; defaults to <resource-root>/<outputRelative>/"
             "houdini_pegasus_import.json")
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args(argv)

    try:
        source = _load_json(args.source.resolve())
        resource_root = args.resource_root.resolve()
        result = _build_manifest(source, resource_root)
        manifest = result["manifest"]
        output = args.output
        if output is None:
            output = resource_root / manifest["outputRelative"] / (
                "houdini_pegasus_import.json")
        output = output.resolve()
        if not args.check_only:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(
                json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8")
        summary = {
            "status": "ok",
            "manifest": str(output),
            "assetName": manifest["assetName"],
            "heightmap": f"{result['heightmapSize'][0]}x{result['heightmapSize'][1]}",
            "sizeCm": manifest["size"],
            "heightScaleCm": manifest["heightScale"],
            "heightOffsetCm": manifest["heightOffset"],
            "layers": len(manifest["layers"]),
            "sampledMaskSum": round(result["maskSum"], 2),
        }
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        return 0
    except (PreparationError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

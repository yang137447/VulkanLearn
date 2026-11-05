"""把 UE KiteDemo 导出数据转换为 VulkanLearn 地形资产。

用法：
    py -3 tool/terrain-ue-import/build_vulkanlearn_assets.py <manifest.json>
    py -3 tool/terrain-ue-import/build_vulkanlearn_assets.py --self-test

默认写入同级资源工作树：
    D:\\YYBWorkSpace\\GitHub\\VulkanLearn-worktrees\\terrain-openworld\\
        VukanLearnResources\\Generated\\KiteDemo

manifest 的核心字段：
    heightmapPath: 16-bit PNG（优先）、8-bit PNG 或 scanline EXR
    size: [X, Z]，单位厘米
    heightScale / heightOffset: 厘米
    weightmaps: { layer name: 8-bit/16-bit PNG path }，顺序不参与层顺序
    layers: 层记录列表。每个记录提供 name，以及以下任一种纹理写法：
        textures: { albedo, normal, pbr, height }
        albedoPath / normalPath / pbrPath / heightPath
        files: [ ... ]，按 UE 常见后缀（_D_R、_N、_H、_PBR、_ORM 等）分类

可选字段：
    textureResolution: 512 / 1024 / 2048，默认 1024
    paintResolution: VLP1 边长，默认取权重图最大边（2..4096）
    flipVertical: 是否为所有输入图像追加一次上下翻转，默认 false
    invertNormalGreen: UE DirectX 法线 G 通道是否翻转为 OpenGL 约定，默认 true
    materialUvScale: MI 的 u_uvScale，默认 8.0
    assetName: 输出 TR/SC/MI 与文件名的资产名，默认 kitedemo
    outputRelative: 资源根相对输出目录，默认 Generated/KiteDemo
    terrainPosition / cameraPosition / cameraRotation: 场景摆放覆盖（cm/度）
    description / sceneDescription: 可选的中文 TR/SC 描述覆盖
    gridResolution / chunkQuads / lodLevels: 可选网格覆盖
    lodDistances / chunkResidencyDistances: 可选 LOD 距离数组覆盖
    heightmapMargin: 可选的网格外圈 overscan 采样数（默认 0）
    pageOrigin / pageTilesPerSide: 可选全局 32x32 VT 页表区域

16-bit weightmap 会先按 257 归一化再写入 VLP1，避免 PIL 直接截断低 8 位。
输出的 TR 使用 16-bit PNG 高度图；纹理数组统一到 textureResolution，
albedo 数组附带同序 displacementSources（16-bit 灰度 PNG）。
权重图写为 VLP1：权重按 layer-major、opacity 全 255，层数与 TR 完全一致。
所有文件先写同目录临时文件再原子替换；路径必须落在输出根内。
脚本不访问网络，不修改运行时 C++ 或契约文档。
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import re
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

try:
    from PIL import Image
except ImportError:  # pragma: no cover - exercised only on machines without Pillow
    Image = None


DEFAULT_RESOURCE_ROOT = Path(
    r"D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources"
)
OUTPUT_RELATIVE = Path("Generated") / "KiteDemo"
SUPPORTED_TEXTURE_RESOLUTIONS = (512, 1024, 2048)
VLP1_HEADER_BYTES = 24
VLP1_MAX_BYTES = 512 * 1024 * 1024
EXR_MAGIC = b"\x76\x2f\x31\x01"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

ROLE_ALIASES = {
    "albedo": (
        "albedo", "albedopath", "diffuse", "diffusepath", "basecolor",
        "basecolorpath", "color", "colorpath",
    ),
    "normal": ("normal", "normalpath", "normalmap", "normalmappath"),
    "pbr": (
        "pbr", "pbrpath", "orm", "ormpath", "rma", "rmapath",
        "arm", "armpath", "mra", "mrapath", "material", "materialpath",
    ),
    "height": (
        "height", "heightpath", "displacement", "displacementpath",
        "heightmap", "heightmappath",
    ),
}


class AssetImportError(RuntimeError):
    """用户可读的导入错误。"""


@dataclass(frozen=True)
class LayerInput:
    name: str
    weightmap: Path | None
    albedo: Path
    normal: Path
    pbr: Path
    height: Path
    height_range: tuple[float, float]
    slope_range: tuple[float, float]
    noise: float
    uv_scale: float
    uv_rotation: float


@dataclass(frozen=True)
class ManifestSpec:
    manifest_path: Path
    asset_name: str
    output_relative: Path
    heightmap_path: Path
    size_x_cm: float
    size_z_cm: float
    height_scale_cm: float
    height_offset_cm: float
    texture_resolution: int
    paint_resolution: int | None
    flip_vertical: bool
    invert_normal_green: bool
    material_uv_scale: float
    description: str
    scene_description: str
    environment: dict[str, Any] | None
    terrain_position_cm: tuple[float, float, float]
    camera_position_cm: tuple[float, float, float] | None
    camera_rotation_deg: tuple[float, float, float]
    grid_resolution: int | None
    chunk_quads: int | None
    lod_levels: int | None
    lod_distances_cm: tuple[float, ...] | None
    chunk_residency_distances_cm: tuple[float, ...] | None
    heightmap_margin: int
    page_origin: tuple[int, int] | None
    page_tiles_per_side: int | None
    layers: tuple[LayerInput, ...]


@dataclass
class HeightField:
    width: int
    height: int
    values: list[int]
    bit_depth: int
    source_kind: str

    def as_u16(self) -> list[int]:
        if self.bit_depth <= 8:
            return [min(65535, max(0, int(value) * 257)) for value in self.values]
        return [min(65535, max(0, int(value))) for value in self.values]


@dataclass(frozen=True)
class GridChoice:
    grid_resolution: int
    chunk_quads: int
    lod_levels: int
    lod_distances_cm: tuple[float, ...]
    chunk_residency_distances_cm: tuple[float, ...]


def _fail(message: str) -> None:
    raise AssetImportError(message)


def _require_pillow() -> None:
    if Image is None:
        _fail("Pillow is required. Install Pillow or run with a Python that provides it.")


def _lower_keys(value: dict[str, Any]) -> dict[str, Any]:
    return {str(key).lower(): item for key, item in value.items()}


def _first(mapping: dict[str, Any], *names: str, default: Any = None) -> Any:
    for name in names:
        key = name.lower()
        if key in mapping:
            return mapping[key]
    return default


def _require_number(value: Any, context: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        _fail(f"{context} must be a number")
    result = float(value)
    if not math.isfinite(result):
        _fail(f"{context} must be finite")
    return result


def _optional_bounded_int(
    mapping: dict[str, Any],
    names: tuple[str, ...],
    context: str,
    minimum: int,
    maximum: int,
) -> int | None:
    value = _first(mapping, *names)
    if value is None:
        return None
    if not isinstance(value, int) or isinstance(value, bool):
        _fail(f"{context} must be an integer")
    if value < minimum or value > maximum:
        _fail(f"{context} must be in [{minimum}, {maximum}]")
    return value


def _optional_number_list(
    mapping: dict[str, Any],
    names: tuple[str, ...],
    context: str,
) -> tuple[float, ...] | None:
    value = _first(mapping, *names)
    if value is None:
        return None
    if not isinstance(value, list):
        _fail(f"{context} must be a number array")
    result: list[float] = []
    previous = 0.0
    for index, item in enumerate(value):
        number = _require_number(item, f"{context}[{index}]")
        if number <= 0.0:
            _fail(f"{context} values must be positive")
        if number <= previous:
            _fail(f"{context} values must be strictly ascending")
        previous = number
        result.append(number)
    return tuple(result)


def _optional_vec3(
    mapping: dict[str, Any],
    names: tuple[str, ...],
    context: str,
) -> tuple[float, float, float] | None:
    value = _first(mapping, *names)
    if value is None:
        return None
    if not isinstance(value, list) or len(value) != 3:
        _fail(f"{context} must be a three-number array")
    return tuple(
        _require_number(item, f"{context}[{index}]")
        for index, item in enumerate(value)
    )


def _asset_name(value: Any) -> str:
    if not isinstance(value, str) or not value.strip():
        _fail("assetName must be a non-empty string")
    name = value.strip()
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-"
    if any(character not in allowed for character in name):
        _fail("assetName may only contain letters, digits, '_', '.' and '-'")
    return name


def _output_relative(value: Any) -> Path:
    if not isinstance(value, str) or not value.strip():
        _fail("outputRelative must be a non-empty string")
    relative = Path(value.strip())
    if relative.is_absolute() or ".." in relative.parts:
        _fail("outputRelative must be a relative path without '..'")
    return relative


def _resolve_input(base: Path, value: Any, context: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        _fail(f"{context} must be a non-empty path string")
    path = Path(value)
    if not path.is_absolute():
        path = base / path
    path = path.resolve()
    if not path.is_file():
        _fail(f"{context} does not exist or is not a regular file: {path}")
    return path


def _clean_float(value: float) -> float:
    rounded = round(float(value), 6)
    return 0.0 if rounded == 0.0 else rounded


def _asset_path(resource_root: Path, target: Path) -> str:
    try:
        relative = target.resolve().relative_to(resource_root.resolve())
    except ValueError:
        _fail(f"output path escapes resource root: {target}")
    return relative.as_posix()


def _guard_output(output_root: Path, target: Path) -> Path:
    try:
        target.resolve(strict=False).relative_to(output_root.resolve())
    except ValueError:
        _fail(f"refusing to write outside output root: {target}")
    return target


def _write_bytes_atomic(path: Path, payload: bytes) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file():
        try:
            if path.read_bytes() == payload:
                return False
        except OSError:
            pass
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        return True
    finally:
        if temporary.exists():
            temporary.unlink()


def _save_png_atomic(path: Path, image: "Image.Image") -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".png", dir=path.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        image.save(temporary, format="PNG", compress_level=9)
        payload = temporary.read_bytes()
        if path.is_file():
            try:
                if path.read_bytes() == payload:
                    return False
            except OSError:
                pass
        os.replace(temporary, path)
        return True
    finally:
        if temporary.exists():
            temporary.unlink()


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=4) + "\n").encode("utf-8")


def _write_json_atomic(path: Path, value: Any) -> bool:
    return _write_bytes_atomic(path, _json_bytes(value))


def _copy_bytes_for_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _png_layout(path: Path) -> tuple[int, int]:
    with path.open("rb") as stream:
        header = stream.read(26)
    if len(header) < 26 or header[:8] != PNG_MAGIC:
        _fail(f"not a valid PNG header: {path}")
    if header[12:16] != b"IHDR":
        _fail(f"PNG is missing IHDR: {path}")
    bit_depth = header[24]
    color_type = header[25]
    if bit_depth not in (8, 16):
        _fail(f"PNG bit depth {bit_depth} is unsupported: {path}")
    return bit_depth, color_type


def _flattened_pixels(image: "Image.Image") -> list[int]:
    getter = getattr(image, "get_flattened_data", None)
    source = getter() if getter is not None else image.getdata()
    values: list[int] = []
    for value in source:
        if isinstance(value, tuple):
            values.extend(int(component) for component in value)
        else:
            values.append(int(value))
    return values


def _flip_height_field(field: HeightField) -> HeightField:
    values: list[int] = []
    for row in range(field.height - 1, -1, -1):
        start = row * field.width
        values.extend(field.values[start:start + field.width])
    return HeightField(
        field.width, field.height, values, field.bit_depth, field.source_kind)


def _load_png_height_field(path: Path) -> HeightField:
    bit_depth, color_type = _png_layout(path)
    if bit_depth == 16 and color_type not in (0,):
        _fail(f"16-bit height PNG must be grayscale (color type 0): {path}")
    try:
        with Image.open(path) as opened:
            opened.load()
            image = opened.copy()
    except Exception as error:
        _fail(f"cannot decode height PNG {path}: {error}")

    if bit_depth == 16:
        if image.mode not in ("I", "I;16", "I;16B", "I;16L"):
            _fail(f"16-bit PNG decoded to unsupported mode {image.mode}: {path}")
        values = _flattened_pixels(image)
        for value in values:
            if value < 0 or value > 65535:
                _fail(f"16-bit height sample outside [0, 65535] in {path}")
        return HeightField(image.width, image.height, values, 16, "PNG16")

    if color_type == 3 or image.mode == "P":
        image = image.convert("L")
    elif image.mode in ("L", "LA"):
        image = image.getchannel(0)
    elif image.mode in ("RGB", "RGBA"):
        image = image.getchannel("R")
    elif image.mode == "1":
        image = image.convert("L")
    else:
        image = image.convert("L")
    values = _flattened_pixels(image)
    for value in values:
        if value < 0 or value > 255:
            _fail(f"8-bit height sample outside [0, 255] in {path}")
    return HeightField(image.width, image.height, values, 8, "PNG8")


def _read_c_string(data: bytes, position: int, limit: int, context: str) -> tuple[str, int]:
    if position >= limit:
        _fail(f"truncated EXR string while reading {context}")
    end = data.find(b"\x00", position, limit)
    if end < 0:
        _fail(f"unterminated EXR string while reading {context}")
    try:
        text = data[position:end].decode("utf-8")
    except UnicodeDecodeError:
        _fail(f"invalid UTF-8 in EXR {context}")
    return text, end + 1


def _exr_undo_zip_predictor(payload: bytes) -> bytes:
    size = len(payload)
    if size == 0:
        return b""
    temporary = bytearray(payload)
    for index in range(1, size):
        temporary[index] = (
            int(temporary[index - 1]) + int(temporary[index]) - 128) & 0xFF
    half = (size + 1) // 2
    restored = bytearray(size)
    for index in range(half):
        restored[index * 2] = temporary[index]
        if index * 2 + 1 < size:
            restored[index * 2 + 1] = temporary[half + index]
    return bytes(restored)


def _read_exr_height_field(path: Path) -> HeightField:
    data = path.read_bytes()
    if len(data) < 8 or data[:4] != EXR_MAGIC:
        _fail(f"not an OpenEXR file: {path}")
    version = struct.unpack_from("<I", data, 4)[0]
    if version & 0x200:
        _fail(f"tiled EXR heightmaps are unsupported: {path}")

    position = 8
    attributes: dict[str, tuple[str, bytes]] = {}
    while True:
        name, position = _read_c_string(data, position, len(data), "attribute name")
        if not name:
            break
        type_name, position = _read_c_string(data, position, len(data), "attribute type")
        if position + 4 > len(data):
            _fail(f"truncated EXR attribute size: {path}")
        size = struct.unpack_from("<i", data, position)[0]
        position += 4
        if size < 0 or position + size > len(data):
            _fail(f"invalid EXR attribute size for {name}: {path}")
        attributes[name] = (type_name, data[position:position + size])
        position += size

    for required in ("channels", "compression", "dataWindow", "lineOrder"):
        if required not in attributes:
            _fail(f"EXR is missing {required}: {path}")

    channel_blob = attributes["channels"][1]
    channels: list[tuple[str, int, int, int]] = []
    channel_position = 0
    while True:
        channel_name, channel_position = _read_c_string(
            channel_blob, channel_position, len(channel_blob), "channel name")
        if not channel_name:
            break
        if channel_position + 16 > len(channel_blob):
            _fail(f"truncated EXR channel record: {path}")
        pixel_type = struct.unpack_from("<i", channel_blob, channel_position)[0]
        x_sampling = struct.unpack_from("<i", channel_blob, channel_position + 8)[0]
        y_sampling = struct.unpack_from("<i", channel_blob, channel_position + 12)[0]
        channel_position += 16
        channels.append((channel_name, pixel_type, x_sampling, y_sampling))
    if not channels:
        _fail(f"EXR has no channels: {path}")

    compression = attributes["compression"][1][0]
    if compression in (0, 1, 2):
        lines_per_chunk = 1
    elif compression == 3:
        lines_per_chunk = 16
    else:
        _fail(
            f"EXR compression {compression} is unsupported; use NONE, RLE, ZIPS or ZIP: {path}")
    if compression == 1:
        _fail(f"EXR RLE compression is unsupported; use NONE, ZIPS or ZIP: {path}")

    window = attributes["dataWindow"][1]
    if len(window) != 16:
        _fail(f"invalid EXR dataWindow: {path}")
    x_min, y_min, x_max, y_max = struct.unpack("<4i", window)
    width = x_max - x_min + 1
    height = y_max - y_min + 1
    if width <= 0 or height <= 0:
        _fail(f"invalid EXR dataWindow dimensions: {path}")
    line_order = attributes["lineOrder"][1][0]
    if line_order != 0:
        _fail(f"EXR decreasing/random line order is unsupported: {path}")

    chosen = None
    for channel in channels:
        if channel[0] in ("Y", "R"):
            chosen = channel
            break
    if chosen is None:
        chosen = channels[0]
    _, pixel_type, x_sampling, y_sampling = chosen
    if x_sampling != 1 or y_sampling != 1:
        _fail(f"subsampled EXR channels are unsupported: {path}")
    if pixel_type == 0:
        pixel_bytes = 4
        unpack_format = "<I"
        value_scale = 1.0 / 4294967295.0
    elif pixel_type == 1:
        pixel_bytes = 2
        unpack_format = "<e"
        value_scale = 1.0
    elif pixel_type == 2:
        pixel_bytes = 4
        unpack_format = "<f"
        value_scale = 1.0
    else:
        _fail(f"EXR channel {chosen[0]} has unsupported pixel type {pixel_type}: {path}")

    channel_offsets: dict[str, tuple[int, int, int]] = {}
    line_bytes = 0
    for channel_name, channel_type, xs, ys in channels:
        if xs != 1 or ys != 1:
            _fail(f"subsampled EXR channel {channel_name} is unsupported: {path}")
        bytes_per_pixel = 2 if channel_type == 1 else 4
        channel_offsets[channel_name] = (line_bytes, bytes_per_pixel, channel_type)
        line_bytes += width * bytes_per_pixel
    chosen_offset = channel_offsets[chosen[0]][0]

    chunk_count = (height + lines_per_chunk - 1) // lines_per_chunk
    offsets_end = position + chunk_count * 8
    if offsets_end > len(data):
        _fail(f"truncated EXR chunk offset table: {path}")
    chunk_offsets = struct.unpack_from(f"<{chunk_count}Q", data, position)

    samples = [0] * (width * height)
    for offset in chunk_offsets:
        if offset + 8 > len(data):
            _fail(f"EXR chunk offset is out of range: {path}")
        y, packed_size = struct.unpack_from("<ii", data, offset)
        if y < y_min or y > y_max or packed_size < 0:
            _fail(f"invalid EXR scanline chunk header: {path}")
        packed = data[offset + 8:offset + 8 + packed_size]
        if len(packed) != packed_size:
            _fail(f"truncated EXR scanline chunk payload: {path}")
        if compression == 0:
            raw = packed
        else:
            try:
                raw = zlib.decompress(packed)
            except zlib.error as error:
                _fail(f"cannot inflate EXR ZIP chunk in {path}: {error}")
            raw = _exr_undo_zip_predictor(raw)

        block_lines = min(lines_per_chunk, y_max - y + 1)
        expected = line_bytes * block_lines
        if len(raw) != expected:
            _fail(
                f"EXR chunk size mismatch in {path}: expected {expected}, got {len(raw)}")
        for local_line in range(block_lines):
            row = y - y_min + local_line
            if row < 0 or row >= height:
                continue
            base = local_line * line_bytes + chosen_offset
            for x in range(width):
                value = struct.unpack_from(
                    unpack_format, raw, base + x * pixel_bytes)[0] * value_scale
                if not math.isfinite(value):
                    _fail(f"EXR height contains a non-finite sample: {path}")
                if value < -1.0e-5 or value > 1.00001:
                    _fail(
                        f"EXR height sample {value} is outside [0,1]; "
                        f"normalize the heightmap before import: {path}")
                samples[row * width + x] = int(round(min(1.0, max(0.0, value)) * 65535.0))

    return HeightField(width, height, samples, 16, "EXR float")


def _load_height_field(path: Path) -> HeightField:
    try:
        with path.open("rb") as stream:
            magic = stream.read(4)
    except OSError as error:
        _fail(f"cannot read heightmap {path}: {error}")
    if magic == EXR_MAGIC:
        return _read_exr_height_field(path)
    if magic != PNG_MAGIC[:4]:
        _fail(f"heightmap must be PNG or OpenEXR: {path}")
    return _load_png_height_field(path)


def _resize_height_field(
    field: HeightField, extent: int, flip_vertical: bool) -> list[int]:
    if field.bit_depth <= 8:
        image = Image.new("L", (field.width, field.height))
        image.putdata(field.values)
        if flip_vertical:
            image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        if image.size != (extent, extent):
            image = image.resize((extent, extent), Image.Resampling.BILINEAR)
        return [int(value) * 257 for value in _flattened_pixels(image)]

    image = Image.new("I;16", (field.width, field.height))
    image.putdata(field.values)
    if flip_vertical:
        image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    if image.size != (extent, extent):
        image = image.resize((extent, extent), Image.Resampling.BILINEAR)
    return [min(65535, max(0, int(value))) for value in _flattened_pixels(image)]


def _write_png16(path: Path, width: int, height: int, values: list[int]) -> bool:
    if len(values) != width * height:
        _fail(f"PNG16 sample count mismatch for {path}")
    image = Image.new("I;16", (width, height))
    image.putdata([min(65535, max(0, int(value))) for value in values])
    return _save_png_atomic(path, image)


def _image_to_rgba8(image: "Image.Image") -> "Image.Image":
    if image.mode in ("I", "I;16", "I;16B", "I;16L"):
        values = _flattened_pixels(image)
        data = bytes(min(255, max(0, (int(value) + 128) // 257)) for value in values)
        gray = Image.frombytes("L", image.size, data)
        return gray.convert("RGBA")
    if image.mode == "P":
        image = image.convert("RGBA")
    elif image.mode == "1":
        image = image.convert("L")
    if "A" not in image.getbands():
        image = image.convert("RGB")
    return image.convert("RGBA")


def _load_rgba8(
    path: Path,
    extent: int,
    flip_vertical: bool,
    invert_green: bool = False,
) -> "Image.Image":
    try:
        with Image.open(path) as opened:
            opened.load()
            image = opened.copy()
    except Exception as error:
        _fail(f"cannot decode texture source {path}: {error}")
    image = _image_to_rgba8(image)
    if invert_green:
        red, green, blue, alpha = image.split()
        inverted_green = green.point(tuple(255 - value for value in range(256)))
        image = Image.merge("RGBA", (red, inverted_green, blue, alpha))
    if flip_vertical:
        image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    if image.size != (extent, extent):
        image = image.resize((extent, extent), Image.Resampling.BILINEAR)
    return image


def _srgb_to_linear(value: float) -> float:
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


def _average_albedo_linear(image: "Image.Image") -> list[float]:
    thumb = image.convert("RGBA").resize((16, 16), Image.Resampling.BOX)
    pixels = _flattened_pixels(thumb)
    red = green = blue = weight_sum = 0.0
    for index in range(0, len(pixels), 4):
        alpha = pixels[index + 3] / 255.0
        if alpha <= 0.0:
            continue
        red += _srgb_to_linear(pixels[index] / 255.0) * alpha
        green += _srgb_to_linear(pixels[index + 1] / 255.0) * alpha
        blue += _srgb_to_linear(pixels[index + 2] / 255.0) * alpha
        weight_sum += alpha
    if weight_sum <= 0.0:
        return [0.18, 0.18, 0.18]
    return [
        _clean_float(red / weight_sum),
        _clean_float(green / weight_sum),
        _clean_float(blue / weight_sum),
    ]


def _classify_ue_source(path: Path) -> str | None:
    stem = path.stem.lower()
    normalized = re.sub(r"[^a-z0-9]+", "_", stem)
    tokens = {token for token in normalized.split("_") if token}
    if tokens & {"n", "normal", "norm", "normalmap"} or "normal" in normalized:
        return "normal"
    if tokens & {"h", "height", "disp", "displacement", "hm"}:
        return "height"
    if tokens & {"pbr", "orm", "rma", "arm", "mra", "mask", "material"}:
        return "pbr"
    if re.search(r"(^|_)d_r($|_)", normalized) or (
            "d" in tokens and "r" in tokens):
        return "albedo"
    if tokens & {"d", "diffuse", "albedo", "basecolor", "bc", "color"}:
        return "albedo"
    return None


def _extract_roles(
    record: dict[str, Any],
    texture_record: Any,
    base: Path,
    layer_name: str,
) -> dict[str, Path]:
    roles: dict[str, Path] = {}
    explicit_record = texture_record if isinstance(texture_record, dict) else None
    if explicit_record is not None:
        lowered = _lower_keys(explicit_record)
        for role, aliases in ROLE_ALIASES.items():
            value = _first(lowered, *aliases)
            if value is not None:
                roles[role] = _resolve_input(
                    base, value, f"layer {layer_name} texture {role}")

    for role, aliases in ROLE_ALIASES.items():
        if role in roles:
            continue
        value = _first(record, *aliases)
        if value is not None:
            roles[role] = _resolve_input(
                base, value, f"layer {layer_name} texture {role}")

    for list_key in ("files", "sources", "texturefiles", "texturepaths"):
        raw_files = record.get(list_key)
        if raw_files is None and isinstance(texture_record, list):
            raw_files = texture_record
        if raw_files is None:
            continue
        if not isinstance(raw_files, list):
            _fail(f"layer {layer_name} field {list_key} must be an array")
        for raw_path in raw_files:
            path = _resolve_input(
                base, raw_path, f"layer {layer_name} source file")
            role = _classify_ue_source(path)
            if role is None:
                continue
            if role in roles and roles[role] != path:
                _fail(
                    f"layer {layer_name} has multiple {role} source files: "
                    f"{roles[role]} and {path}")
            roles[role] = path

    missing = [role for role in ("albedo", "normal", "pbr", "height")
               if role not in roles]
    if missing:
        _fail(
            f"layer {layer_name} is missing texture roles: {', '.join(missing)}; "
            "provide textures {albedo, normal, pbr, height}, flat *Path fields, "
            "or a files list with recognizable UE suffixes")
    return roles


def _read_weightmap_records(value: Any, base: Path) -> dict[str, Path]:
    result: dict[str, Path] = {}
    if value is None:
        return result
    if isinstance(value, dict):
        iterator: Iterable[tuple[str, Any]] = value.items()
    elif isinstance(value, list):
        converted = []
        for item in value:
            if not isinstance(item, dict):
                _fail("weightmaps array entries must be objects")
            lowered = _lower_keys(item)
            name = _first(lowered, "name", "layer", "layername")
            path = _first(lowered, "path", "file", "weightmap", "weightmappath")
            converted.append((name, path))
        iterator = converted
    else:
        _fail("weightmaps must be an object keyed by layer name or an array")

    for name, raw_path in iterator:
        if not isinstance(name, str) or not name.strip():
            _fail("weightmap layer name must be a non-empty string")
        if isinstance(raw_path, dict):
            lowered = _lower_keys(raw_path)
            raw_path = _first(lowered, "path", "file", "weightmappath")
        if any(existing.casefold() == name.casefold() for existing in result):
            _fail(f"duplicate weightmap layer name: {name}")
        result[name] = _resolve_input(
            base, raw_path, f"weightmap for layer {name}")
    return result


def _parse_range(value: Any, fallback: tuple[float, float], context: str) -> tuple[float, float]:
    if value is None:
        return fallback
    if not isinstance(value, list) or len(value) != 2:
        _fail(f"{context} must be a two-number array")
    minimum = _require_number(value[0], context + "[0]")
    maximum = _require_number(value[1], context + "[1]")
    if minimum < 0.0 or minimum > maximum:
        _fail(f"{context} must satisfy 0 <= min <= max")
    return minimum, maximum


def _build_layers(
    top: dict[str, Any],
    weightmaps: dict[str, Path],
    base: Path,
) -> tuple[LayerInput, ...]:
    raw_layers = _first(top, "layers")
    layer_textures = _first(top, "layertextures", "texturelayers")
    if isinstance(layer_textures, dict):
        layer_textures = {
            str(name): value for name, value in layer_textures.items()}
    else:
        layer_textures = {}

    if raw_layers is None:
        if not weightmaps and not layer_textures:
            _fail("manifest requires layers or weightmaps/layerTextures")
        names = list(weightmaps.keys()) or list(layer_textures.keys())
        raw_layers = [{"name": name} for name in names]
    if isinstance(raw_layers, dict):
        raw_layers = [
            dict(_lower_keys(record) if isinstance(record, dict) else {},
                 name=name)
            for name, record in raw_layers.items()
        ]
    if not isinstance(raw_layers, list) or not raw_layers:
        _fail("layers must be a non-empty array")

    weightmap_by_fold = {name.casefold(): (name, path) for name, path in weightmaps.items()}
    texture_by_fold = {name.casefold(): value for name, value in layer_textures.items()}
    layers: list[LayerInput] = []
    seen_names: set[str] = set()

    for index, raw_layer in enumerate(raw_layers):
        if isinstance(raw_layer, str):
            record: dict[str, Any] = {"name": raw_layer}
        elif isinstance(raw_layer, dict):
            record = _lower_keys(raw_layer)
        else:
            _fail(f"layers[{index}] must be an object or layer name")

        name = _first(record, "name", "layer", "layername", "paintlayer")
        if not isinstance(name, str) or not name.strip():
            _fail(f"layers[{index}] requires a non-empty name")
        name = name.strip()
        folded = name.casefold()
        if folded in seen_names:
            _fail(f"duplicate terrain layer name: {name}")
        seen_names.add(folded)

        weightmap_value = _first(record, "weightmap", "weightmappath", "weights", "weightspath")
        if weightmap_value is None and folded in weightmap_by_fold:
            weightmap = weightmap_by_fold[folded][1]
        elif weightmap_value is None:
            weightmap = None
        else:
            weightmap = _resolve_input(
                base, weightmap_value, f"layer {name} weightmap")

        texture_record = _first(record, "textures", "texturefiles", "texturepaths")
        if texture_record is None:
            texture_record = texture_by_fold.get(folded)
        roles = _extract_roles(record, texture_record, base, name)

        height_range = _parse_range(
            _first(record, "heightrange"), (0.0, 1.0),
            f"layer {name} heightRange")
        slope_range = _parse_range(
            _first(record, "sloperange"), (0.0, 90.0),
            f"layer {name} slopeRange")
        if height_range[1] > 1.0:
            _fail(f"layer {name} heightRange must stay in 0..1")
        if slope_range[1] > 90.0:
            _fail(f"layer {name} slopeRange must stay in 0..90")
        noise = _require_number(
            _first(record, "noise", default=0.0), f"layer {name} noise")
        if noise < 0.0:
            _fail(f"layer {name} noise must be non-negative")
        uv_scale = _require_number(
            _first(record, "uvscale", default=1.0), f"layer {name} uvScale")
        if uv_scale <= 0.0:
            _fail(f"layer {name} uvScale must be positive")
        uv_rotation = _require_number(
            _first(record, "uvrotation", default=0.0), f"layer {name} uvRotation")

        layers.append(LayerInput(
            name=name,
            weightmap=weightmap,
            albedo=roles["albedo"],
            normal=roles["normal"],
            pbr=roles["pbr"],
            height=roles["height"],
            height_range=(_clean_float(height_range[0]), _clean_float(height_range[1])),
            slope_range=(_clean_float(slope_range[0]), _clean_float(slope_range[1])),
            noise=_clean_float(noise),
            uv_scale=_clean_float(uv_scale),
            uv_rotation=_clean_float(uv_rotation),
        ))

    if weightmaps:
        layer_names = {layer.name.casefold() for layer in layers}
        missing = [name for name in weightmaps if name.casefold() not in layer_names]
        if missing:
            _fail(
                "weightmaps exist for layers without texture records: " +
                ", ".join(missing))
    return tuple(layers)


def _load_manifest(path: Path, texture_resolution_override: int | None,
                   paint_resolution_override: int | None) -> ManifestSpec:
    try:
        raw = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        _fail(f"cannot read manifest {path}: {error}")
    if not isinstance(raw, dict):
        _fail("manifest root must be a JSON object")
    top = _lower_keys(raw)
    base = path.resolve().parent
    heightmap_value = _first(top, "heightmappath", "heightmap", "height")
    heightmap_path = _resolve_input(base, heightmap_value, "heightmapPath")

    size = _first(top, "size", "sizecm")
    if isinstance(size, dict):
        size_lower = _lower_keys(size)
        size_x = _require_number(_first(size_lower, "x"), "size.x")
        size_z = _require_number(_first(size_lower, "z"), "size.z")
    elif isinstance(size, list) and len(size) == 2:
        size_x = _require_number(size[0], "size[0]")
        size_z = _require_number(size[1], "size[1]")
    else:
        _fail("size must be [X, Z] cm or {x, z} cm")
    if size_x <= 0.0 or size_z <= 0.0:
        _fail("size components must be positive")

    height_scale = _require_number(
        _first(top, "heightscale", "height_scale"), "heightScale")
    height_offset = _require_number(
        _first(top, "heightoffset", "height_offset", default=0.0), "heightOffset")

    texture_resolution = texture_resolution_override
    if texture_resolution is None:
        raw_resolution = _first(
            top, "textureresolution", "texture_resolution", default=1024)
        if not isinstance(raw_resolution, int) or isinstance(raw_resolution, bool):
            _fail("textureResolution must be an integer")
        texture_resolution = raw_resolution
    if texture_resolution not in SUPPORTED_TEXTURE_RESOLUTIONS:
        _fail(
            "textureResolution must be one of " +
            ", ".join(str(value) for value in SUPPORTED_TEXTURE_RESOLUTIONS))

    paint_resolution = paint_resolution_override
    if paint_resolution is None:
        raw_paint = _first(top, "paintresolution", "paint_resolution")
        if raw_paint is not None:
            if not isinstance(raw_paint, int) or isinstance(raw_paint, bool):
                _fail("paintResolution must be an integer")
            paint_resolution = raw_paint

    flip_vertical = _first(top, "flipvertical", "flip_vertical", default=False)
    if not isinstance(flip_vertical, bool):
        _fail("flipVertical must be a boolean")
    invert_normal_green = _first(
        top, "invertnormalgreen", "invert_normal_green", default=True)
    if not isinstance(invert_normal_green, bool):
        _fail("invertNormalGreen must be a boolean")
    material_uv_scale = _require_number(
        _first(top, "materialuvscale", "material_uv_scale", default=8.0),
        "materialUvScale")
    if material_uv_scale <= 0.0:
        _fail("materialUvScale must be positive")
    description = _first(top, "description", default="")
    scene_description = _first(top, "scenedescription", "scene_description", default="")
    if not isinstance(description, str) or not isinstance(scene_description, str):
        _fail("description and sceneDescription must be strings")
    environment = _first(top, "environment")
    if environment is not None and not isinstance(environment, dict):
        _fail("environment must be an object")
    asset_name = _asset_name(_first(top, "assetname", "asset_name", default="kitedemo"))
    output_relative = _output_relative(
        _first(top, "outputrelative", "output_relative", default=OUTPUT_RELATIVE.as_posix()))
    terrain_position = _optional_vec3(
        top, ("terrainposition", "terrain_position"), "terrainPosition")
    camera_position = _optional_vec3(
        top, ("cameraposition", "camera_position"), "cameraPosition")
    camera_rotation = _optional_vec3(
        top, ("camerarotation", "camera_rotation"), "cameraRotation")
    grid_resolution = _optional_bounded_int(
        top, ("gridresolution", "grid_resolution"), "gridResolution", 1, 4096)
    chunk_quads = _optional_bounded_int(
        top, ("chunkquads", "chunk_quads"), "chunkQuads", 1, 4096)
    lod_levels = _optional_bounded_int(
        top, ("lodlevels", "lod_levels"), "lodLevels", 1, 4)
    lod_distances = _optional_number_list(
        top, ("loddistances", "lod_distances"), "lodDistances")
    chunk_residency_distances = _optional_number_list(
        top,
        ("chunkresidencydistances", "chunk_residency_distances"),
        "chunkResidencyDistances",
    )
    heightmap_margin = _optional_bounded_int(
        top, ("heightmapmargin", "heightmap_margin"),
        "heightmapMargin", 0, 16) or 0
    raw_page_origin = _first(top, "pageorigin", "page_origin")
    page_origin: tuple[int, int] | None = None
    if raw_page_origin is not None:
        if (not isinstance(raw_page_origin, list) or
                len(raw_page_origin) != 2 or
                not all(isinstance(value, int) and not isinstance(value, bool)
                        for value in raw_page_origin)):
            _fail("pageOrigin must be two integers")
        page_origin = (raw_page_origin[0], raw_page_origin[1])
        if not all(0 <= value < 32 for value in page_origin):
            _fail("pageOrigin values must be in [0, 31]")
    page_tiles_per_side = _optional_bounded_int(
        top, ("pagetilesperside", "page_tiles_per_side"),
        "pageTilesPerSide", 1, 32)
    if page_origin is not None and page_tiles_per_side is not None:
        if (page_origin[0] + page_tiles_per_side > 32 or
                page_origin[1] + page_tiles_per_side > 32):
            _fail("pageOrigin + pageTilesPerSide exceeds the 32x32 page table")

    weightmaps = _read_weightmap_records(_first(top, "weightmaps"), base)
    layers = _build_layers(top, weightmaps, base)
    if any(layer.weightmap is not None for layer in layers):
        if not all(layer.weightmap is not None for layer in layers):
            _fail("every layer must have a weightmap when paint import is requested")

    return ManifestSpec(
        manifest_path=path.resolve(),
        asset_name=asset_name,
        output_relative=output_relative,
        heightmap_path=heightmap_path,
        size_x_cm=size_x,
        size_z_cm=size_z,
        height_scale_cm=height_scale,
        height_offset_cm=height_offset,
        texture_resolution=texture_resolution,
        paint_resolution=paint_resolution,
        flip_vertical=flip_vertical,
        invert_normal_green=invert_normal_green,
        material_uv_scale=_clean_float(material_uv_scale),
        description=description.strip(),
        scene_description=scene_description.strip(),
        environment=environment,
        terrain_position_cm=terrain_position or (0.0, 0.0, 0.0),
        camera_position_cm=camera_position,
        camera_rotation_deg=camera_rotation or (-18.0, 0.0, 0.0),
        grid_resolution=grid_resolution,
        chunk_quads=chunk_quads,
        lod_levels=lod_levels,
        lod_distances_cm=lod_distances,
        chunk_residency_distances_cm=chunk_residency_distances,
        heightmap_margin=heightmap_margin,
        page_origin=page_origin,
        page_tiles_per_side=page_tiles_per_side,
        layers=layers,
    )


def _next_power_of_two_floor(value: int) -> int:
    if value <= 1:
        return 1
    return 1 << (value.bit_length() - 1)


def _inferred_lod_levels(longest_cm: float) -> int:
    if longest_cm < 12000.0:
        return 1
    if longest_cm < 40000.0:
        return 2
    if longest_cm < 100000.0:
        return 3
    return 4


def _choose_grid(
    width: int,
    height: int,
    size_x: float,
    size_z: float,
    grid_override: int | None = None,
    chunk_override: int | None = None,
    lod_override: int | None = None,
    lod_distances_override: tuple[float, ...] | None = None,
    chunk_residency_override: tuple[float, ...] | None = None,
) -> GridChoice:
    max_quads = min(4096, max(1, min(width - 1, height - 1)))
    longest = max(size_x, size_z, 1.0)
    distance_levels: list[int] = []
    if lod_distances_override is not None:
        distance_levels.append(len(lod_distances_override) + 1)
    if chunk_residency_override is not None:
        distance_levels.append(len(chunk_residency_override) + 1)
    if len(set(distance_levels)) > 1:
        _fail(
            "lodDistances and chunkResidencyDistances imply different lodLevels")
    if distance_levels and (distance_levels[0] < 1 or distance_levels[0] > 4):
        _fail("distance arrays imply lodLevels outside [1, 4]")

    if lod_override is not None:
        lod_levels = lod_override
        explicit_lod = True
    elif distance_levels:
        lod_levels = distance_levels[0]
        explicit_lod = True
    else:
        lod_levels = _inferred_lod_levels(longest)
        explicit_lod = False
        while lod_levels > 1 and (1 << (lod_levels - 1)) > max_quads:
            lod_levels -= 1
    if lod_levels < 1 or lod_levels > 4:
        _fail("lodLevels must be in [1, 4]")
    lod_step = 1 << (lod_levels - 1)

    grid_resolution = grid_override
    chunk_quads = chunk_override
    if grid_resolution is None and chunk_quads is None:
        target_chunk = max(1, min(256, max_quads // 16))
        chunk_quads = _next_power_of_two_floor(target_chunk)
        if chunk_quads < lod_step:
            chunk_quads = lod_step
        if chunk_quads > max_quads:
            chunk_quads = (max_quads // lod_step) * lod_step
        if chunk_quads <= 0:
            if explicit_lod:
                _fail(
                    "lodLevels coarsest step exceeds the source heightmap bounds; "
                    "reduce lodLevels or provide gridResolution/chunkQuads")
            chunk_quads = 1
            lod_levels = 1
            lod_step = 1
        grid_resolution = (max_quads // chunk_quads) * chunk_quads
        if grid_resolution <= 0:
            grid_resolution = chunk_quads
    elif grid_resolution is None:
        assert chunk_quads is not None
        grid_resolution = (max_quads // chunk_quads) * chunk_quads
        if grid_resolution <= 0:
            grid_resolution = chunk_quads
    elif chunk_quads is None:
        assert grid_resolution is not None
        target_chunk = max(1, min(256, grid_resolution // 16))
        target_chunk = _next_power_of_two_floor(target_chunk)
        target_chunk = max(target_chunk, lod_step)
        for candidate in range(min(target_chunk, grid_resolution), lod_step - 1, -1):
            if grid_resolution % candidate == 0 and candidate % lod_step == 0:
                chunk_quads = candidate
                break
        if chunk_quads is None:
            _fail(
                "cannot infer chunkQuads that divides gridResolution and the "
                "coarsest LOD step; provide chunkQuads explicitly")

    assert grid_resolution is not None and chunk_quads is not None
    if grid_resolution < 1 or grid_resolution > 4096:
        _fail("gridResolution must be in [1, 4096]")
    if chunk_quads < 1 or chunk_quads > 4096:
        _fail("chunkQuads must be in [1, 4096]")
    if chunk_quads > grid_resolution:
        _fail("chunkQuads must be <= gridResolution")
    if grid_resolution % chunk_quads != 0:
        _fail("gridResolution must be divisible by chunkQuads")
    if chunk_quads % lod_step != 0:
        _fail("chunkQuads must be divisible by the coarsest LOD step")
    if grid_resolution % chunk_quads != 0 or chunk_quads % (1 << (lod_levels - 1)) != 0:
        _fail("internal grid selection produced an invalid chunk/LOD combination")

    if lod_distances_override is None:
        lod_distances = tuple(
            _clean_float(longest * index / lod_levels)
            for index in range(1, lod_levels))
    else:
        if len(lod_distances_override) != lod_levels - 1:
            _fail("lodDistances requires lodLevels-1 values")
        lod_distances = tuple(_clean_float(value) for value in lod_distances_override)
    if chunk_residency_override is None:
        residency = tuple(_clean_float(value * 1.1) for value in lod_distances)
    else:
        if len(chunk_residency_override) != lod_levels - 1:
            _fail("chunkResidencyDistances requires lodLevels-1 values")
        residency = tuple(_clean_float(value) for value in chunk_residency_override)
    for name, distances in (
            ("lodDistances", lod_distances),
            ("chunkResidencyDistances", residency)):
        previous = 0.0
        for value in distances:
            if value <= previous:
                _fail(f"{name} must remain positive and strictly ascending")
            previous = value
    return GridChoice(
        grid_resolution=grid_resolution,
        chunk_quads=chunk_quads,
        lod_levels=lod_levels,
        lod_distances_cm=lod_distances,
        chunk_residency_distances_cm=residency,
    )


def _load_weightmap(path: Path, resolution: int, flip_vertical: bool) -> bytes:
    if path.suffix.lower() != ".png":
        _fail(f"weightmap must be a PNG: {path}")
    bit_depth, color_type = _png_layout(path)
    if bit_depth == 16 and color_type != 0:
        _fail(f"16-bit weightmap must be grayscale (color type 0): {path}")
    try:
        with Image.open(path) as opened:
            opened.load()
            if bit_depth == 16:
                # Houdini heightfield masks are grayscale I;16. PIL 的
                # convert("L") 会直接截断到低 8 位，必须先按 257 归一化。
                image = opened.point(lambda value: value / 257.0).convert("L")
            else:
                image = opened.convert("L")
    except Exception as error:
        _fail(f"cannot decode weightmap {path}: {error}")
    if flip_vertical:
        image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    if image.size != (resolution, resolution):
        image = image.resize((resolution, resolution), Image.Resampling.BILINEAR)
    return image.tobytes()


def _write_vlp1(
    path: Path,
    resolution: int,
    layer_weight_bytes: list[bytes],
) -> bool:
    layer_count = len(layer_weight_bytes)
    if layer_count < 1 or layer_count > 256:
        _fail("VLP1 requires 1..256 layers")
    if resolution < 2 or resolution > 4096:
        _fail("VLP1 resolution must be in [2, 4096]")
    sample_count = resolution * resolution
    for data in layer_weight_bytes:
        if len(data) != sample_count:
            _fail("weightmap sample count does not match the selected paint resolution")
    total_samples = sample_count * layer_count
    expected_size = VLP1_HEADER_BYTES + total_samples * 4 + total_samples
    if expected_size > VLP1_MAX_BYTES:
        _fail("VLP1 output exceeds the 512 MiB runtime budget")

    sums = array.array("H", [0]) * sample_count
    for data in layer_weight_bytes:
        for index, value in enumerate(data):
            sums[index] += value

    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(b"VLP1")
            stream.write(struct.pack("<5I", resolution, resolution, layer_count, 0, 0))
            for layer_index, data in enumerate(layer_weight_bytes):
                normalized = array.array("f", [0.0]) * sample_count
                for index, value in enumerate(data):
                    total = sums[index]
                    if total == 0:
                        normalized[index] = 1.0 if layer_index == 0 else 0.0
                    else:
                        normalized[index] = value / total
                if sys.byteorder != "little":
                    normalized.byteswap()
                stream.write(normalized.tobytes())
            stream.write(b"\xff" * total_samples)
            stream.flush()
            os.fsync(stream.fileno())
        payload = temporary.read_bytes()
        if len(payload) != expected_size:
            _fail("internal VLP1 size mismatch")
        if path.is_file():
            try:
                if path.read_bytes() == payload:
                    return False
            except OSError:
                pass
        os.replace(temporary, path)
        return True
    finally:
        if temporary.exists():
            temporary.unlink()


def _check_vlp1(path: Path, expected_layers: int) -> tuple[int, int, int]:
    data = path.read_bytes()
    if len(data) < VLP1_HEADER_BYTES or data[:4] != b"VLP1":
        _fail(f"invalid VLP1 file: {path}")
    width, height, layers, flags, reserved = struct.unpack_from("<5I", data, 4)
    if width != height or width < 2 or width > 4096:
        _fail(f"invalid VLP1 dimensions: {path}")
    if layers != expected_layers:
        _fail(
            f"VLP1 layer count {layers} does not match terrain layers {expected_layers}")
    if flags != 0 or reserved != 0:
        _fail(f"VLP1 flags/reserved must be zero: {path}")
    expected = VLP1_HEADER_BYTES + width * height * layers * 5
    if len(data) != expected:
        _fail(f"VLP1 file size mismatch: expected {expected}, got {len(data)}")
    return width, height, layers


def _texture_json(name: str, sources: list[str], color_space: str) -> dict[str, Any]:
    return {
        "name": name,
        "type": "textureArray",
        "sources": sources,
        "colorSpace": color_space,
        "mipmaps": True,
        "filter": "linear",
        "wrapMode": "repeat",
    }


def _convert(
    manifest_path: Path,
    resource_root: Path,
    texture_resolution_override: int | None,
    paint_resolution_override: int | None,
    verbose: bool,
) -> dict[str, Any]:
    _require_pillow()
    spec = _load_manifest(
        manifest_path, texture_resolution_override, paint_resolution_override)
    resource_root = resource_root.resolve()
    output_root = resource_root / spec.output_relative
    output_root.mkdir(parents=True, exist_ok=True)

    height_field = _load_height_field(spec.heightmap_path)
    if spec.flip_vertical:
        height_field = _flip_height_field(height_field)
    if height_field.width < 2 or height_field.height < 2:
        _fail("heightmap must be at least 2x2")

    grid = _choose_grid(
        height_field.width,
        height_field.height,
        spec.size_x_cm,
        spec.size_z_cm,
        spec.grid_resolution,
        spec.chunk_quads,
        spec.lod_levels,
        spec.lod_distances_cm,
        spec.chunk_residency_distances_cm,
    )
    extent = spec.texture_resolution
    layer_count = len(spec.layers)
    if layer_count > 256:
        _fail("terrain supports at most 256 layers")

    source_dir = _guard_output(output_root, output_root / "Source")
    textures_dir = _guard_output(output_root, output_root / "Textures")
    texture_source_dir = _guard_output(output_root, textures_dir / "Source")
    source_dir.mkdir(parents=True, exist_ok=True)
    texture_source_dir.mkdir(parents=True, exist_ok=True)

    changed_files = 0
    unchanged_files = 0

    heightmap_output = _guard_output(output_root, source_dir / "heightmap.png")
    if _write_png16(
            heightmap_output, height_field.width, height_field.height,
            height_field.as_u16()):
        changed_files += 1
    else:
        unchanged_files += 1

    albedo_sources: list[str] = []
    normal_sources: list[str] = []
    pbr_sources: list[str] = []
    displacement_sources: list[str] = []
    layer_json: list[dict[str, Any]] = []

    for index, layer in enumerate(spec.layers):
        albedo_image = _load_rgba8(layer.albedo, extent, spec.flip_vertical)
        avg_color = _average_albedo_linear(albedo_image)
        albedo_output = _guard_output(
            output_root, texture_source_dir / f"layer_{index:02d}_albedo.png")
        if _save_png_atomic(albedo_output, albedo_image):
            changed_files += 1
        else:
            unchanged_files += 1

        normal_image = _load_rgba8(
            layer.normal,
            extent,
            spec.flip_vertical,
            invert_green=spec.invert_normal_green,
        )
        normal_output = _guard_output(
            output_root, texture_source_dir / f"layer_{index:02d}_normal.png")
        if _save_png_atomic(normal_output, normal_image):
            changed_files += 1
        else:
            unchanged_files += 1

        height_source = _load_height_field(layer.height)
        height_values = _resize_height_field(
            height_source, extent, spec.flip_vertical)
        height_output = _guard_output(
            output_root, texture_source_dir / f"layer_{index:02d}_height16.png")
        if _write_png16(height_output, extent, extent, height_values):
            changed_files += 1
        else:
            unchanged_files += 1

        pbr_image = _load_rgba8(layer.pbr, extent, spec.flip_vertical)
        alpha_bytes = bytes(
            min(255, max(0, (int(value) + 128) // 257))
            for value in height_values)
        pbr_image.putalpha(Image.frombytes("L", (extent, extent), alpha_bytes))
        pbr_output = _guard_output(
            output_root, texture_source_dir / f"layer_{index:02d}_pbr.png")
        if _save_png_atomic(pbr_output, pbr_image):
            changed_files += 1
        else:
            unchanged_files += 1

        albedo_sources.append(_asset_path(resource_root, albedo_output))
        normal_sources.append(_asset_path(resource_root, normal_output))
        pbr_sources.append(_asset_path(resource_root, pbr_output))
        displacement_sources.append(_asset_path(resource_root, height_output))
        layer_key = "kitedemo." + layer.name
        if len(layer_key.encode("utf-8")) > 256:
            _fail(f"layer key is longer than 256 bytes: {layer.name}")
        layer_json.append({
            "name": layer.name,
            "key": layer_key,
            "heightRange": [layer.height_range[0], layer.height_range[1]],
            "slopeRange": [layer.slope_range[0], layer.slope_range[1]],
            "noise": layer.noise,
            "uvScale": layer.uv_scale,
            "uvRotation": layer.uv_rotation,
            "sourceResolution": extent,
            "avgColor": avg_color,
            "sourceSet": 0,
        })

    albedo_json_path = _guard_output(
        output_root, textures_dir / "T_terrain_kitedemo_albedo_array.json")
    normal_json_path = _guard_output(
        output_root, textures_dir / "T_terrain_kitedemo_normal_array.json")
    pbr_json_path = _guard_output(
        output_root, textures_dir / "T_terrain_kitedemo_pbr_array.json")
    albedo_json = _texture_json(
        "T_terrain_kitedemo_albedo_array", albedo_sources, "srgb")
    albedo_json["displacementSources"] = displacement_sources
    normal_json = _texture_json(
        "T_terrain_kitedemo_normal_array", normal_sources, "linear")
    pbr_json = _texture_json(
        "T_terrain_kitedemo_pbr_array", pbr_sources, "linear")
    for path, value in (
            (albedo_json_path, albedo_json),
            (normal_json_path, normal_json),
            (pbr_json_path, pbr_json)):
        if _write_json_atomic(path, value):
            changed_files += 1
        else:
            unchanged_files += 1

    placeholder_path = _guard_output(
        output_root, textures_dir / "T_terrain_kitedemo_data_placeholder.json")
    placeholder_json = {
        "name": "T_terrain_kitedemo_data_placeholder",
        "type": "texture",
        "source": "Common/Source/Textures/terrain_layer_placeholder.bmp",
        "colorSpace": "linear",
        "mipmaps": False,
        "filter": "nearest",
        "wrapMode": "clamp",
    }
    if _write_json_atomic(placeholder_path, placeholder_json):
        changed_files += 1
    else:
        unchanged_files += 1

    paint_path_value: str | None = None
    paint_summary: str
    has_paint = all(layer.weightmap is not None for layer in spec.layers)
    if has_paint:
        if spec.paint_resolution is not None:
            paint_resolution = spec.paint_resolution
            if paint_resolution < 2 or paint_resolution > 4096:
                _fail("paintResolution must be in [2, 4096]")
        else:
            first = spec.layers[0].weightmap
            assert first is not None
            with Image.open(first) as opened:
                paint_resolution = max(opened.size)

        weight_bytes = [
            _load_weightmap(layer.weightmap, paint_resolution, spec.flip_vertical)
            for layer in spec.layers if layer.weightmap is not None
        ]
        if len(weight_bytes) != layer_count:
            _fail("weightmap count does not match terrain layer count")
        paint_output = _guard_output(
            output_root,
            source_dir / f"terrain_{spec.asset_name}_paint.vlpaint")
        if _write_vlp1(paint_output, paint_resolution, weight_bytes):
            changed_files += 1
        else:
            unchanged_files += 1
        _check_vlp1(paint_output, layer_count)
        paint_path_value = _asset_path(resource_root, paint_output)
        paint_summary = (
            f"VLP1 {paint_resolution}x{paint_resolution}, {layer_count} layers")
    else:
        paint_summary = (
            "omitted: no UE weightmaps were supplied; imported weights remain export-only")

    mi_path = _guard_output(
        output_root, output_root / f"MI_terrain_{spec.asset_name}.json")
    mi_json = {
        "name": f"MI_terrain_{spec.asset_name}",
        "type": "materialInstance",
        "material": "shader/glsl/M_terrain.json",
        "parameters": {
            "u_uvScale": spec.material_uv_scale,
        },
        "textures": {
            "layerTable": _asset_path(resource_root, placeholder_path),
            "layerIds": _asset_path(resource_root, placeholder_path),
            "layerWeights": _asset_path(resource_root, placeholder_path),
            "pageTable": _asset_path(resource_root, placeholder_path),
            "layerAlbedo": _asset_path(resource_root, albedo_json_path),
            "layerNormal": _asset_path(resource_root, normal_json_path),
            "layerPbr": _asset_path(resource_root, pbr_json_path),
        },
    }
    if _write_json_atomic(mi_path, mi_json):
        changed_files += 1
    else:
        unchanged_files += 1

    tr_path = _guard_output(
        output_root, output_root / f"TR_terrain_{spec.asset_name}.json")
    tr_json: dict[str, Any] = {
        "name": f"TR_terrain_{spec.asset_name}",
        "type": "terrain",
        "description": spec.description or (
            "由导入清单中的高度图、绘制权重与层纹理生成的地形资产，"
            "用于验证 VulkanLearn 分层地形、纹理数组与 VLP1 权重快照；"
            "不参与论文数值验证。"
        ),
        "heightmapPath": _asset_path(resource_root, heightmap_output),
        "size": [_clean_float(spec.size_x_cm), _clean_float(spec.size_z_cm)],
        "heightScale": _clean_float(spec.height_scale_cm),
        "heightOffset": _clean_float(spec.height_offset_cm),
        "gridResolution": grid.grid_resolution,
        "chunkQuads": grid.chunk_quads,
        "lodLevels": grid.lod_levels,
        "lodDistances": list(grid.lod_distances_cm),
        "chunkResidencyDistances": list(grid.chunk_residency_distances_cm),
        "materialInstancePath": _asset_path(resource_root, mi_path),
        "layers": layer_json,
    }
    if paint_path_value is not None:
        tr_json["paintPath"] = paint_path_value
    if spec.heightmap_margin > 0:
        tr_json["heightmapMargin"] = spec.heightmap_margin
    if spec.page_origin is not None:
        tr_json["pageOrigin"] = list(spec.page_origin)
    if spec.page_tiles_per_side is not None:
        tr_json["pageTilesPerSide"] = spec.page_tiles_per_side
    if _write_json_atomic(tr_path, tr_json):
        changed_files += 1
    else:
        unchanged_files += 1

    longest = max(spec.size_x_cm, spec.size_z_cm)
    terrain_height = (
        abs(spec.height_scale_cm) + abs(spec.height_offset_cm) + longest * 0.08)
    scene_path = _guard_output(
        output_root, output_root / f"SC_terrain_{spec.asset_name}.json")
    scene_json = {
        "name": f"SC_terrain_{spec.asset_name}",
        "type": "scene",
        "description": spec.scene_description or (
            "地形导入查看场景：展示高度图地形、分层地表纹理数组、"
            "日光、环境天空与远景相机；用于资产导入验收，不参与论文验证。"
        ),
        "worldUnit": "cm",
        "objects": [
            {
                "name": "Landscape",
                "type": "terrain",
                "terrainPath": _asset_path(resource_root, tr_path),
                "position": [
                    _clean_float(value) for value in spec.terrain_position_cm],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            },
            {
                "name": "Sun_Light",
                "type": "directionalLight",
                "position": [0.0, max(1000.0, longest * 0.25), 0.0],
                "rotation": [-38.0, -32.0, 0.0],
                "color": [1.0, 0.96, 0.88],
                "intensity": 3.2,
                "intensityUnit": "lux",
                "shadow": {
                    "castShadows": False,
                    "dynamicShadowDistance": _clean_float(
                        max(20000.0, longest * 1.5)),
                },
            },
            {
                "name": "Camera_01",
                "type": "camera",
                "position": [
                    _clean_float(spec.camera_position_cm[0])
                    if spec.camera_position_cm is not None else 0.0,
                    _clean_float(spec.camera_position_cm[1])
                    if spec.camera_position_cm is not None
                    else _clean_float(max(500.0, terrain_height)),
                    _clean_float(spec.camera_position_cm[2])
                    if spec.camera_position_cm is not None
                    else _clean_float(longest * 0.55),
                ],
                "rotation": [
                    _clean_float(value) for value in spec.camera_rotation_deg],
                "scale": [1.0, 1.0, 1.0],
                "fov": 50.0,
                "near_clip": 10.0,
                "far_clip": _clean_float(max(60000.0, longest * 3.0)),
                "exposure": {
                    "mode": "ev100",
                    "ev100": 0.0,
                    "compensation": 0.0,
                    "aperture": 4.0,
                    "shutterSeconds": 0.008,
                    "iso": 100.0,
                    "autoMinEv100": -10.0,
                    "autoMaxEv100": 20.0,
                },
            },
            {
                "name": "Environment_01",
                "type": "environment",
                "environment": spec.environment or {
                    "type": "hdri",
                    "hdrPath": "Common/Environments/sunset.exr",
                    "cubeSize": 512,
                    "intensity": 1.0,
                },
            },
        ],
    }
    if _write_json_atomic(scene_path, scene_json):
        changed_files += 1
    else:
        unchanged_files += 1

    summary = {
        "status": "ok",
        "assetName": spec.asset_name,
        "manifest": str(spec.manifest_path),
        "outputRoot": str(output_root),
        "layers": layer_count,
        "heightmap": (
            f"{height_field.source_kind} {height_field.width}x{height_field.height}"),
        "gridResolution": grid.grid_resolution,
        "chunkQuads": grid.chunk_quads,
        "lodLevels": grid.lod_levels,
        "textureResolution": extent,
        "invertNormalGreen": spec.invert_normal_green,
        "paint": paint_summary,
        "filesChanged": changed_files,
        "filesUnchanged": unchanged_files,
    }
    if verbose:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    else:
        print(
            f"Imported {layer_count} layers -> {output_root}")
        print(
            f"Heightmap {summary['heightmap']}; grid {grid.grid_resolution}, "
            f"chunk {grid.chunk_quads}, LOD {grid.lod_levels}; textures {extent}.")
        print(f"Paint: {paint_summary}")
        print(
            f"Files changed: {changed_files}, unchanged: {unchanged_files}")
    return summary


def _save_self_test_png16(path: Path, width: int, height: int,
                          values: list[int]) -> None:
    image = Image.new("I;16", (width, height))
    image.putdata(values)
    image.save(path, format="PNG")


def _save_self_test_png8(path: Path, width: int, height: int,
                         values: bytes) -> None:
    image = Image.frombytes("L", (width, height), values)
    image.save(path, format="PNG")


def _self_test_output_hashes(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): _copy_bytes_for_hash(path)
        for path in sorted(root.rglob("*")) if path.is_file()
    }


def _self_test_verify(
    resource_root: Path,
    summary: dict[str, Any],
    source_hashes: dict[str, str],
) -> None:
    output_root = resource_root / OUTPUT_RELATIVE
    tr_path = output_root / "TR_terrain_kitedemo.json"
    sc_path = output_root / "SC_terrain_kitedemo.json"
    mi_path = output_root / "MI_terrain_kitedemo.json"
    for path in (tr_path, sc_path, mi_path):
        if not path.is_file():
            _fail(f"self-test missing output {path}")
    tr = json.loads(tr_path.read_text(encoding="utf-8"))
    sc = json.loads(sc_path.read_text(encoding="utf-8"))
    mi = json.loads(mi_path.read_text(encoding="utf-8"))
    if tr.get("type") != "terrain" or len(tr.get("layers", [])) != 3:
        _fail("self-test TR type/layer count is wrong")
    if not tr.get("description"):
        _fail("self-test TR description is empty")
    if tr["gridResolution"] % tr["chunkQuads"] != 0:
        _fail("self-test gridResolution is not divisible by chunkQuads")
    if tr["chunkQuads"] % (1 << (tr["lodLevels"] - 1)) != 0:
        _fail("self-test chunkQuads is not divisible by the coarsest LOD step")
    if (tr["gridResolution"], tr["chunkQuads"], tr["lodLevels"]) != (6, 2, 2):
        _fail("self-test explicit grid/chunk/LOD overrides were not honored")
    if tr.get("lodDistances") != [400.0]:
        _fail("self-test explicit lodDistances override was not honored")
    if tr.get("chunkResidencyDistances") != [440.0]:
        _fail("self-test explicit chunkResidencyDistances override was not honored")
    if tr.get("pageOrigin") != [8, 8]:
        _fail("self-test explicit pageOrigin override was not honored")
    if tr.get("pageTilesPerSide") != 8:
        _fail("self-test explicit pageTilesPerSide override was not honored")
    if len(tr.get("lodDistances", [])) != tr["lodLevels"] - 1:
        _fail("self-test lodDistances length is wrong")
    if all(layer.get("sourceResolution") != 512 for layer in tr["layers"]):
        _fail("self-test layer sourceResolution was not recorded as 512")
    if not tr.get("paintPath"):
        _fail("self-test VLP1 paintPath is missing")
    paint_path = resource_root / tr["paintPath"]
    width, height, layers = _check_vlp1(paint_path, 3)
    if (width, height, layers) != (8, 8, 3):
        _fail("self-test VLP1 header dimensions/layers are wrong")
    paint_data = paint_path.read_bytes()
    weights = struct.unpack_from(f"<{8 * 8 * 3}f", paint_data, VLP1_HEADER_BYTES)
    for texel in range(64):
        total = sum(weights[layer * 64 + texel] for layer in range(3))
        if abs(total - 1.0) > 1.0e-4:
            _fail("self-test VLP1 per-texel weights do not sum to one")
    opacity = paint_data[VLP1_HEADER_BYTES + 8 * 8 * 3 * 4:]
    if opacity != b"\xff" * (8 * 8 * 3):
        _fail("self-test VLP1 opacity is not fully covered")

    objects = sc.get("objects", [])
    object_types = {obj.get("type") for obj in objects if isinstance(obj, dict)}
    if not {"terrain", "directionalLight", "camera", "environment"} <= object_types:
        _fail("self-test scene is missing a required object type")
    if mi.get("material") != "shader/glsl/M_terrain.json":
        _fail("self-test MI does not target M_terrain")
    if "u_terrainLayerMode" in mi.get("parameters", {}):
        _fail("self-test MI redundantly overrides the material-default u_terrainLayerMode")
    if not {"layerAlbedo", "layerNormal", "layerPbr"} <= set(mi.get("textures", {})):
        _fail("self-test MI does not bind the three layer arrays")

    albedo_array_path = resource_root / mi["textures"]["layerAlbedo"]
    albedo_array = json.loads(albedo_array_path.read_text(encoding="utf-8"))
    if len(albedo_array.get("sources", [])) != 3:
        _fail("self-test albedo array source count is wrong")
    if len(albedo_array.get("displacementSources", [])) != 3:
        _fail("self-test displacementSources count is wrong")
    for relative in albedo_array["displacementSources"]:
        path = resource_root / relative
        if _png_layout(path) != (16, 0):
            _fail(f"self-test displacement is not a 16-bit grayscale PNG: {path}")

    for array_key in ("layerNormal", "layerPbr"):
        array = json.loads(
            (resource_root / mi["textures"][array_key]).read_text(encoding="utf-8"))
        if len(array.get("sources", [])) != 3:
            _fail(f"self-test {array_key} source count is wrong")
        if array_key == "layerNormal":
            with Image.open(resource_root / array["sources"][0]) as image:
                image.load()
                red, green, blue, alpha = image.convert("RGBA").getpixel((0, 0))
                if (red, green, blue, alpha) != (128, 127, 255, 255):
                    _fail(
                        "self-test normal green inversion changed the wrong channels: "
                        f"got {(red, green, blue, alpha)}")
    for relative in albedo_array["sources"]:
        with Image.open(resource_root / relative) as image:
            if image.size != (512, 512):
                _fail(f"self-test texture was not normalized to 512: {relative}")

    height_output = resource_root / tr["heightmapPath"]
    if _png_layout(height_output) != (16, 0):
        _fail("self-test heightmap output is not PNG16 grayscale")
    with Image.open(height_output) as image:
        image.load()
        if image.size != (16, 8):
            _fail("self-test heightmap dimensions changed")

    eight_bit_height = resource_root / albedo_array["displacementSources"][0]
    with Image.open(eight_bit_height) as image:
        image.load()
        values = _flattened_pixels(image)
        if values[0] != 0 or values[1] != 257 or values[17] != 17 * 257:
            _fail("self-test 8-bit height was not promoted by v * 257")

    sixteen_bit_height = resource_root / albedo_array["displacementSources"][1]
    with Image.open(sixteen_bit_height) as image:
        image.load()
        values = _flattened_pixels(image)
        if values[0] != 0 or values[1] != 257 or values[17] != 17 * 257:
            _fail("self-test 16-bit height samples were not preserved")

    for relative, digest in source_hashes.items():
        path = resource_root / "inputs" / relative
        if _copy_bytes_for_hash(path) != digest:
            _fail(f"self-test modified an input file: {path}")

    if summary.get("filesChanged", 0) <= 0:
        _fail("self-test conversion reported no changed files")


def _run_self_test(verbose: bool) -> None:
    _require_pillow()
    with tempfile.TemporaryDirectory(prefix="vl-kitedemo-selftest-") as temporary_name:
        root = Path(temporary_name)
        inferred = _choose_grid(2048, 2048, 20000.0, 20000.0)
        if (inferred.grid_resolution, inferred.chunk_quads, inferred.lod_levels) != (
                1984, 64, 2):
            _fail("self-test inferred grid fallback changed unexpectedly")
        resource_root = root / "Resources"
        inputs = resource_root / "inputs"
        inputs.mkdir(parents=True)
        export = inputs / "export"
        export.mkdir()

        height_values = [
            ((x * 131 + y * 977) & 0xFFFF)
            for y in range(8) for x in range(16)
        ]
        height16 = export / "height16.png"
        _save_self_test_png16(height16, 16, 8, height_values)

        height8_values = bytes((x & 0xFF)
                               for _y in range(512) for x in range(512))
        height8_texture = export / "layer0_h.png"
        _save_self_test_png8(height8_texture, 512, 512, height8_values)
        height16_values = [((x * 257 + y * 17) & 0xFFFF)
                           for y in range(512) for x in range(512)]
        height16_texture = export / "layer1_h.png"
        _save_self_test_png16(height16_texture, 512, 512, height16_values)
        height8_texture2 = export / "layer2_h.png"
        _save_self_test_png8(height8_texture2, 512, 512, bytes([0]) * (512 * 512))

        layer_specs = [
            ("Grass", (44, 116, 48, 255), height8_texture, "ORM"),
            ("Rock", (112, 106, 96, 255), height16_texture, "PBR"),
            ("Mud", (119, 86, 52, 255), height8_texture2, "MASK"),
        ]
        layers = []
        weightmaps = {}
        for index, (name, color, height_path, suffix) in enumerate(layer_specs):
            albedo = export / f"layer{index}_D_R.png"
            normal = export / f"layer{index}_N.png"
            pbr = export / f"layer{index}_{suffix}.png"
            Image.new("RGBA", (512, 512), color).save(albedo)
            Image.new("RGBA", (512, 512), (128, 128, 255, 255)).save(normal)
            Image.new("RGBA", (512, 512), (180, 20, 240, 255)).save(pbr)

            weightmap = export / f"weight_{name}.png"
            weight_values = [
                255 if index == 0 else (96 if index == 1 else 0)
            ] * (8 * 8)
            _save_self_test_png8(
                weightmap, 8, 8, bytes(weight_values))
            weightmaps[name] = weightmap.relative_to(root).as_posix()
            layers.append({
                "name": name,
                "files": [
                    albedo.relative_to(root).as_posix(),
                    normal.relative_to(root).as_posix(),
                    pbr.relative_to(root).as_posix(),
                    height_path.relative_to(root).as_posix(),
                ],
                "heightRange": [0.0, 1.0],
                "slopeRange": [0.0, 90.0],
            })

        manifest = {
            "heightmapPath": height16.relative_to(root).as_posix(),
            "size": [1600.0, 800.0],
            "heightScale": 250.0,
            "heightOffset": 10.0,
            "textureResolution": 512,
            "paintResolution": 8,
            "materialUvScale": 6.0,
            "gridResolution": 6,
            "chunkQuads": 2,
            "lodLevels": 2,
            "lodDistances": [400.0],
            "chunkResidencyDistances": [440.0],
            "pageOrigin": [8, 8],
            "pageTilesPerSide": 8,
            "weightmaps": weightmaps,
            "layers": layers,
        }
        manifest_path = root / "manifest.json"
        manifest_path.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2),
            encoding="utf-8")
        source_hashes = {
            path.relative_to(inputs).as_posix(): _copy_bytes_for_hash(path)
            for path in inputs.rglob("*") if path.is_file()
        }
        summary = _convert(
            manifest_path, resource_root, None, None, False)
        _self_test_verify(resource_root, summary, source_hashes)
        first_hashes = _self_test_output_hashes(resource_root / OUTPUT_RELATIVE)
        second = _convert(
            manifest_path, resource_root, None, None, False)
        second_hashes = _self_test_output_hashes(resource_root / OUTPUT_RELATIVE)
        if first_hashes != second_hashes:
            _fail("self-test output is not deterministic/idempotent")
        if second.get("filesChanged", 0) != 0:
            _fail("self-test second run rewrote unchanged files")
        override_manifest = dict(manifest)
        override_manifest["invertNormalGreen"] = False
        override_path = root / "manifest_no_normal_green_invert.json"
        override_path.write_text(
            json.dumps(override_manifest, ensure_ascii=False, indent=2),
            encoding="utf-8")
        override_spec = _load_manifest(override_path, None, None)
        if override_spec.invert_normal_green:
            _fail("self-test invertNormalGreen=false override was ignored")

        invalid_cases = [
            (
                "grid_not_divisible",
                {
                    "gridResolution": 7,
                    "chunkQuads": 4,
                    "lodLevels": 1,
                    "lodDistances": None,
                    "chunkResidencyDistances": None,
                },
            ),
            (
                "chunk_not_divisible_by_lod",
                {
                    "gridResolution": 56,
                    "chunkQuads": 28,
                    "lodLevels": 4,
                    "lodDistances": [100.0, 200.0, 300.0],
                    "chunkResidencyDistances": [110.0, 220.0, 330.0],
                },
            ),
            (
                "chunk_larger_than_grid",
                {"gridResolution": 6, "chunkQuads": 64, "lodLevels": 2},
            ),
            (
                "lod_distance_count_mismatch",
                {
                    "gridResolution": 6,
                    "chunkQuads": 2,
                    "lodLevels": 2,
                    "lodDistances": [100.0, 200.0],
                    "chunkResidencyDistances": [110.0],
                },
            ),
            (
                "lod_distance_not_ascending",
                {
                    "gridResolution": 6,
                    "chunkQuads": 2,
                    "lodLevels": 2,
                    "lodDistances": [400.0, 300.0],
                    "chunkResidencyDistances": [440.0],
                },
            ),
        ]
        for label, updates in invalid_cases:
            invalid_manifest = dict(manifest)
            invalid_manifest.update(updates)
            invalid_path = root / f"manifest_invalid_{label}.json"
            invalid_path.write_text(
                json.dumps(invalid_manifest, ensure_ascii=False, indent=2),
                encoding="utf-8")
            try:
                invalid_spec = _load_manifest(invalid_path, None, None)
                _choose_grid(
                    16,
                    8,
                    invalid_spec.size_x_cm,
                    invalid_spec.size_z_cm,
                    invalid_spec.grid_resolution,
                    invalid_spec.chunk_quads,
                    invalid_spec.lod_levels,
                    invalid_spec.lod_distances_cm,
                    invalid_spec.chunk_residency_distances_cm,
                )
            except AssetImportError:
                continue
            _fail(f"self-test invalid grid/LOD case was accepted: {label}")
        print(
            "SELF-TEST PASS: PNG16/PNG8 height, normalized 512px arrays, "
            "DirectX->OpenGL normal green inversion, 16-bit displacements, "
            "3-layer VLP1, explicit grid/LOD overrides, invalid override rejection, "
            "terrain/scene/MI JSON, "
            "deterministic second run.")
        if verbose:
            print(json.dumps(second, ensure_ascii=False, indent=2))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Convert UE KiteDemo terrain exports into VulkanLearn assets.")
    parser.add_argument("manifest", nargs="?", type=Path,
                        help="UE export manifest JSON")
    parser.add_argument(
        "--resource-root", type=Path, default=DEFAULT_RESOURCE_ROOT,
        help="VulkanLearn resource root; output is always Generated/KiteDemo")
    parser.add_argument(
        "--texture-resolution", type=int,
        choices=SUPPORTED_TEXTURE_RESOLUTIONS,
        help="override manifest textureResolution")
    parser.add_argument(
        "--paint-resolution", type=int,
        help="override manifest paintResolution (VLP1 side length)")
    parser.add_argument(
        "--self-test", action="store_true",
        help="generate temporary PNG16/PNG8 inputs and validate all outputs")
    parser.add_argument(
        "--verbose", action="store_true", help="print a JSON summary")
    args = parser.parse_args(argv)

    try:
        if args.self_test:
            _run_self_test(args.verbose)
            return 0
        if args.manifest is None:
            parser.error("manifest is required unless --self-test is used")
        _convert(
            args.manifest,
            args.resource_root,
            args.texture_resolution,
            args.paint_resolution,
            args.verbose,
        )
        return 0
    except AssetImportError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

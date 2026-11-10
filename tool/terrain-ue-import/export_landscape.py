"""UE 5.8 地形导出器（只读 UE 资产，只写指定输出目录）。

用法（在 Unreal Editor 的 Python 控制台执行）：
  py "<此脚本绝对路径>" landscape --output-dir "<输出目录>" [--landscape "<名称或对象路径>"]
     [--resolution 2048x2048] [--texture-assets /Game/... /Game/...]
  py "<此脚本绝对路径>" textures --output-dir "<输出目录>" --assets /Game/... /Game/...

默认输出目录：
  D:\\YYBWorkSpace\\GitHub\\VulkanLearn-worktrees\\terrain-openworld\\VukanLearnResources\\Generated\\KiteDemo

高度图路径：使用 ALandscape.render_heightmap，将引擎内部的 16-bit height 经
RTF_RGBA8 的 COMPRESS_HEIGHT 模式无损拆成 R/G 两字节，再由本脚本还原为
16-bit 灰度 PNG 和 little-endian uint16 .r16。UE 5.8 Python 没有暴露
PF_G16 渲染目标或 PF_G16 直接落盘接口，因此不声称导出的是原生 G16 文件；
manifest 会明确记录实际编码。

权重图路径：逐层调用 ALandscape.render_weightmap，写成一张 8-bit 灰度 PNG。
纹理资产：使用 unreal.AssetToolsHelpers/AssetTools.export_assets，非交互；
若 UAssetImportData 保留源文件，则额外复制原始源图字节，避免导出器量化源位深。

限制：必须在 UE 5.8 编辑器 Python 环境内运行；依赖当前打开的 editor world 和
已加载的 ALandscape。大分辨率 RGBA8 PNG 的解码/重编码是 CPU 操作，可能较慢。
脚本不会修改 UE 资产、关卡或纹理项目，也不会向 `effect` 项目写入任何文件。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import time
import traceback
import zlib
from array import array
from pathlib import Path
from typing import Any, Iterable, Sequence

import unreal


EXPORTER_VERSION = "1.0.0"
DEFAULT_OUTPUT_DIR = Path(
    r"D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees"
    r"\terrain-openworld\VukanLearnResources\Generated\KiteDemo"
)
FORBIDDEN_OUTPUT_ROOT = Path(r"D:\YYBWorkSpace\UnrealProject\effect")
MAX_RENDER_TARGET_DIMENSION = 16384
LANDSCAPE_ZSCALE = 1.0 / 128.0
LANDSCAPE_MID_VALUE = 32768.0
LANDSCAPE_MAX_VALUE = 65535.0
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_INVALID_FILENAME = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


class ExportError(RuntimeError):
    pass


def _log_info(message: str) -> None:
    try:
        unreal.log(message)
    except Exception:
        print(message)


def _log_error(message: str) -> None:
    try:
        unreal.log_error(message)
    except Exception:
        print(message, file=sys.stderr)


def _canonical(path: os.PathLike[str] | str) -> str:
    return os.path.normcase(os.path.realpath(os.fspath(path)))


def _is_within(root: os.PathLike[str] | str, path: os.PathLike[str] | str) -> bool:
    try:
        return os.path.commonpath([_canonical(root), _canonical(path)]) == _canonical(root)
    except ValueError:
        return False


def _assert_not_effect_root(output_root: Path) -> None:
    if _is_within(FORBIDDEN_OUTPUT_ROOT, output_root):
        raise ExportError(
            f"Refusing output under the UE effect project: {output_root}"
        )


def _assert_write_path(output_root: Path, path: Path) -> None:
    if _is_within(FORBIDDEN_OUTPUT_ROOT, path):
        raise ExportError(
            f"Refusing write into the UE effect project: {path}"
        )
    if not _is_within(output_root, path):
        raise ExportError(
            f"Refusing write outside output directory: {path} (root={output_root})"
        )


def _resolve_output_dir(raw: str | None) -> Path:
    value = Path(raw) if raw else DEFAULT_OUTPUT_DIR
    if not value.is_absolute():
        project_dir = Path.cwd()
        try:
            project_dir = Path(
                unreal.Paths.convert_relative_path_to_full(
                    unreal.Paths.project_dir()
                )
            )
        except Exception:
            pass
        value = project_dir / value
    value = value.resolve()
    _assert_not_effect_root(value)
    value.mkdir(parents=True, exist_ok=True)
    return value


def _property(
    obj: Any,
    *names: str,
    default: Any = None,
) -> Any:
    for name in names:
        try:
            value = getattr(obj, name)
        except Exception:
            value = None
        if value is not None:
            return value
        try:
            value = obj.get_editor_property(name)
        except Exception:
            value = None
        if value is not None:
            return value
    return default


def _vector_dict(value: Any) -> dict[str, float]:
    return {
        "x": float(_property(value, "x", default=0.0)),
        "y": float(_property(value, "y", default=0.0)),
        "z": float(_property(value, "z", default=0.0)),
    }


def _vector2_dict(value: Any) -> dict[str, float]:
    return {
        "x": float(_property(value, "x", default=0.0)),
        "y": float(_property(value, "y", default=0.0)),
    }


def _quat_dict(value: Any) -> dict[str, float]:
    return {
        "x": float(_property(value, "x", default=0.0)),
        "y": float(_property(value, "y", default=0.0)),
        "z": float(_property(value, "z", default=0.0)),
        "w": float(_property(value, "w", default=1.0)),
    }


def _transform_dict(value: Any) -> dict[str, Any]:
    return {
        "translation": _vector_dict(_property(value, "translation")),
        "rotation_quat": _quat_dict(_property(value, "rotation")),
        "scale": _vector_dict(_property(value, "scale3d")),
    }


def _path_record(output_root: Path, path: Path, kind: str = "file") -> dict[str, Any]:
    _assert_write_path(output_root, path)
    return {
        "absolute": str(path.resolve()),
        "relative": os.path.relpath(path.resolve(), output_root).replace("\\", "/"),
        "kind": kind,
    }


def _safe_filename(value: str, fallback: str) -> str:
    cleaned = _INVALID_FILENAME.sub("_", value).strip(" .")
    return (cleaned[:120] or fallback)


def _safe_texture_value(value: Any) -> str:
    text = str(value)
    return re.sub(r"[^0-9A-Za-z._-]+", "_", text).strip("_") or "texture"


def _get_editor_world() -> unreal.World:
    try:
        subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
        world = subsystem.get_editor_world()
    except Exception:
        world = None
    if world is None:
        try:
            world = unreal.EditorLevelLibrary.get_editor_world()
        except Exception:
            world = None
    if world is None:
        raise ExportError("No current editor world was found.")
    return world


def _get_actor_subsystem() -> unreal.EditorActorSubsystem:
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if subsystem is None:
        raise ExportError("EditorActorSubsystem is unavailable.")
    return subsystem


def _actor_record(actor: unreal.Actor) -> dict[str, Any]:
    label = _property(actor, "actor_label", default=None)
    return {
        "name": actor.get_name(),
        "path": actor.get_path_name(),
        "label": str(label) if label else actor.get_name(),
        "class": actor.get_class().get_name(),
    }


def _select_landscape(
    actor_subsystem: unreal.EditorActorSubsystem,
    selector: str | None,
) -> unreal.Landscape:
    landscapes = [
        actor
        for actor in actor_subsystem.get_all_level_actors()
        if isinstance(actor, unreal.Landscape)
    ]
    if not landscapes:
        raise ExportError("No ALandscape actor is loaded in the current editor world.")

    if selector:
        selector_lower = selector.casefold()
        matches = []
        for actor in landscapes:
            candidates = {
                actor.get_name().casefold(),
                actor.get_path_name().casefold(),
                str(_property(actor, "actor_label", default="")).casefold(),
            }
            if selector_lower in candidates:
                matches.append(actor)
        if len(matches) == 1:
            return matches[0]
        available = ", ".join(
            f"{actor.get_name()} ({actor.get_path_name()})" for actor in landscapes
        )
        if not matches:
            raise ExportError(
                f"Landscape selector '{selector}' did not match. Available: {available}"
            )
        raise ExportError(
            f"Landscape selector '{selector}' is ambiguous. Available: {available}"
        )

    selected = [
        actor
        for actor in actor_subsystem.get_selected_level_actors()
        if isinstance(actor, unreal.Landscape)
    ]
    if len(selected) == 1:
        return selected[0]
    if len(selected) > 1:
        raise ExportError(
            "Multiple ALandscape actors are selected. Select exactly one or pass --landscape."
        )
    if len(landscapes) == 1:
        return landscapes[0]

    available = ", ".join(
        f"{actor.get_name()} ({actor.get_path_name()})" for actor in landscapes
    )
    raise ExportError(
        "No unambiguous landscape was selected. Select one ALandscape or pass "
        f"--landscape. Available: {available}"
    )


def _collect_components(
    actor: unreal.Landscape,
) -> tuple[list[dict[str, Any]], int, int, int]:
    component_class = unreal.LandscapeComponent.static_class()
    components = actor.get_components_by_class(component_class)
    if not components:
        raise ExportError(
            f"Landscape '{actor.get_name()}' has no loaded LandscapeComponent objects."
        )

    actor_component_size = int(
        _property(actor, "component_size_quads", default=0) or 0
    )
    actor_subsection_size = int(
        _property(actor, "subsection_size_quads", default=0) or 0
    )
    actor_num_subsections = int(
        _property(actor, "num_subsections", default=0) or 0
    )

    records: list[dict[str, Any]] = []
    for component in components:
        section_x = int(_property(component, "section_base_x", default=0) or 0)
        section_y = int(_property(component, "section_base_y", default=0) or 0)
        component_size = int(
            _property(
                component,
                "component_size_quads",
                default=actor_component_size,
            )
            or actor_component_size
        )
        subsection_size = int(
            _property(
                component,
                "subsection_size_quads",
                default=actor_subsection_size,
            )
            or actor_subsection_size
        )
        num_subsections = int(
            _property(
                component,
                "num_subsections",
                default=actor_num_subsections,
            )
            or actor_num_subsections
        )
        if component_size <= 0:
            raise ExportError(
                f"Landscape component '{component.get_name()}' has an invalid "
                "component_size_quads."
            )

        try:
            origin, extent, sphere_radius = unreal.SystemLibrary.get_component_bounds(
                component
            )
        except Exception as exc:
            raise ExportError(
                f"Could not read component bounds for {component.get_path_name()}: {exc}"
            ) from exc

        origin_dict = _vector_dict(origin)
        extent_dict = _vector_dict(extent)
        records.append(
            {
                "name": component.get_name(),
                "path": component.get_path_name(),
                "section_base": {"x": section_x, "y": section_y},
                "component_size_quads": component_size,
                "subsection_size_quads": subsection_size,
                "num_subsections": num_subsections,
                "bounds": {
                    "origin": origin_dict,
                    "extent": extent_dict,
                    "min": {
                        "x": origin_dict["x"] - extent_dict["x"],
                        "y": origin_dict["y"] - extent_dict["y"],
                        "z": origin_dict["z"] - extent_dict["z"],
                    },
                    "max": {
                        "x": origin_dict["x"] + extent_dict["x"],
                        "y": origin_dict["y"] + extent_dict["y"],
                        "z": origin_dict["z"] + extent_dict["z"],
                    },
                    "sphere_radius": float(sphere_radius),
                },
            }
        )

    if actor_component_size <= 0:
        actor_component_size = records[0]["component_size_quads"]
    if actor_subsection_size <= 0:
        actor_subsection_size = records[0]["subsection_size_quads"]
    if actor_num_subsections <= 0:
        actor_num_subsections = records[0]["num_subsections"]
    return records, actor_component_size, actor_subsection_size, actor_num_subsections


def _component_grid_extents(
    components: Sequence[dict[str, Any]],
) -> tuple[int, int, int, int]:
    min_x = min(component["section_base"]["x"] for component in components)
    min_y = min(component["section_base"]["y"] for component in components)
    max_x = max(
        component["section_base"]["x"] + component["component_size_quads"]
        for component in components
    )
    max_y = max(
        component["section_base"]["y"] + component["component_size_quads"]
        for component in components
    )
    if max_x <= min_x or max_y <= min_y:
        raise ExportError("Landscape component grid extent is empty.")
    return min_x, min_y, max_x, max_y


def _make_box2d(min_x: int, min_y: int, max_x: int, max_y: int) -> unreal.Box2D:
    # FBox2D 的默认构造不会把 bIsValid 设为 true；必须走引擎的 NativeMake，
    # 否则 RenderHeightmap/RenderWeightmap 会忽略我们传入的局部范围。
    return unreal.KismetMathLibrary.make_box2d(
        unreal.Vector2D(float(min_x), float(min_y)),
        unreal.Vector2D(float(max_x), float(max_y)),
    )


def _parse_resolution(value: str | None) -> tuple[int, int] | None:
    if not value:
        return None
    match = re.fullmatch(r"\s*(\d+)\s*[xX]\s*(\d+)\s*", value)
    if not match:
        raise ExportError("--resolution must use WIDTHxHEIGHT, for example 2048x2048.")
    width, height = int(match.group(1)), int(match.group(2))
    if width < 2 or height < 2:
        raise ExportError("--resolution dimensions must be at least 2.")
    if (
        width > MAX_RENDER_TARGET_DIMENSION
        or height > MAX_RENDER_TARGET_DIMENSION
    ):
        raise ExportError(
            f"--resolution exceeds the supported render-target limit "
            f"{MAX_RENDER_TARGET_DIMENSION}."
        )
    return width, height


def _create_rgba8_render_target(
    world: unreal.World,
    width: int,
    height: int,
) -> unreal.TextureRenderTarget2D:
    target = unreal.KismetRenderingLibrary.create_render_target2d(
        world,
        width,
        height,
        unreal.TextureRenderTargetFormat.RTF_RGBA8,
        unreal.LinearColor(0.0, 0.0, 0.0, 0.0),
        False,
        False,
    )
    if target is None:
        raise ExportError(f"Could not create a {width}x{height} RTF_RGBA8 render target.")
    return target


def _export_render_target_png(
    render_target: unreal.TextureRenderTarget2D,
    path: Path,
    output_root: Path,
) -> None:
    _assert_write_path(output_root, path)
    if path.exists():
        path.unlink()
    options = unreal.ImageWriteOptions()
    options.format = unreal.DesiredImageFormat.PNG
    options.compression_quality = 0
    options.b_overwrite_file = True
    options.b_async = False
    unreal.ImageWriteBlueprintLibrary.export_to_disk(
        render_target,
        str(path),
        options,
    )
    if not path.is_file() or path.stat().st_size <= 0:
        raise ExportError(
            f"ImageWriteBlueprintLibrary.export_to_disk produced no file: {path}"
        )


def _paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    distance_left = abs(estimate - left)
    distance_above = abs(estimate - above)
    distance_upper_left = abs(estimate - upper_left)
    if distance_left <= distance_above and distance_left <= distance_upper_left:
        return left
    if distance_above <= distance_upper_left:
        return above
    return upper_left


def _read_png8(path: Path) -> tuple[int, int, int, bytearray]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise ExportError(f"Not a PNG file: {path}")

    offset = len(PNG_SIGNATURE)
    header: tuple[int, int, int, int, int, int, int] | None = None
    idat = bytearray()
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4 : offset + 8]
        chunk_data = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if chunk_type == b"IHDR":
            header = struct.unpack(">IIBBBBB", chunk_data)
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if header is None:
        raise ExportError(f"PNG has no IHDR chunk: {path}")
    width, height, bit_depth, color_type, compression, filter_method, interlace = header
    if compression != 0 or filter_method != 0:
        raise ExportError(f"Unsupported PNG compression/filter method: {path}")
    if interlace != 0:
        raise ExportError(f"Interlaced PNG is unsupported: {path}")
    if bit_depth != 8:
        raise ExportError(f"Expected an 8-bit intermediate PNG, got {bit_depth}: {path}")
    channels_by_color_type = {0: 1, 2: 3, 4: 2, 6: 4}
    if color_type not in channels_by_color_type:
        raise ExportError(f"Unsupported PNG color type {color_type}: {path}")
    channels = channels_by_color_type[color_type]
    stride = width * channels

    raw = zlib.decompress(bytes(idat))
    expected = height * (stride + 1)
    if len(raw) != expected:
        raise ExportError(
            f"PNG payload size mismatch: expected {expected}, got {len(raw)} ({path})"
        )

    pixels = bytearray(height * stride)
    previous = bytearray(stride)
    source_offset = 0
    for row_index in range(height):
        filter_type = raw[source_offset]
        source_offset += 1
        row = bytearray(raw[source_offset : source_offset + stride])
        source_offset += stride
        row_start = row_index * stride

        if filter_type == 1:
            for index in range(channels, stride):
                row[index] = (row[index] + row[index - channels]) & 0xFF
        elif filter_type == 2:
            for index in range(stride):
                row[index] = (row[index] + previous[index]) & 0xFF
        elif filter_type == 3:
            for index in range(stride):
                left = row[index - channels] if index >= channels else 0
                above = previous[index]
                row[index] = (row[index] + ((left + above) >> 1)) & 0xFF
        elif filter_type == 4:
            for index in range(stride):
                left = row[index - channels] if index >= channels else 0
                above = previous[index]
                upper_left = previous[index - channels] if index >= channels else 0
                row[index] = (
                    row[index] + _paeth(left, above, upper_left)
                ) & 0xFF
        elif filter_type != 0:
            raise ExportError(
                f"Unsupported PNG scanline filter {filter_type}: {path}"
            )

        pixels[row_start : row_start + stride] = row
        previous = row

    return width, height, channels, pixels


def _decode_packed_height(
    path: Path,
    expected_width: int,
    expected_height: int,
) -> array:
    width, height, channels, pixels = _read_png8(path)
    if (width, height) != (expected_width, expected_height):
        raise ExportError(
            f"Heightmap render target returned {width}x{height}, expected "
            f"{expected_width}x{expected_height}."
        )
    if channels < 2:
        raise ExportError(
            "Heightmap intermediate lacks the RG channels required by "
            "Landscape COMPRESS_HEIGHT."
        )
    values = array("H", [0]) * (width * height)
    for index in range(width * height):
        sample = index * channels
        values[index] = (pixels[sample] << 8) | pixels[sample + 1]
    return values


def _decode_gray8(
    path: Path,
    expected_width: int,
    expected_height: int,
) -> bytearray:
    width, height, channels, pixels = _read_png8(path)
    if (width, height) != (expected_width, expected_height):
        raise ExportError(
            f"Weightmap render target returned {width}x{height}, expected "
            f"{expected_width}x{expected_height}."
        )
    if channels == 1:
        return bytearray(pixels)
    values = bytearray(width * height)
    for index in range(width * height):
        values[index] = pixels[index * channels]
    return values


def _png_chunk(chunk_type: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + chunk_type
        + payload
        + struct.pack(">I", zlib.crc32(chunk_type + payload) & 0xFFFFFFFF)
    )


def _write_atomic(path: Path, data: bytes, output_root: Path) -> None:
    _assert_write_path(output_root, path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        suffix=".tmp",
        dir=str(path.parent),
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        temporary.write_bytes(data)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _write_gray_png(
    path: Path,
    width: int,
    height: int,
    values: Sequence[int] | array,
    bit_depth: int,
    output_root: Path,
) -> None:
    if bit_depth not in (8, 16):
        raise ExportError(f"Unsupported grayscale PNG bit depth: {bit_depth}")
    expected = width * height
    if len(values) != expected:
        raise ExportError(
            f"Grayscale payload length mismatch: expected {expected}, got {len(values)}"
        )

    if bit_depth == 8:
        rows = bytearray((width + 1) * height)
        for y in range(height):
            source = y * width
            destination = y * (width + 1)
            rows[destination] = 0
            rows[destination + 1 : destination + 1 + width] = bytes(
                values[source : source + width]
            )
    else:
        rows = bytearray((width * 2 + 1) * height)
        for y in range(height):
            destination = y * (width * 2 + 1)
            rows[destination] = 0
            destination += 1
            source = y * width
            for x in range(width):
                struct.pack_into(">H", rows, destination + x * 2, int(values[source + x]))

    ihdr = struct.pack(
        ">IIBBBBB",
        width,
        height,
        bit_depth,
        0,
        0,
        0,
        0,
    )
    png = (
        PNG_SIGNATURE
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(bytes(rows), level=9))
        + _png_chunk(b"IEND", b"")
    )
    _write_atomic(path, png, output_root)


def _write_r16(path: Path, values: array, output_root: Path) -> None:
    payload = values
    if sys.byteorder != "little":
        payload = array("H", values)
        payload.byteswap()
    _write_atomic(path, payload.tobytes(), output_root)


def _write_json(path: Path, value: Any, output_root: Path) -> None:
    text = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    _write_atomic(path, text.encode("utf-8"), output_root)


def _asset_registry_path(asset: unreal.Object) -> str:
    # 目录名可以包含点号，只能剥掉最终的 .ObjectName（以及可选子对象）后缀。
    object_path = str(asset.get_path_name()).split(":", 1)[0]
    package_path = object_path.rsplit(".", 1)[0]
    if not package_path.startswith("/"):
        raise ExportError(f"Asset has no registry package path: {asset}")
    return package_path


def _selected_texture_assets() -> list[unreal.Texture]:
    try:
        selected = unreal.EditorUtilityLibrary.get_selected_assets()
    except Exception:
        return []
    textures: list[unreal.Texture] = []
    seen: set[str] = set()
    for asset in selected:
        if not isinstance(asset, unreal.Texture):
            continue
        path = _asset_registry_path(asset)
        if path.casefold() in seen:
            continue
        seen.add(path.casefold())
        textures.append(asset)
    return textures


def _source_import_files(asset: unreal.Texture) -> list[Path]:
    import_data = _property(asset, "asset_import_data", default=None)
    if import_data is None:
        return []
    filenames: Iterable[str] = []
    extract = getattr(import_data, "extract_filenames", None)
    if callable(extract):
        try:
            filenames = extract()
        except Exception:
            filenames = []
    resolved: list[Path] = []
    for filename in filenames:
        candidate = Path(str(filename))
        if not candidate.is_absolute():
            try:
                candidate = Path(
                    unreal.Paths.convert_relative_path_to_full(str(candidate))
                )
            except Exception:
                pass
        if candidate.is_file() and _canonical(candidate) not in {
            _canonical(existing) for existing in resolved
        }:
            resolved.append(candidate)
    return resolved


def _copy_texture_sources(
    asset: unreal.Texture,
    source_directory: Path,
    output_root: Path,
) -> list[dict[str, Any]]:
    sources = _source_import_files(asset)
    records: list[dict[str, Any]] = []
    asset_key = hashlib.sha1(
        _asset_registry_path(asset).encode("utf-8")
    ).hexdigest()[:8]
    for index, source in enumerate(sources):
        extension = source.suffix or ".bin"
        destination = source_directory / (
            f"{_safe_texture_value(asset.get_name())}__{asset_key}"
            f"__source_{index}{extension}"
        )
        _assert_write_path(output_root, destination)
        shutil.copy2(source, destination)
        records.append(
            {
                "source_absolute": str(source.resolve()),
                "copy": _path_record(output_root, destination),
                "source_bytes": destination.stat().st_size,
                "preserves_original_bytes": True,
            }
        )
    return records


def _snapshot_files(root: Path) -> dict[str, tuple[int, int]]:
    snapshot: dict[str, tuple[int, int]] = {}
    for path in root.rglob("*"):
        if path.is_file():
            stat = path.stat()
            snapshot[str(path.resolve()).casefold()] = (
                stat.st_mtime_ns,
                stat.st_size,
            )
    return snapshot


def _export_texture_assets(
    asset_paths: Sequence[str],
    texture_directory: Path,
    output_root: Path,
) -> dict[str, Any]:
    requested: list[str] = []
    seen: set[str] = set()
    for raw_path in asset_paths:
        path = str(raw_path).strip()
        if not path:
            continue
        if not path.startswith("/"):
            raise ExportError(
                f"Texture asset path must be an asset registry path starting with '/': "
                f"{path}"
            )
        if path.casefold() in seen:
            continue
        seen.add(path.casefold())
        requested.append(path)

    _assert_write_path(output_root, texture_directory)
    texture_directory.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {
        "requested_assets": requested,
        "export_directory": _path_record(output_root, texture_directory, kind="directory"),
        "asset_tools_outputs": [],
        "source_copies": [],
        "notes": [
            "AssetTools.export_assets uses Unreal's registered exporter and may "
            "quantize HDR/16-bit source data.",
            "When UAssetImportData retains a source file, its exact bytes are "
            "copied under source_assets to preserve the original bit depth.",
        ],
    }
    if not requested:
        return result

    assets: list[unreal.Object] = []
    for path in requested:
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if asset is None:
            raise ExportError(f"Could not load texture asset: {path}")
        if not isinstance(asset, unreal.Texture):
            raise ExportError(
                f"Asset is not a Texture: {path} ({asset.get_class().get_name()})"
            )
        assets.append(asset)

    before = _snapshot_files(texture_directory)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    if asset_tools is None:
        raise ExportError("AssetToolsHelpers.get_asset_tools() returned None.")
    asset_tools.export_assets(list(requested), str(texture_directory))
    after = _snapshot_files(texture_directory)

    changed_files = [
        Path(path)
        for path, signature in after.items()
        if before.get(path) != signature
    ]
    source_directory = texture_directory / "source_assets"
    _assert_write_path(output_root, source_directory)
    source_directory.mkdir(parents=True, exist_ok=True)

    for asset in assets:
        asset_name = asset.get_name()
        prefix = f"{asset_name}.".casefold()
        exported = sorted(
            path
            for path in changed_files
            if path.name.casefold().startswith(prefix)
        )
        if not exported:
            raise ExportError(
                f"AssetTools reported no exported file for {_asset_registry_path(asset)}. "
                "The exporter may be unavailable or may have rejected the asset class."
            )
        result["asset_tools_outputs"].append(
            {
                "asset_registry_path": _asset_registry_path(asset),
                "asset_class": asset.get_class().get_name(),
                "files": [
                    _path_record(output_root, path) for path in exported
                ],
            }
        )
        result["source_copies"].append(
            {
                "asset_registry_path": _asset_registry_path(asset),
                "source_files": _copy_texture_sources(
                    asset,
                    source_directory,
                    output_root,
                ),
            }
        )
    return result


def _export_landscape(args: argparse.Namespace) -> int:
    output_root = _resolve_output_dir(args.output_dir)
    world = _get_editor_world()
    actor_subsystem = _get_actor_subsystem()
    actor = _select_landscape(actor_subsystem, args.landscape)

    components, component_size, subsection_size, num_subsections = (
        _collect_components(actor)
    )
    min_x, min_y, max_x, max_y = _component_grid_extents(components)
    derived_width = max_x - min_x + 1
    derived_height = max_y - min_y + 1
    override = _parse_resolution(args.resolution)
    width, height = override or (derived_width, derived_height)
    if (
        width > MAX_RENDER_TARGET_DIMENSION
        or height > MAX_RENDER_TARGET_DIMENSION
    ):
        raise ExportError(
            f"Derived resolution {width}x{height} exceeds the supported "
            f"{MAX_RENDER_TARGET_DIMENSION} render-target limit."
        )

    transform = actor.get_actor_transform()
    extents = _make_box2d(min_x, min_y, max_x, max_y)
    actor_bounds_origin, actor_bounds_extent = actor.get_actor_bounds(False)
    actor_bounds_origin_dict = _vector_dict(actor_bounds_origin)
    actor_bounds_extent_dict = _vector_dict(actor_bounds_extent)
    actor_bounds = {
        "origin": actor_bounds_origin_dict,
        "extent": actor_bounds_extent_dict,
        "min": {
            "x": actor_bounds_origin_dict["x"] - actor_bounds_extent_dict["x"],
            "y": actor_bounds_origin_dict["y"] - actor_bounds_extent_dict["y"],
            "z": actor_bounds_origin_dict["z"] - actor_bounds_extent_dict["z"],
        },
        "max": {
            "x": actor_bounds_origin_dict["x"] + actor_bounds_extent_dict["x"],
            "y": actor_bounds_origin_dict["y"] + actor_bounds_extent_dict["y"],
            "z": actor_bounds_origin_dict["z"] + actor_bounds_extent_dict["z"],
        },
    }

    actor_name = _safe_filename(actor.get_name(), "landscape")
    height_png = output_root / f"{actor_name}_heightmap.png"
    height_r16 = output_root / f"{actor_name}_heightmap.r16"
    layer_directory = output_root / "Layers"
    _assert_write_path(output_root, layer_directory)
    layer_directory.mkdir(parents=True, exist_ok=True)

    height_temporary = output_root / f".{actor_name}_heightmap_rg8.tmp.png"
    try:
        height_target = _create_rgba8_render_target(world, width, height)
        if not actor.render_heightmap(transform, extents, height_target):
            raise ExportError(
                "ALandscape.render_heightmap returned false. The RTF_RGBA8 render "
                "target or landscape render data was rejected by the engine."
            )
        _export_render_target_png(height_target, height_temporary, output_root)
        height_values = _decode_packed_height(height_temporary, width, height)
    finally:
        if height_temporary.exists():
            height_temporary.unlink()

    _write_gray_png(
        height_png,
        width,
        height,
        height_values,
        bit_depth=16,
        output_root=output_root,
    )
    _write_r16(height_r16, height_values, output_root)

    layer_names = list(actor.get_target_layer_names(False))
    layer_outputs: list[dict[str, Any]] = []
    for index, layer_name in enumerate(layer_names):
        display_name = str(layer_name)
        layer_temporary = (
            output_root
            / f".{actor_name}_layer_{index:03d}_rgba8.tmp.png"
        )
        try:
            target = _create_rgba8_render_target(world, width, height)
            if not actor.render_weightmap(transform, extents, layer_name, target):
                raise ExportError(
                    f"ALandscape.render_weightmap returned false for layer "
                    f"'{display_name}'."
                )
            _export_render_target_png(target, layer_temporary, output_root)
            gray_values = _decode_gray8(layer_temporary, width, height)
        finally:
            if layer_temporary.exists():
                layer_temporary.unlink()

        layer_path = layer_directory / (
            f"{index:03d}_{_safe_filename(display_name, 'layer')}.png"
        )
        _write_gray_png(
            layer_path,
            width,
            height,
            gray_values,
            bit_depth=8,
            output_root=output_root,
        )
        layer_outputs.append(
            {
                "index": index,
                "name": display_name,
                "file": _path_record(output_root, layer_path),
            }
        )

    texture_directory = output_root / "Textures"
    explicit_texture_paths = args.texture_assets
    if explicit_texture_paths is None:
        texture_paths = [
            _asset_registry_path(asset) for asset in _selected_texture_assets()
        ]
        texture_source = "editor_selection"
    else:
        texture_paths = list(explicit_texture_paths)
        texture_source = "command_line"
    texture_result = _export_texture_assets(
        texture_paths,
        texture_directory,
        output_root,
    )
    texture_result["requested_from"] = texture_source

    scale = _property(transform, "scale3d")
    scale_x = float(_property(scale, "x", default=1.0))
    scale_y = float(_property(scale, "y", default=1.0))
    scale_z = float(_property(scale, "z", default=1.0))
    local_quad_size_x = max_x - min_x
    local_quad_size_y = max_y - min_y
    normalized_height_scale = LANDSCAPE_MAX_VALUE * LANDSCAPE_ZSCALE * scale_z
    normalized_height_offset = -LANDSCAPE_MID_VALUE * LANDSCAPE_ZSCALE * scale_z

    manifest = {
        "exporter_version": EXPORTER_VERSION,
        "mode": "landscape",
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "output_root": {
            "absolute": str(output_root),
            "relative": ".",
        },
        "world": {
            "name": world.get_name(),
            "path": world.get_path_name(),
        },
        "landscape": {
            **_actor_record(actor),
            "transform": _transform_dict(transform),
            "actor_bounds": actor_bounds,
        },
        "component_count": len(components),
        "component_scope": (
            "Loaded ULandscapeComponent objects attached to the selected "
            "ALandscape actor."
        ),
        "component_bounds": components,
        "component_sizes": {
            "component_size_quads": component_size,
            "subsection_size_quads": subsection_size,
            "num_subsections": num_subsections,
        },
        "grid_extent": {
            "min_x": min_x,
            "min_y": min_y,
            "max_x": max_x,
            "max_y": max_y,
            "width_quads": local_quad_size_x,
            "height_quads": local_quad_size_y,
        },
        "resolution": {
            "width": width,
            "height": height,
            "derived_width": derived_width,
            "derived_height": derived_height,
            "overridden": override is not None,
        },
        "size_x": local_quad_size_x * scale_x,
        "size_y": local_quad_size_y * scale_y,
        "size_units": "cm",
        "size_note": (
            "size_x/size_y are local landscape grid spans multiplied by actor "
            "scale X/Y; actor rotation is recorded separately in transform."
        ),
        "height_scale": normalized_height_scale,
        "height_offset": normalized_height_offset,
        "height_scale_units": "cm per normalized [0,1] height",
        "height_mapping": {
            "raw_min": 0,
            "raw_max": 65535,
            "raw_mid": 32768,
            "local_cm_per_raw_unit": LANDSCAPE_ZSCALE,
            "world_cm_per_raw_unit": LANDSCAPE_ZSCALE * scale_z,
            "local_cm_at_raw_zero": -LANDSCAPE_MID_VALUE * LANDSCAPE_ZSCALE,
            "world_cm_at_raw_zero": normalized_height_offset,
            "normalized_world_height_scale": normalized_height_scale,
            "normalized_world_height_offset": normalized_height_offset,
            "actor_scale_z": scale_z,
            "formula": (
                "world_z_along_local_axis = actor_translation_z + "
                "(raw_u16 - 32768) * actor_scale_z / 128"
            ),
        },
        "heightmap": {
            "file": _path_record(output_root, height_png),
            "raw_r16_file": _path_record(output_root, height_r16),
            "png_format": "grayscale16",
            "effective_precision_bits": 16,
            "native_g16_render_target": False,
            "engine_render_target_format": "RTF_RGBA8",
            "engine_encoding": "COMPRESS_HEIGHT: high byte in R, low byte in G",
            "reconstruction": "u16 = (R << 8) | G",
            "r16_byte_order": "little-endian",
            "precision_note": (
                "UE 5.8 Python does not expose a PF_G16 render target or a direct "
                "PF_G16 ImageWrite export. The engine's lossless RG8 height packing "
                "is used and reconstructed to a real 16-bit grayscale image; this is "
                "not a native G16 render-target file."
            ),
        },
        "target_layer_names": [str(name) for name in layer_names],
        "layers": layer_outputs,
        "texture_asset_export_dir": _path_record(
            output_root,
            texture_directory,
            kind="directory",
        ),
        "texture_assets": texture_result,
        "engine_apis": [
            "unreal.UnrealEditorSubsystem.get_editor_world",
            "unreal.EditorActorSubsystem.get_selected_level_actors",
            "unreal.EditorActorSubsystem.get_all_level_actors",
            "ALandscape.get_target_layer_names",
            "ALandscape.render_heightmap",
            "ALandscape.render_weightmap",
            "unreal.KismetRenderingLibrary.create_render_target2d",
            "unreal.ImageWriteBlueprintLibrary.export_to_disk",
            "unreal.AssetToolsHelpers.get_asset_tools().export_assets",
            "unreal.EditorUtilityLibrary.get_selected_assets",
        ],
    }

    manifest_path = output_root / "manifest.json"
    _write_json(manifest_path, manifest, output_root)
    _log_info(
        f"Terrain export complete: {output_root} "
        f"({width}x{height}, {len(layer_outputs)} layers)"
    )
    return 0


def _export_textures(args: argparse.Namespace) -> int:
    output_root = _resolve_output_dir(args.output_dir)
    result = _export_texture_assets(
        list(args.assets),
        output_root,
        output_root,
    )
    manifest = {
        "exporter_version": EXPORTER_VERSION,
        "mode": "textures",
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "output_root": {
            "absolute": str(output_root),
            "relative": ".",
        },
        "texture_asset_export_dir": _path_record(
            output_root,
            output_root,
            kind="directory",
        ),
        **result,
    }
    _write_json(output_root / "textures_manifest.json", manifest, output_root)
    _log_info(
        f"Texture asset export complete: {output_root} "
        f"({len(result['requested_assets'])} assets)"
    )
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Export UE 5.8 landscape height/layers and selected texture assets."
    )
    subparsers = parser.add_subparsers(dest="mode", required=True)

    landscape = subparsers.add_parser(
        "landscape",
        help="Export one ALandscape heightmap, all paint layers, and optional textures.",
    )
    landscape.add_argument(
        "--output-dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help="All writes remain below this directory.",
    )
    landscape.add_argument(
        "--landscape",
        default=None,
        help="Optional landscape actor name, actor label, or full object path.",
    )
    landscape.add_argument(
        "--resolution",
        default=None,
        help="Optional WIDTHxHEIGHT override; defaults to the landscape vertex extent.",
    )
    landscape.add_argument(
        "--texture-assets",
        nargs="*",
        default=None,
        help=(
            "Optional asset registry paths. If omitted, selected Texture assets "
            "from the Content Browser are exported."
        ),
    )

    textures = subparsers.add_parser(
        "textures",
        help="Export texture assets without touching landscape data.",
    )
    textures.add_argument(
        "--output-dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help="All writes remain below this directory.",
    )
    textures.add_argument(
        "--assets",
        nargs="+",
        required=True,
        help="Asset registry paths, for example /Game/KiteDemo/Textures/T_Rock_D.",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    try:
        args = parser.parse_args(argv)
        if args.mode == "landscape":
            return _export_landscape(args)
        if args.mode == "textures":
            return _export_textures(args)
        raise ExportError(f"Unknown mode: {args.mode}")
    except SystemExit:
        raise
    except Exception as exc:
        _log_error(f"Terrain export failed: {exc}")
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

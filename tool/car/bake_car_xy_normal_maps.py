import argparse
import math
import os
from pathlib import Path


TGA_HEADER_SIZE = 18
TGA_TRUE_COLOR_RLE = 10


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Bake the Z channel used by Blender's GetNormalMap node into "
            "32-bit RLE TGA XY normal maps."
        )
    )
    parser.add_argument(
        "normal_maps",
        nargs="*",
        type=Path,
    )
    parser.add_argument(
        "--source-target",
        action="append",
        nargs=2,
        type=Path,
        metavar=("SOURCE", "TARGET"),
        help=(
            "Restore the authored R/G/A channels from SOURCE, "
            "then bake Z into TARGET."
        ),
    )
    arguments = parser.parse_args()
    if (
        not arguments.normal_maps
        and not arguments.source_target
    ):
        parser.error(
            "Provide normal_maps or at least one "
            "--source-target pair."
        )
    return arguments


def calculate_color_map_size(data):
    color_map_type = data[1]
    if color_map_type == 0:
        return 0

    color_map_length = int.from_bytes(
        data[5:7],
        byteorder="little",
    )
    color_map_entry_bits = data[7]
    color_map_entry_bytes = (
        color_map_entry_bits + 7
    ) // 8
    return color_map_length * color_map_entry_bytes


def reconstruct_blue(red, green):
    normal_x = red / 255.0 * 2.0 - 1.0
    normal_y = green / 255.0 * 2.0 - 1.0
    normal_z = math.sqrt(
        max(
            0.0,
            1.0
            - normal_x * normal_x
            - normal_y * normal_y,
        )
    )

    # Blender 的 GetNormalMap 节点把 sqrt 结果直接接到 Normal Map.Color.B，
    # 因此这里按同样的 0..1 数值写入，而不是再做一次 0.5 偏移编码。
    return min(
        255,
        max(
            0,
            int(normal_z * 255.0 + 0.5),
        ),
    )


def build_baked_normal_map(data, source_path):
    data = bytearray(data)
    if len(data) < TGA_HEADER_SIZE:
        raise RuntimeError(
            f"TGA header is truncated: {source_path}"
        )

    image_type = data[2]
    width = int.from_bytes(
        data[12:14],
        byteorder="little",
    )
    height = int.from_bytes(
        data[14:16],
        byteorder="little",
    )
    bits_per_pixel = data[16]
    if image_type != TGA_TRUE_COLOR_RLE:
        raise RuntimeError(
            f"Expected 32-bit RLE TGA image type 10: {source_path}"
        )
    if bits_per_pixel != 32:
        raise RuntimeError(
            f"Expected 32-bit TGA pixels: {source_path}"
        )

    pixel_count = width * height
    data_offset = (
        TGA_HEADER_SIZE
        + data[0]
        + calculate_color_map_size(data)
    )
    cursor = data_offset
    decoded_pixel_count = 0
    stored_pixel_count = 0
    changed_stored_pixel_count = 0

    while decoded_pixel_count < pixel_count:
        if cursor >= len(data):
            raise RuntimeError(
                f"TGA RLE stream is truncated: {source_path}"
            )

        packet_header = data[cursor]
        cursor += 1
        packet_pixel_count = (
            packet_header & 0x7F
        ) + 1
        stored_pixels = (
            1
            if packet_header & 0x80
            else packet_pixel_count
        )

        for _ in range(stored_pixels):
            if cursor + 4 > len(data):
                raise RuntimeError(
                    f"TGA pixel payload is truncated: {source_path}"
                )

            blue_offset = cursor
            green = data[cursor + 1]
            red = data[cursor + 2]
            reconstructed_blue = reconstruct_blue(
                red,
                green,
            )
            if data[blue_offset] != reconstructed_blue:
                data[blue_offset] = reconstructed_blue
                changed_stored_pixel_count += 1

            cursor += 4
            stored_pixel_count += 1

        decoded_pixel_count += packet_pixel_count

    if decoded_pixel_count != pixel_count:
        raise RuntimeError(
            f"TGA RLE stream exceeds image dimensions: {source_path}"
        )

    return bytes(data), {
        "width": width,
        "height": height,
        "storedPixelCount": stored_pixel_count,
        "changedStoredPixelCount":
            changed_stored_pixel_count,
        "fileLength": len(data),
    }


def atomic_write_bytes(path, data):
    temporary_path = path.with_suffix(path.suffix + ".tmp")
    temporary_path.write_bytes(data)
    os.replace(temporary_path, path)


def bake_normal_map(path):
    baked_data, result = build_baked_normal_map(
        path.read_bytes(),
        path,
    )
    atomic_write_bytes(path, baked_data)
    result["path"] = str(path)
    return result


def bake_normal_map_from_source(source_path, target_path):
    source_data = source_path.read_bytes()
    target_length = (
        target_path.stat().st_size
        if target_path.exists()
        else len(source_data)
    )
    if len(source_data) != target_length:
        raise RuntimeError(
            "Source and target TGA lengths differ: "
            f"{source_path} -> {target_path}"
        )

    # 直接从 Blender 源贴图恢复 R/G/A，确保不会残留历史上的 Y 反相；
    # 随后的 bake_normal_map 只负责按 GetNormalMap 公式补写 B/Z。
    baked_data, result = build_baked_normal_map(
        source_data,
        source_path,
    )
    atomic_write_bytes(target_path, baked_data)
    result["path"] = str(target_path)
    result["sourcePath"] = str(source_path)
    return result


def main():
    arguments = parse_arguments()
    for source_path, target_path in (
        arguments.source_target or []
    ):
        result = bake_normal_map_from_source(
            source_path.resolve(),
            target_path.resolve(),
        )
        print(result)

    for normal_map in arguments.normal_maps:
        result = bake_normal_map(
            normal_map.resolve()
        )
        print(result)


if __name__ == "__main__":
    main()

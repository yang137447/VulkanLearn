import argparse
import os
import struct
from pathlib import Path


TGA_HEADER_SIZE = 18
TGA_RLE_TRUE_COLOR = 10
TGA_BITS_PER_PIXEL_OFFSET = 16


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Set the packed wheel-mask blue channel to neutral AO "
            "without changing roughness, metallic, or alpha."
        )
    )
    parser.add_argument("images", nargs="+", type=Path)
    return parser.parse_args()


def calculate_color_map_size(data):
    color_map_type = data[1]
    if color_map_type == 0:
        return 0
    if color_map_type != 1:
        raise RuntimeError(
            f"Unsupported TGA color-map type {color_map_type}"
        )

    color_map_length = struct.unpack_from("<H", data, 5)[0]
    color_map_entry_bits = data[7]
    color_map_entry_bytes = (
        color_map_entry_bits + 7
    ) // 8
    return color_map_length * color_map_entry_bytes


def neutralize_blue_channel(path):
    data = bytearray(path.read_bytes())
    if len(data) < TGA_HEADER_SIZE:
        raise RuntimeError("TGA header is truncated")

    image_type = data[2]
    bits_per_pixel = data[TGA_BITS_PER_PIXEL_OFFSET]
    if image_type != TGA_RLE_TRUE_COLOR:
        raise RuntimeError(
            f"Expected RLE true-color TGA, got image type {image_type}"
        )
    if bits_per_pixel != 32:
        raise RuntimeError(
            f"Expected 32-bit TGA, got {bits_per_pixel}-bit"
        )

    width, height = struct.unpack_from("<HH", data, 12)
    pixel_count = width * height
    pixel_size = bits_per_pixel // 8
    cursor = (
        TGA_HEADER_SIZE
        + data[0]
        + calculate_color_map_size(data)
    )
    if cursor > len(data):
        raise RuntimeError("TGA metadata extends beyond the file")

    decoded_pixel_count = 0
    changed_pixel_count = 0
    while decoded_pixel_count < pixel_count:
        if cursor >= len(data):
            raise RuntimeError("TGA RLE stream ended before all pixels")
        packet_header = data[cursor]
        cursor += 1
        packet_count = (packet_header & 0x7F) + 1
        is_run_length_packet = bool(packet_header & 0x80)
        stored_pixel_count = (
            1
            if is_run_length_packet
            else packet_count
        )

        for _ in range(stored_pixel_count):
            if cursor + pixel_size > len(data):
                raise RuntimeError("TGA RLE packet is truncated")

            # TGA true-color pixels are BGRA. Updating the stored value in
            # place preserves Image ID, footer metadata, and RLE packet layout.
            if data[cursor] != 255:
                data[cursor] = 255
                changed_pixel_count += (
                    packet_count
                    if is_run_length_packet
                    else 1
                )
            cursor += pixel_size

        decoded_pixel_count += packet_count

    if decoded_pixel_count != pixel_count:
        raise RuntimeError("TGA pixel count does not match its dimensions")

    temporary_path = path.with_suffix(path.suffix + ".tmp")
    temporary_path.write_bytes(data)
    os.replace(temporary_path, path)

    return {
        "path": str(path),
        "width": width,
        "height": height,
        "changedPixelCount": changed_pixel_count,
        "fileLength": len(data),
    }


def main():
    arguments = parse_arguments()
    for image_path in arguments.images:
        print(neutralize_blue_channel(image_path.resolve()))


if __name__ == "__main__":
    main()

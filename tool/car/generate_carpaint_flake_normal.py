import argparse
import math
import struct
from pathlib import Path


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Generate a deterministic tileable Voronoi flake normal map."
        )
    )
    parser.add_argument("output", type=Path)
    parser.add_argument("--size", type=int, default=64)
    parser.add_argument("--cells", type=int, default=64)
    parser.add_argument("--seed", type=int, default=20260816)
    parser.add_argument("--maximum-slope", type=float, default=0.95)
    parser.add_argument("--edge-blend", type=float, default=0.18)
    return parser.parse_args()


def hash_u32(value):
    value &= 0xFFFFFFFF
    value ^= value >> 16
    value = (value * 0x7FEB352D) & 0xFFFFFFFF
    value ^= value >> 15
    value = (value * 0x846CA68B) & 0xFFFFFFFF
    value ^= value >> 16
    return value


def random_value(cell_x, cell_y, channel, cells, seed):
    wrapped_x = cell_x % cells
    wrapped_y = cell_y % cells
    value = (
        wrapped_x * 0x1F123BB5
        ^ wrapped_y * 0x5F356495
        ^ channel * 0x6C8E9CF5
        ^ seed
    )
    return hash_u32(value) / 0xFFFFFFFF


def feature_point(cell_x, cell_y, cells, seed):
    return (
        cell_x + random_value(cell_x, cell_y, 0, cells, seed),
        cell_y + random_value(cell_x, cell_y, 1, cells, seed),
    )


def feature_normal(cell_x, cell_y, cells, seed, maximum_slope):
    angle = (
        random_value(cell_x, cell_y, 2, cells, seed)
        * math.tau
    )
    radius = (
        math.sqrt(random_value(cell_x, cell_y, 3, cells, seed))
        * maximum_slope
    )
    normal_x = math.cos(angle) * radius
    normal_y = math.sin(angle) * radius
    normal_z = math.sqrt(1.0 - radius * radius)
    return normal_x, normal_y, normal_z


def smoothstep(value):
    return value * value * (3.0 - 2.0 * value)


def sample_normal(x, y, cells, seed, maximum_slope, edge_blend):
    base_x = math.floor(x)
    base_y = math.floor(y)
    nearest = None
    second = None

    for offset_y in (-1, 0, 1):
        for offset_x in (-1, 0, 1):
            cell_x = base_x + offset_x
            cell_y = base_y + offset_y
            point_x, point_y = feature_point(
                cell_x,
                cell_y,
                cells,
                seed,
            )
            distance = abs(point_x - x) + abs(point_y - y)
            candidate = (distance, cell_x, cell_y)
            if nearest is None or candidate < nearest:
                second = nearest
                nearest = candidate
            elif second is None or candidate < second:
                second = candidate

    nearest_normal = feature_normal(
        nearest[1],
        nearest[2],
        cells,
        seed,
        maximum_slope,
    )
    second_normal = feature_normal(
        second[1],
        second[2],
        cells,
        seed,
        maximum_slope,
    )
    distance_gap = second[0] - nearest[0]
    nearest_weight = 1.0
    if distance_gap < edge_blend:
        nearest_weight = 0.5 + 0.5 * smoothstep(
            distance_gap / edge_blend
        )

    blended = tuple(
        second_normal[index]
        + (nearest_normal[index] - second_normal[index])
        * nearest_weight
        for index in range(3)
    )
    length = math.sqrt(sum(component * component for component in blended))
    return tuple(component / length for component in blended)


def encode_channel(value):
    return min(255, max(0, round((value * 0.5 + 0.5) * 255.0)))


def build_tga(size, cells, seed, maximum_slope, edge_blend):
    header = bytearray(18)
    header[2] = 2
    header[12:14] = struct.pack("<H", size)
    header[14:16] = struct.pack("<H", size)
    header[16] = 32
    header[17] = 0x28

    pixels = bytearray(size * size * 4)
    cursor = 0
    for pixel_y in range(size):
        sample_y = (pixel_y + 0.5) * cells / size
        for pixel_x in range(size):
            sample_x = (pixel_x + 0.5) * cells / size
            normal = sample_normal(
                sample_x,
                sample_y,
                cells,
                seed,
                maximum_slope,
                edge_blend,
            )
            red = encode_channel(normal[0])
            green = encode_channel(normal[1])
            blue = encode_channel(normal[2])
            pixels[cursor : cursor + 4] = bytes(
                (blue, green, red, 255)
            )
            cursor += 4

    return bytes(header + pixels)


def validate_periodicity(cells, seed, maximum_slope, edge_blend):
    sample_points = ((0.125, 0.375), (7.75, 11.25), (31.5, 47.875))
    for sample_x, sample_y in sample_points:
        reference = sample_normal(
            sample_x,
            sample_y,
            cells,
            seed,
            maximum_slope,
            edge_blend,
        )
        shifted = sample_normal(
            sample_x + cells,
            sample_y + cells,
            cells,
            seed,
            maximum_slope,
            edge_blend,
        )
        maximum_difference = max(
            abs(reference[index] - shifted[index])
            for index in range(3)
        )
        if maximum_difference > 1.0e-9:
            raise RuntimeError("generated normal field is not periodic")


def main():
    arguments = parse_arguments()
    if arguments.size <= 0 or arguments.size > 65535:
        raise ValueError("size must be in [1, 65535]")
    if arguments.cells <= 1 or arguments.cells > arguments.size:
        raise ValueError("cells must be in [2, size]")
    if not 0.0 < arguments.maximum_slope < 1.0:
        raise ValueError("maximum-slope must be in (0, 1)")
    if arguments.edge_blend <= 0.0:
        raise ValueError("edge-blend must be greater than zero")

    validate_periodicity(
        arguments.cells,
        arguments.seed,
        arguments.maximum_slope,
        arguments.edge_blend,
    )
    output_path = arguments.output.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(
        build_tga(
            arguments.size,
            arguments.cells,
            arguments.seed,
            arguments.maximum_slope,
            arguments.edge_blend,
        )
    )
    print(
        "CARPAINT_FLAKE_NORMAL_GENERATED "
        f"size={arguments.size} cells={arguments.cells} "
        f"output={output_path}"
    )


if __name__ == "__main__":
    main()

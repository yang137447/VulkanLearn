"""Independent double-precision height oracle; no material or production helpers."""

from array import array
import math


def load_r16(path):
    values = array("H")
    values.frombytes(path.read_bytes())
    return values


def interpolate(values, weights):
    lower = values[0] * (1 - weights[0]) + values[1] * weights[0]
    upper = values[2] * (1 - weights[0]) + values[3] * weights[0]
    return lower * (1 - weights[1]) + upper * weights[1]


def bilinear(plane, width, height, uv, repeat=True):
    addressed = [value - math.floor(value) if repeat else value for value in uv]
    position = [addressed[0] * width - 0.5, addressed[1] * height - 0.5]
    base = [math.floor(value) for value in position]
    weights = [position[axis] - base[axis] for axis in range(2)]
    neighbors = []
    values = []
    for row, column in ((0, 0), (0, 1), (1, 0), (1, 1)):
        column_index, row_index = base[0] + column, base[1] + row
        if repeat:
            column_index %= width
            row_index %= height
        else:
            column_index = min(width - 1, max(0, column_index))
            row_index = min(height - 1, max(0, row_index))
        neighbors.append([column_index, row_index])
        values.append(plane[row_index * width + column_index])
    return {
        "position": position, "neighbors": neighbors, "neighborR16": values,
        "axisWeights": weights,
        "fourWeights": [(1 - weights[0]) * (1 - weights[1]), weights[0] * (1 - weights[1]),
                        (1 - weights[0]) * weights[1], weights[0] * weights[1]],
        "exactR16": interpolate(values, weights),
    }


def verify_storage(source, atlas, atlas_width):
    errors = 0
    columns = atlas_width // 136
    for slice_index in range(len(source) // (256 * 256)):
        for page_y in range(2):
            for page_x in range(2):
                slot = slice_index * 4 + page_y * 2 + page_x
                for row in range(136):
                    for column in range(136):
                        source_index = slice_index * 256 * 256 + (
                            (page_y * 128 + row - 4) % 256) * 256 + (page_x * 128 + column - 4) % 256
                        destination = (slot // columns * 136 + row) * atlas_width + slot % columns * 136 + column
                        errors += source[source_index] != atlas[destination]
    return errors


def validate_oracle_anchors():
    plane = [0, 65535, 65535, 0]
    anchors = [((0.5, 0.5), 32767.5), ((-0.25, 0.25), 65535),
               ((0.375, 0.375), 24575.625), ((0.0, 0.0), 32767.5)]
    for uv, expected in anchors:
        if bilinear(plane, 2, 2, uv)["exactR16"] != expected:
            raise AssertionError(f"Independent bilinear anchor failed: {uv}")
    return {"known2x2CenterRepeatFractionalAnchors": len(anchors), "passed": True}

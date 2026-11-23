"""Check real heightmap vertices against the fixture camera without a renderer.

Uses the sceneNode.cpp horizontal-FOV/look_at projection convention. Reports
geometric frustum coverage only, not GPU pixels, visibility through occluders,
Cook success, material correctness, or editor panel layout.
"""

import argparse
import binascii
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def png16(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"Expected PNG: {path}")
    offset = 8
    compressed = bytearray()
    width = height = 0
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        expected_crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        if binascii.crc32(kind + body) & 0xFFFFFFFF != expected_crc:
            raise ValueError(f"Invalid PNG CRC: {path}")
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", body)
            if (depth, color, compression, filtering, interlace) != (16, 0, 0, 0, 0):
                raise ValueError("Harness supports non-interlaced 16-bit grayscale PNG only")
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            break
        offset += length + 12
    raw = zlib.decompress(compressed)
    stride = width * 2
    if width < 2 or height < 2 or len(raw) != (stride + 1) * height:
        raise ValueError("Invalid PNG scanline size")
    previous = bytearray(stride)
    rows = []
    for row_index in range(height):
        begin = row_index * (stride + 1)
        method = raw[begin]
        row = bytearray(raw[begin + 1:begin + 1 + stride])
        if method not in range(5):
            raise ValueError("Unsupported PNG filter")
        for column in range(stride):
            left = row[column - 2] if column >= 2 else 0
            above = previous[column]
            corner = previous[column - 2] if column >= 2 else 0
            if method == 0:
                predictor = 0
            elif method == 1:
                predictor = left
            elif method == 2:
                predictor = above
            elif method == 3:
                predictor = (left + above) // 2
            else:
                prediction = left + above - corner
                distances = (abs(prediction - left), abs(prediction - above), abs(prediction - corner))
                predictor = (left, above, corner)[distances.index(min(distances))]
            row[column] = (row[column] + predictor) & 255
        rows.append(struct.unpack(f">{width}H", row))
        previous = row
    return rows


def subtract(first, second):
    return [left - right for left, right in zip(first, second)]


def dot(first, second):
    return sum(left * right for left, right in zip(first, second))


def cross(first, second):
    return [
        first[1] * second[2] - first[2] * second[1],
        first[2] * second[0] - first[0] * second[2],
        first[0] * second[1] - first[1] * second[0],
    ]


def normalized(vector):
    length = math.sqrt(dot(vector, vector))
    if length <= 0:
        raise ValueError("Degenerate camera basis")
    return [component / length for component in vector]


def check(resource_root, scene_name="Generated/Validation/TerrainCompositedVT/SC_composited_vt_2x2.json"):
    scene_path = (resource_root / scene_name).resolve()
    if not scene_path.is_relative_to(resource_root.resolve()):
        raise ValueError("Scene must stay within the topic resource root")
    scene = read_json(scene_path)
    camera = next(item for item in scene["objects"] if item["type"] == "camera")
    position = camera["position"]
    forward = normalized(subtract(camera["look_at"], position))
    right = normalized(cross(forward, [0, 1, 0]))
    up = cross(right, forward)
    tangent = math.tan(math.radians(camera["fov"] / 2))
    aspects = {"4:3": 4 / 3, "16:9": 16 / 9, "21:9": 21 / 9}
    records = []
    for placement in scene["objects"]:
        if placement["type"] != "terrain":
            continue
        if placement["rotation"] != [0.0, 0.0, 0.0] or placement["scale"] != [1.0, 1.0, 1.0]:
            raise ValueError("Harness requires unrotated unit terrain placement")
        terrain = read_json(resource_root / placement["terrainPath"])
        height_path = resource_root / terrain["heightmapPath"]
        rows = png16(height_path)
        grid = terrain["gridResolution"]
        margin = terrain["heightmapMargin"]
        if len(rows) != grid + margin * 2 + 1 or len(rows[0]) != len(rows):
            raise ValueError("Heightmap dimensions differ from runtime margin contract")
        count = clipped = 0
        min_depth, max_depth = math.inf, -math.inf
        min_height, max_height = math.inf, -math.inf
        min_x = min_y = math.inf
        max_x = max_y = -math.inf
        for row_index in range(margin, margin + grid + 1):
            for column in range(margin, margin + grid + 1):
                height = rows[row_index][column] / 65535 * terrain["heightScale"] + terrain["heightOffset"]
                point = [
                    placement["position"][0] + ((column - margin) / grid - 0.5) * terrain["size"][0],
                    placement["position"][1] + height,
                    placement["position"][2] + ((row_index - margin) / grid - 0.5) * terrain["size"][1],
                ]
                relative = subtract(point, position)
                depth = dot(relative, forward)
                if depth <= 0:
                    raise ValueError("Terrain vertex behind camera")
                horizontal = dot(relative, right) / (depth * tangent)
                vertical = -dot(relative, up) / (depth * tangent)
                min_x, max_x = min(min_x, horizontal), max(max_x, horizontal)
                min_y, max_y = min(min_y, vertical), max(max_y, vertical)
                min_height, max_height = min(min_height, height), max(max_height, height)
                min_depth, max_depth = min(min_depth, depth), max(max_depth, depth)
                if not camera["near_clip"] <= depth <= camera["far_clip"]:
                    clipped += 1
                count += 1
        projections = {}
        for label, aspect in aspects.items():
            rectangle = [min_x, min_y * aspect, max_x, max_y * aspect]
            visible = clipped == 0 and all(abs(component) < 1 for component in rectangle)
            if not visible:
                raise ValueError(f"{placement['name']} not fully inside {label} frustum: {rectangle}")
            projections[label] = {
                "ndcBounds": [round(component, 6) for component in rectangle],
                "allVerticesInside": True,
            }
        records.append({
            "placement": placement["name"],
            "heightmap": terrain["heightmapPath"],
            "heightmapSha256": hashlib.sha256(height_path.read_bytes()).hexdigest(),
            "verticesChecked": count,
            "heightRangeCm": [round(min_height, 6), round(max_height, 6)],
            "cameraDepthRangeCm": [round(min_depth, 6), round(max_depth, 6)],
            "clipFailures": clipped,
            "projections": projections,
        })
    if len(records) not in (1, 4, 16):
        raise ValueError("Expected the single 100m fixture, real 2x2 or full 4x4 terrain placements")
    return {
        "geometryFrustumOnly": True,
        "rendererStarted": False,
        "sceneSha256": hashlib.sha256(scene_path.read_bytes()).hexdigest(),
        "camera": camera,
        "tiles": records,
        "totalVerticesChecked": sum(record["verticesChecked"] for record in records),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", default="Generated/Validation/TerrainCompositedVT/SC_composited_vt_2x2.json")
    arguments = parser.parse_args()
    resource_root = Path(__file__).resolve().parents[3].parent / "VukanLearnResources"
    print(json.dumps(check(resource_root, arguments.scene), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

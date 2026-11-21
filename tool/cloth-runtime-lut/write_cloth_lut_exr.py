#!/usr/bin/env python3
"""Write the Ashikhmin cloth runtime LUT atlas as an uncompressed EXR.

Atlas layout (256x256, RGBA half):
  (0,0)    64x64  Velvet 1D table replicated on every row
  (64,0)   64x64  Satin 2D table (x=phi, y=mu)
  (0,64)   64x64  Slanted Velvet 2D table (x=phi, y=mu)
  (192,0)  3x1    (eta, c0, c1) constants for Velvet/Satin/Slanted

RGB channels are g, R0, R1.  Alpha is 1 in table texels.  The material
texture asset sets flipY=false so the shader can address rows directly.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_cloth_ashikhmin_lut import (  # noqa: E402
    PI,
    build_satin,
    build_slanted_velvet,
    build_velvet,
)

SIZE = 256
TILE = 64


def pack_name(value: str) -> bytes:
    return value.encode("ascii") + b"\0"


def attribute(name: str, kind: str, payload: bytes) -> bytes:
    return pack_name(name) + pack_name(kind) + struct.pack("<i", len(payload)) + payload


def write_exr(path: Path, pixels: list[list[tuple[float, float, float, float]]]) -> None:
    height = len(pixels)
    width = len(pixels[0])
    header = bytearray()
    header += struct.pack("<II", 20000630, 2)

    channels = bytearray()
    for channel in ("A", "B", "G", "R"):
        channels += pack_name(channel)
        channels += struct.pack("<i", 1)  # HALF
        channels += b"\0"                 # pLinear
        channels += b"\0\0\0"             # reserved
        channels += struct.pack("<ii", 1, 1)
    channels += b"\0"
    header += attribute("channels", "chlist", bytes(channels))
    header += attribute("compression", "compression", b"\0")
    header += attribute(
        "dataWindow",
        "box2i",
        struct.pack("<iiii", 0, 0, width - 1, height - 1),
    )
    header += attribute(
        "displayWindow",
        "box2i",
        struct.pack("<iiii", 0, 0, width - 1, height - 1),
    )
    header += attribute("lineOrder", "lineOrder", b"\0")
    header += attribute("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    header += attribute(
        "screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0)
    )
    header += attribute("screenWindowWidth", "float", struct.pack("<f", 1.0))
    header += b"\0"

    row_bytes = width * 4 * 2
    block_size = 8 + row_bytes
    data_start = len(header) + height * 8
    offsets = [data_start + y * block_size for y in range(height)]

    with path.open("wb") as stream:
        stream.write(header)
        stream.write(struct.pack(f"<{height}Q", *offsets))
        for file_y in range(height):
            # Engine material textures flip decoded rows once.  Store the
            # atlas upside down so flipY=true restores row 0 at v=0.
            row = pixels[height - 1 - file_y]
            stream.write(struct.pack("<ii", file_y, row_bytes))
            for channel_index in (3, 2, 1, 0):  # A, B, G, R
                values = [pixel[channel_index] for pixel in row]
                stream.write(struct.pack(f"<{width}e", *values))


def make_atlas() -> list[list[tuple[float, float, float, float]]]:
    satin = build_satin(64, 64)
    velvet = build_velvet(64)
    slanted = build_slanted_velvet(64, 64)

    pixels = [
        [(1.0, 0.0, 0.0, 1.0) for _ in range(SIZE)]
        for _ in range(SIZE)
    ]

    for y in range(TILE):
        for x in range(TILE):
            pixels[y][x] = (
                velvet["g"][x],
                velvet["r0"][x],
                velvet["r1"][x],
                1.0,
            )

    for y in range(TILE):
        for x in range(TILE):
            index = y * TILE + x
            pixels[y][TILE + x] = (
                satin["g"][index],
                satin["r0"][index],
                satin["r1"][index],
                1.0,
            )
            pixels[TILE + y][x] = (
                slanted["g"][index],
                slanted["r0"][index],
                slanted["r1"][index],
                1.0,
            )

    constants = [
        (velvet["normal_mean"], velvet["c0"], velvet["c1"], 1.0),
        (satin["normal_mean"], satin["c0"], satin["c1"], 1.0),
        (slanted["normal_mean"], slanted["c0"], slanted["c1"], 1.0),
    ]
    for index, value in enumerate(constants):
        pixels[0][192 + index] = value

    return pixels


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(
            "../VukanLearnResources/Common/Source/Textures/"
            "T_clothAshikhminLut.exr"
        ),
    )
    args = parser.parse_args()
    pixels = make_atlas()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_exr(args.output, pixels)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()

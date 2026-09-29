"""Validate one exported VL offline snapshot."""

import argparse
import json
import math
import struct
from pathlib import Path


def read_pfm(path: Path):
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError(f"{path}: expected RGB PFM")
        width, height = map(int, stream.readline().split())
        if stream.readline().strip() != b"-1.0":
            raise ValueError(f"{path}: expected little-endian unit-scale PFM")
        payload = stream.read()
    expected = width * height * 3
    if len(payload) != expected * 4:
        raise ValueError(f"{path}: incomplete pixel payload")
    return width, height, struct.unpack(f"<{expected}f", payload)


def verify(directory: Path):
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    if manifest["type"] != "offlineRenderResult" or manifest["version"] != 1:
        raise ValueError("not a VL offline result manifest")
    width, height, combined = read_pfm(directory / "combined.pfm")
    direct = read_pfm(directory / "direct.pfm")
    indirect = read_pfm(directory / "indirect.pfm")
    if (width, height) != (
        manifest["integrator"]["width"], manifest["integrator"]["height"]
    ) or direct[:2] != (width, height) or indirect[:2] != (width, height):
        raise ValueError("AOV dimensions disagree with manifest")
    if not (0 < manifest["samples"] <= manifest["targetSamples"]):
        raise ValueError("invalid published sample count")
    if manifest["complete"] != (manifest["samples"] == manifest["targetSamples"]):
        raise ValueError("completion flag disagrees with sample count")
    if not manifest["inputDigest"] or not manifest["scene"]:
        raise ValueError("missing frozen input identity")
    if not all(math.isfinite(value) for value in combined):
        raise ValueError("non-finite combined pixel")
    if any(abs(c - (d + i)) > 2e-6 * max(1, abs(c))
           for c, d, i in zip(combined, direct[2], indirect[2])):
        raise ValueError("combined differs from direct + indirect")
    if max(combined) <= 0:
        raise ValueError("blank linear result")
    with (directory / "combined.ppm").open("rb") as stream:
        header = (stream.readline(), stream.readline(), stream.readline())
        if header != (b"P6\n", f"{width} {height}\n".encode(), b"255\n"):
            raise ValueError("preview dimensions disagree with linear film")
        if len(stream.read()) != width * height * 3:
            raise ValueError("incomplete preview")
    print(
        f"VL offline export valid: {directory} "
        f"({width}x{height}, {manifest['samples']} spp, "
        f"max={max(combined):.6g})"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    verify(parser.parse_args().directory)

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Convert a DirectX-style tangent-space normal map (TGA) to OpenGL-style.

UE bakes normal maps with the DirectX green-channel convention (+Y down);
VulkanLearn renders with the OpenGL convention (+Y up). The standard
DX -> GL conversion inverts only the green channel: G = 255 - G.

The conversion is baked into the asset (the TGA), never into the shader.

Only handles 24-bit uncompressed truecolor TGA (type 2), which is what the
6125 hair pack's exported textures are. Refuses anything else rather than
guessing.
"""
import struct
import sys


def flip_green_tga(path):
    data = bytearray(open(path, 'rb').read())
    if len(data) < 18:
        raise SystemExit(f"too short to be a TGA: {path}")

    id_len = data[0]
    cm_type = data[1]
    img_type = data[2]
    # color map spec (5 bytes) then dimensions
    width = struct.unpack_from('<H', data, 12)[0]
    height = struct.unpack_from('<H', data, 14)[0]
    depth = data[16]
    descriptor = data[17]

    if img_type != 2:
        raise SystemExit(f"unsupported image type {img_type} (expected 2 = uncompressed truecolor): {path}")
    if depth != 24:
        raise SystemExit(f"unsupported depth {depth} (expected 24-bit BGR): {path}")
    if cm_type != 0:
        raise SystemExit(f"unexpected color map type {cm_type} (expected 0): {path}")

    pixel_offset = 18 + id_len
    if cm_type != 0:
        raise SystemExit("color-mapped TGA not supported")

    bpp = 3
    pixel_count = width * height
    expected = pixel_offset + pixel_count * bpp
    if len(data) < expected:
        raise SystemExit(f"truncated image data: have {len(data)}, need {expected}")

    before_g = [0, 0]
    after_g = [0, 0]
    for i in range(pixel_count):
        off = pixel_offset + i * bpp
        # 24-bit TGA stores pixels as B, G, R
        g = data[off + 1]
        before_g[0] += g
        before_g[1] = max(before_g[1], g)
        g = 255 - g
        data[off + 1] = g
        after_g[0] += g
        after_g[1] = max(after_g[1], g)

    open(path, 'wb').write(data)

    print(f"{path}: {width}x{height} 24-bit uncompressed")
    print(f"  green mean: {before_g[0]/pixel_count:.1f} -> {after_g[0]/pixel_count:.1f}  (max {before_g[1]} -> {after_g[1]})")
    print("  green channel inverted (DX -> GL)")


if __name__ == '__main__':
    for p in sys.argv[1:]:
        flip_green_tga(p)

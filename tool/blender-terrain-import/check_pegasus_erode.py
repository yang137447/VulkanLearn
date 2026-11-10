"""验收：对比侵蚀前后，并输出与 Houdini 基准的对照图。

```powershell
& blender.exe --background --python tool\\blender-terrain-import\\check_pegasus_erode.py -- `
  --blend artifacts\\pegasus\\erode\\erode.blend --out artifacts\\pegasus\\erode
```
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy
import numpy as np


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blend", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--before", type=Path, default=None,
                        help="可选的 Houdini 基准 before_height.png")
    parser.add_argument("--after", type=Path, default=None,
                        help="可选的 Houdini 基准 after_height.png")
    return parser.parse_args(argv)


def read_png16(path: Path) -> np.ndarray:
    """读 16-bit 灰度 PNG（无 Pillow）。"""
    import struct
    import zlib

    raw = path.read_bytes()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    cursor = 8
    width = height = bit_depth = color_type = 0
    idat = bytearray()
    while cursor < len(raw):
        length = struct.unpack(">I", raw[cursor:cursor + 4])[0]
        tag = raw[cursor + 4:cursor + 8]
        data = raw[cursor + 8:cursor + 8 + length]
        if tag == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", data[:10])
        elif tag == b"IDAT":
            idat += data
        elif tag == b"IEND":
            break
        cursor += 12 + length
    decoded = zlib.decompress(bytes(idat))
    channels = 1 if color_type == 0 else 3
    stride = width * channels * (bit_depth // 8) + 1
    image = np.zeros((height, width), dtype=np.float32)
    for row in range(height):
        line = decoded[row * stride:(row + 1) * stride]
        payload = line[1:]
        if bit_depth == 16:
            values = np.frombuffer(payload, dtype=">u2").astype(np.float32)
        else:
            values = np.frombuffer(payload, dtype=np.uint8).astype(np.float32) * 257.0
        image[row] = values[:width * channels].reshape(width, channels)[:, 0]
    return image


def hillshade(height: np.ndarray, cell: float, azimuth=315.0, altitude=45.0):
    """山体阴影。

    ``height`` 必须是**米制**高度，``cell`` 是米制格距——用归一化高度画阴影
    会把 dz/dx 的绝对量级算错，任何微小的逐格抖动都会被放大成满对比度的碎渣
    （这是验收脚本自身的 bug，不是地形的形态）。
    """
    dzdx = np.gradient(height, axis=1) / cell
    dzdy = np.gradient(height, axis=0) / cell
    slope = np.arctan(np.hypot(dzdx, dzdy))
    aspect = np.arctan2(dzdy, -dzdx)
    shade = (np.sin(np.radians(altitude)) * np.cos(slope)
             + np.cos(np.radians(altitude)) * np.sin(slope)
             * np.cos(np.radians(azimuth) - aspect))
    return np.clip(shade, 0.0, 1.0)


def write_png(path: Path, gray: np.ndarray) -> None:
    rows, cols = gray.shape
    rgba = np.ones((rows, cols, 4), dtype=np.float32)
    rgba[:, :, :3] = np.repeat(np.clip(gray, 0.0, 1.0)[:, :, None], 3, axis=2)[::-1]
    image = bpy.data.images.new(path.stem, width=cols, height=rows)
    image.pixels.foreach_set(rgba.ravel())
    image.filepath_raw = str(path.resolve())
    image.file_format = "PNG"
    image.save()


def attribute(data, name: str, count: int) -> np.ndarray:
    values = np.zeros(count, dtype=np.float32)
    attribute_ref = data.attributes.get(name)
    if attribute_ref is None:
        return values
    attribute_ref.data.foreach_get("value", values)
    return values


def main() -> int:
    args = _parse_args()
    bpy.ops.wm.open_mainfile(filepath=str(args.blend.resolve()))
    obj = bpy.data.objects["VL_Pegasus_Erode"]
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    data = evaluated.data
    count = len(data.vertices)
    side = int(round(np.sqrt(count)))
    print(f"CHECK vertices={count} side={side}")

    coords = np.zeros(count * 3, dtype=np.float32)
    data.vertices.foreach_get("co", coords)
    z = coords.reshape(-1, 3)[:, 2]

    for layer in ("height", "water", "debris", "sediment", "bedrock"):
        values = attribute(data, layer, count)
        print(f"CHECK {layer}: min={values.min():.5f} max={values.max():.5f} "
              f"mean={values.mean():.5f} nonzero={int((values != 0).sum())}")

    height = attribute(data, "height", count)
    # 位移后的世界坐标才是"米制高度"，hillshade 必须用它。
    cell = 1000.0 / max(side - 1, 1)
    args.out.mkdir(parents=True, exist_ok=True)
    if side * side == count:
        grid = height.reshape(side, side, order="F")
        z_grid = z.reshape(side, side, order="F")
        low, high = float(grid.min()), float(grid.max())
        write_png(args.out / "gn_height.png",
                  (grid - low) / max(high - low, 1e-9))
        write_png(args.out / "gn_hillshade.png", hillshade(z_grid, cell))
        print(f"CHECK z_range_m {z_grid.min():.2f}..{z_grid.max():.2f} "
              f"cell={cell:.2f}")
        print(f"CHECK wrote={args.out}")

        if args.before and args.after:
            before = read_png16(args.before).astype(np.float32)
            after = read_png16(args.after).astype(np.float32)
            print(f"REF before range {before.min():.0f}..{before.max():.0f} "
                  f"after range {after.min():.0f}..{after.max():.0f}")
            # 归一化后比形态（幅度不同，比结构）
            def norm(arr):
                return (arr - arr.min()) / max(arr.max() - arr.min(), 1e-9)
            ref_before = norm(before)
            ref_after = norm(after)
            if ref_before.shape == (side, side):
                diff_before = np.abs(norm(grid) - ref_before)
                print(f"CHECK diff vs houdini_before: mean={diff_before.mean():.4f} "
                      f"max={diff_before.max():.4f}")
            if ref_after.shape == (side, side):
                diff_after = np.abs(norm(grid) - ref_after)
                print(f"CHECK diff vs houdini_after: mean={diff_after.mean():.4f} "
                      f"max={diff_after.max():.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

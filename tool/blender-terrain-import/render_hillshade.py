"""把 4 km 全局高度场渲染成一眼能看出起伏的 hillshade 图。

引擎截图受相机位姿与材质影响，很难直接看出地形起伏（4 km 范围只有 ±75 m）。
这里直接对高度场做山体阴影 + 色带，给出确定性的形态图。

用法：

```powershell
& blender.exe --background --python tool\\blender-terrain-import\\render_hillshade.py -- ^
  --input  <res>\\Generated\\BlenderPegasusGN\\Global\\Source\\heightmap_gn.png ^
  --meta   <res>\\Generated\\BlenderPegasusGN\\Global\\blender_gn_import.json ^
  --output artifacts\\pegasus\\pegasus_4km_hillshade.png --size-m 4000
```
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

import bpy
import numpy as np


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--meta", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-m", type=float, default=4000.0)
    parser.add_argument("--resolution", type=int, default=1024,
                        help="输出图像边长（影响观感细节）")
    return parser.parse_args(argv)


def read_png16(path: Path) -> np.ndarray:
    raw = path.read_bytes()
    cursor = 8
    width = height = color_type = 0
    idat = bytearray()
    while cursor < len(raw):
        length = struct.unpack(">I", raw[cursor:cursor + 4])[0]
        tag = raw[cursor + 4:cursor + 8]
        data = raw[cursor + 8:cursor + 8 + length]
        if tag == b"IHDR":
            width, height, _, color_type = struct.unpack(">IIBB", data[:10])
        elif tag == b"IDAT":
            idat += data
        elif tag == b"IEND":
            break
        cursor += 12 + length
    decoded = zlib.decompress(bytes(idat))
    channels = 1 if color_type == 0 else 3
    stride = width * channels * 2 + 1
    image = np.zeros((height, width), dtype=np.float64)
    for row in range(height):
        line = decoded[row * stride:(row + 1) * stride][1:]
        values = np.frombuffer(line, dtype=">u2").astype(np.float64)
        image[row] = values[:width * channels].reshape(width, channels)[:, 0]
    return image


def resize_bilinear(source: np.ndarray, side: int) -> np.ndarray:
    rows, cols = source.shape
    yi = np.linspace(0, rows - 1, side)
    xi = np.linspace(0, cols - 1, side)
    y0 = np.floor(yi).astype(int)
    x0 = np.floor(xi).astype(int)
    y1 = np.minimum(y0 + 1, rows - 1)
    x1 = np.minimum(x0 + 1, cols - 1)
    wy = (yi - y0)[:, None]
    wx = (xi - x0)[None, :]
    top = source[np.ix_(y0, x0)] * (1 - wx) + source[np.ix_(y0, x1)] * wx
    bottom = source[np.ix_(y1, x0)] * (1 - wx) + source[np.ix_(y1, x1)] * wx
    return top * (1 - wy) + bottom * wy


def write_rgb(path: Path, rgb: np.ndarray) -> None:
    rows, cols, _ = rgb.shape
    rgba = np.ones((rows, cols, 4), dtype=np.float32)
    rgba[:, :, :3] = np.clip(rgb, 0.0, 1.0)[::-1]
    image = bpy.data.images.new(path.stem, width=cols, height=rows)
    image.pixels.foreach_set(rgba.ravel())
    image.filepath_raw = str(path.resolve())
    image.file_format = "PNG"
    image.save()


def main() -> int:
    args = _parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    manifest = json.loads(args.meta.read_text(encoding="utf-8-sig"))
    low = float(manifest["sourceLow"])
    high = float(manifest["sourceHigh"])
    raw = read_png16(args.input)
    height = low + raw / 65535.0 * (high - low)
    height = resize_bilinear(height, args.resolution)

    side = height.shape[0]
    cell = args.size_m / max(side - 1, 1)

    # 山体阴影：西北 315°、高度角 45°
    dzdx = np.gradient(height, axis=1) / cell
    dzdy = np.gradient(height, axis=0) / cell
    slope = np.arctan(np.hypot(dzdx, dzdy))
    aspect = np.arctan2(dzdy, -dzdx)
    shade = (np.sin(np.radians(45.0)) * np.cos(slope)
             + np.cos(np.radians(45.0)) * np.sin(slope)
             * np.cos(np.radians(315.0) - aspect))
    shade = np.clip(shade, 0.0, 1.0)

    # 高程色带：低处青灰 -> 中段草绿 -> 高处岩褐 -> 峰顶近白
    normalized = (height - low) / max(high - low, 1e-9)
    stops = np.array([0.0, 0.30, 0.55, 0.78, 1.0])
    colors = np.array([
        [0.26, 0.32, 0.36],
        [0.34, 0.42, 0.30],
        [0.46, 0.50, 0.34],
        [0.56, 0.48, 0.40],
        [0.82, 0.80, 0.76],
    ])
    ramp = np.zeros((side, side, 3))
    for channel in range(3):
        ramp[:, :, channel] = np.interp(normalized, stops, colors[:, channel])

    # 阴影以 0.55 为中心调制，保留色带可辨识度
    rgb = ramp * (0.45 + 0.85 * shade[:, :, None])
    write_rgb(args.output, rgb)
    print(f"HILLSHADE height {low:.2f}..{high:.2f} m  -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

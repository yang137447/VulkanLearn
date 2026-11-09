"""把 Project Pegasus 的岛屿 landscape 导出成 VulkanLearn 的 16-bit 层。

与 ``export_pegasus_4km.py`` 不同，这里不再放大侵蚀示例，而是直接采样
Pegasus 工程自带的 landscape heightfield 缓存：

```text
Pegasus_Landscape_Layers.bgeo.sc        height + Layer0/1/2
Pegasus_Landscape_Later_1_Source.bgeo.sc height + mask/water/debris/sediment/bedrock
```

岛的世界尺寸来自 height volume 的 bounding box（约 4032 m），不是脚本猜值。

用法：
    hython.exe tool\\houdini-terrain-import\\export_pegasus_island.py -- ^
      --out D:\\YYBWorkSpace\\Pegasus\\decoded\\island --samples 1024
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
import zlib


DEFAULT_LAYERS = (
    r"D:\YYBWorkSpace\Pegasus\LandscapeWorkflow"
    r"\ProjectFiles_LandscapeWorkflow\SourceContent\Pegasus\Landscape"
    r"\Landscape Heightfield Layers\Pegasus_Landscape_Layers.bgeo.sc")
DEFAULT_ERODE = (
    r"D:\YYBWorkSpace\Pegasus\LandscapeWorkflow"
    r"\ProjectFiles_LandscapeWorkflow\SourceContent\Pegasus\Landscape"
    r"\Landscape Heightfield Layers\Pegasus_Landscape_Later_1_Source.bgeo.sc")


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layers-file", default=DEFAULT_LAYERS)
    parser.add_argument("--erode-file", default=DEFAULT_ERODE)
    parser.add_argument("--out", required=True)
    parser.add_argument("--samples", type=int, default=1024)
    parser.add_argument("--tile-grid", type=int, default=4)
    parser.add_argument("--tile-resolution", type=int, default=256)
    parser.add_argument("--tile-margin", type=int, default=1)
    return parser.parse_args(argv)


def write_gray16_png(path: str, rows) -> None:
    height = len(rows)
    width = len(rows[0])

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)
    raw = b"".join(b"\x00" + b"".join(struct.pack(">H", int(v)) for v in row)
                   for row in rows)
    with open(path, "wb") as stream:
        stream.write(b"\x89PNG\r\n\x1a\n")
        stream.write(chunk(b"IHDR", ihdr))
        stream.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        stream.write(chunk(b"IEND", b""))


def load_layers(path: str):
    import hou
    obj = hou.node("/obj")
    geo = obj.createNode("geo", "export_geo", run_init_scripts=False)
    file_sop = geo.createNode("file", "file1")
    file_sop.parm("file").set(path)
    file_sop.cook(force=True)
    geometry = file_sop.geometry()
    prims = {}
    for prim in geometry.prims():
        try:
            prims[prim.attribValue("name")] = prim
        except Exception:  # noqa: BLE001
            continue
    return geo, prims


def sample_rect(prim, origin, size_x, size_z, y_mid, side: int):
    import hou
    grid = []
    step_x = size_x / max(side - 1, 1)
    step_z = size_z / max(side - 1, 1)
    for j in range(side):
        row = []
        z = origin[1] + j * step_z
        for i in range(side):
            x = origin[0] + i * step_x
            row.append(float(prim.sample(hou.Vector3(x, y_mid, z))))
        grid.append(row)
    return grid


def export_layer(prims, name, origin, size_x, size_z, side,
                 out_dir, stats):
    prim = prims.get(name)
    if prim is None:
        print(f"EXP missing layer {name}")
        return
    # 每个 volume 的 y 范围不同（mask/water 等是薄层），必须用自身的 bbox 取中面。
    bound = prim.boundingBox()
    y_mid = bound.minvec().y() + 0.5 * (bound.maxvec().y() - bound.minvec().y())
    started = time.time()
    grid = sample_rect(prim, origin, size_x, size_z, y_mid, side)
    flat = [value for row in grid for value in row]
    low, high = min(flat), max(flat)
    if high - low < 1e-9:
        print(f"EXP {name}: constant {low:.4f}")
        stats[name] = {"min": low, "max": high, "flat": True}
        return
    scale = 65535.0 / (high - low)
    rows = [[int((value - low) * scale) for value in row] for row in grid]
    write_gray16_png(os.path.join(out_dir, f"height_after_{name}.png"), rows)
    with open(os.path.join(out_dir, f"height_after_{name}.txt"), "w",
              encoding="utf-8") as stream:
        stream.write(f"layer={name}\nmin={low}\nmax={high}\n"
                     f"resolution={side}\n")
    stats[name] = {"min": low, "max": high}
    print(f"EXP {name}: {low:.3f} .. {high:.3f}  "
          f"({time.time() - started:.1f}s)")


def main() -> int:
    args = _parse_args()
    out_dir = os.path.abspath(args.out)
    global_dir = os.path.join(out_dir, "global")
    os.makedirs(global_dir, exist_ok=True)

    geo_a, layers = load_layers(os.path.abspath(args.layers_file))
    geo_b, erode = load_layers(os.path.abspath(args.erode_file))

    height = layers.get("height")
    if height is None:
        print("EXP ERROR height layer missing")
        return 1
    bound = height.boundingBox()
    lo = bound.minvec()
    hi = bound.maxvec()
    size_x = hi.x() - lo.x()
    size_z = hi.z() - lo.z()
    print(f"EXP island size={size_x:.1f} x {size_z:.1f} m "
          f"samples={args.samples}")

    origin = (lo.x(), lo.z())
    stats = {}
    export_layer(layers, "height", origin, size_x, size_z,
                 args.samples, global_dir, stats)
    for name in ("mask", "bedrock", "debris", "sediment", "water"):
        export_layer(erode, name, origin, size_x, size_z,
                     args.samples, global_dir, stats)

    manifest = {
        "sizeM": float(round(0.5 * (size_x + size_z), 3)),
        "samples": args.samples,
        "tileGrid": args.tile_grid,
        "tileResolution": args.tile_resolution,
        "tileMargin": args.tile_margin,
        "source": {
            "layersFile": os.path.abspath(args.layers_file),
            "erodeFile": os.path.abspath(args.erode_file),
        },
        "stats": {"after": stats},
    }
    with open(os.path.join(out_dir, "manifest.json"), "w",
              encoding="utf-8") as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
    print(f"EXP wrote={out_dir} sizeM={manifest['sizeM']}")

    geo_a.destroy()
    geo_b.destroy()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

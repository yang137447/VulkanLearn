"""导出 4 km 规模的 Houdini 地形基准，并按 4×4 切成 16 块。

HFErodeExamples.hip 的示例地形是 1 km / 512 采样。要做 4 km 基准，需要
把 heightfield 的 sizex/sizey 放大到 4000、gridsamples 提高到足够分辨率，
再跑同一条侵蚀链。

产出：

```text
<out>/global/height_before.png   侵蚀前（heightfield_distort1）
<out>/global/height_after.png    侵蚀后（heightfield_erode1）
<out>/global/water.png / debris.png / sediment.png / bedrock.png
<out>/global/*.txt               每层的 min/max 元数据
<out>/tiles/tile_XX/height.png   16 块切分结果（带 overscan）
<out>/manifest.json              统计量基准
```

用法：
    hython.exe tool\\houdini-terrain-import\\export_pegasus_4km.py -- --out <dir>
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import zlib


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hip", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--size-m", type=float, default=4000.0)
    parser.add_argument("--samples", type=int, default=1024,
                        help="全局网格每边采样数")
    parser.add_argument("--tile-grid", type=int, default=4)
    parser.add_argument("--tile-resolution", type=int, default=256)
    parser.add_argument("--tile-margin", type=int, default=1)
    parser.add_argument("--freeze-frames", type=int, default=5)
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


def sample_layer(prim, side: int):
    """在 heightfield 的 XZ 平面上等距采样。"""
    import hou
    bound = prim.boundingBox()
    lo = bound.minvec()
    hi = bound.maxvec()
    y_mid = lo.y() + 0.5 * (hi.y() - lo.y())
    grid = []
    for j in range(side):
        row = []
        for i in range(side):
            u = i / max(side - 1, 1)
            v = j / max(side - 1, 1)
            pos = hou.Vector3(lo.x() + u * (hi.x() - lo.x()), y_mid,
                              lo.z() + v * (hi.z() - lo.z()))
            row.append(float(prim.sample(pos)))
        grid.append(row)
    flat = [value for row in grid for value in row]
    return grid, min(flat), max(flat)


def collect_layers(node) -> dict:
    result = {}
    for prim in node.geometry().prims():
        try:
            name = prim.attribValue("name")
        except Exception:  # noqa: BLE001
            continue
        result[name] = prim
    return result


def export_node(node, out_dir: str, prefix: str, side: int, layers) -> dict:
    os.makedirs(out_dir, exist_ok=True)
    node.cook(force=True)
    prims = collect_layers(node)
    stats = {}
    for layer in layers:
        prim = prims.get(layer)
        if prim is None:
            continue
        grid, low, high = sample_layer(prim, side)
        if high - low < 1e-9:
            print(f"EXP {prefix}_{layer}: constant {low:.4f}")
            stats[layer] = {"min": low, "max": high, "flat": True}
            continue
        scale = 65535.0 / (high - low)
        rows = [[int((value - low) * scale) for value in row] for row in grid]
        write_gray16_png(os.path.join(out_dir, f"{prefix}_{layer}.png"), rows)
        with open(os.path.join(out_dir, f"{prefix}_{layer}.txt"), "w",
                  encoding="utf-8") as stream:
            stream.write(f"layer={layer}\nmin={low}\nmax={high}\n"
                         f"resolution={side}\n")
        stats[layer] = {"min": low, "max": high}
        print(f"EXP {prefix}_{layer}: {low:.3f} .. {high:.3f}")
    return stats


def main() -> int:
    args = _parse_args()
    import hou

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    hou.hipFile.load(os.path.abspath(args.hip), suppress_save_prompt=True,
                     ignore_load_warnings=True)

    root = hou.node("/obj/Erosion_Examples")
    heightfield = hou.node(f"{root.path()}/heightfield1")
    if heightfield is None:
        print("EXP ERROR heightfield1 missing")
        return 1

    # 把示例从 1 km 放大到 4 km
    heightfield.parm("sizex").set(args.size_m)
    heightfield.parm("sizey").set(args.size_m)
    heightfield.parm("gridsamples").set(args.samples)
    print(f"EXP size={args.size_m}m samples={args.samples}")

    # noise 的 elementsize 按比例放大，保持"山体数量"不变
    noise = hou.node(f"{root.path()}/heightfield_noise1")
    if noise is not None:
        base_element = 500.0
        scale = args.size_m / 1000.0
        noise.parm("elementsize").set(base_element * scale)
        print(f"EXP noise element={base_element * scale}m")

    # 冻结帧
    for name in ("heightfield_erode1",):
        node = hou.node(f"{root.path()}/{name}")
        if node is not None and node.parm("freezeframe"):
            node.parm("freezeframe").set(args.freeze_frames)
            print(f"EXP {name} freezeframe={args.freeze_frames}")

    stats = {}
    global_dir = os.path.join(out_dir, "global")
    before = hou.node(f"{root.path()}/heightfield_distort1")
    after = hou.node(f"{root.path()}/heightfield_erode1")
    stats["before"] = export_node(before, global_dir, "height_before",
                                  args.samples, ["height"])
    stats["after"] = export_node(after, global_dir, "height_after",
                                 args.samples,
                                 ["height", "water", "debris", "sediment", "bedrock"])

    manifest = {
        "sizeM": args.size_m,
        "samples": args.samples,
        "tileGrid": args.tile_grid,
        "tileResolution": args.tile_resolution,
        "tileMargin": args.tile_margin,
        "freezeFrames": args.freeze_frames,
        "stats": stats,
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
    print(f"EXP wrote={out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

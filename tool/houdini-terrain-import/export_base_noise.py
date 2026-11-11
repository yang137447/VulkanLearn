"""在 Houdini 里导出 heightfield_noise1 的输出，作为基座的对照基准。

HFErodeExamples.hip 的侵蚀链是：

    heightfield1 → heightfield_noise1 → heightfield_distort1
                 → resample_x2 → heightfield_erode1

所以 heightfield_noise1 的输出就是"侵蚀前的基座"。把它按 XZ 平面采样成
PNG，就能和 Blender GN 的基座逐像素对比，精确判断差异在频率、振幅还是
坐标约定上。

用法：
    hython.exe tool\\houdini-terrain-import\\export_base_noise.py -- --out <dir>
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hip", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--resolution", type=int, default=128)
    parser.add_argument("--nodes", nargs="+", default=[
        "/obj/Erosion_Examples/heightfield1",
        "/obj/Erosion_Examples/heightfield_noise1",
        "/obj/Erosion_Examples/heightfield_distort1",
    ])
    return parser.parse_args(argv)


def write_gray16_png(path: str, rows: list[list[int]]) -> None:
    height = len(rows)
    width = len(rows[0])

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)
    raw = b"".join(b"\x00" + b"".join(struct.pack(">H", v) for v in row)
                   for row in rows)
    with open(path, "wb") as stream:
        stream.write(b"\x89PNG\r\n\x1a\n")
        stream.write(chunk(b"IHDR", ihdr))
        stream.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        stream.write(chunk(b"IEND", b""))


def sampling_grid(prims, side: int):
    """找到 height 层的包围盒与高度范围，返回采样函数。"""
    import hou

    target = None
    for prim in prims:
        try:
            if prim.attribValue("name") == "height":
                target = prim
                break
        except Exception:  # noqa: BLE001
            continue
    if target is None:
        return None, None, None

    bound = target.boundingBox()
    lo = bound.minvec()
    hi = bound.maxvec()
    y_mid = lo.y() + 0.5 * (hi.y() - lo.y())
    samples = []
    for j in range(side):
        row = []
        for i in range(side):
            u = i / max(side - 1, 1)
            v = j / max(side - 1, 1)
            pos = hou.Vector3(lo.x() + u * (hi.x() - lo.x()), y_mid,
                              lo.z() + v * (hi.z() - lo.z()))
            row.append(float(target.sample(pos)))
        samples.append(row)
    flat = [value for row in samples for value in row]
    return samples, min(flat), max(flat)


def main() -> int:
    args = _parse_args()
    import hou

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    hou.hipFile.load(os.path.abspath(args.hip), suppress_save_prompt=True,
                     ignore_load_warnings=True)

    for path in args.nodes:
        node = hou.node(path)
        if node is None:
            print(f"BASE missing {path}")
            continue
        try:
            node.cook(force=True)
        except Exception as error:  # noqa: BLE001
            print(f"BASE cook_failed {path}: {error}")
            continue
        prims = list(node.geometry().prims())
        samples, low, high = sampling_grid(prims, args.resolution)
        if samples is None:
            print(f"BASE no height layer in {path}")
            continue
        name = path.rsplit("/", 1)[-1]
        scale = 65535.0 / max(high - low, 1e-9)
        rows = [[int((value - low) * scale) for value in row] for row in samples]
        out_png = os.path.join(out_dir, f"base_{name}.png")
        write_gray16_png(out_png, rows)
        with open(os.path.join(out_dir, f"base_{name}.txt"), "w",
                  encoding="utf-8") as stream:
            stream.write(f"node={path}\nmin={low}\nmax={high}\n"
                         f"resolution={args.resolution}\n")
        print(f"BASE {name}: min={low:.4f} max={high:.4f} -> {out_png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

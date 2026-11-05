"""从 Pegasus 的 HFErodeExamples.hip 生成真实侵蚀前后的对照高度场。

这个 hip 是纯程序生成的（heightfield → noise → distort → erode），不依赖外部
文件，所以可以在 hython 里完整 cook，导出：

* `before.png` — 侵蚀前（heightfield_distort1 之后）
* `after.png`  — 侵蚀后（heightfield_erode1 之后）
* `after_water.png` / `after_debris.png` — 水层与碎屑层

这三张图就是 Blender GN 版本必须对上的基准。
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
    parser.add_argument("--node", default="/obj/Erosion_Examples/heightfield_erode1")
    parser.add_argument("--before-node", default="/obj/Erosion_Examples/heightfield_distort1")
    parser.add_argument("--out", required=True)
    parser.add_argument("--resolution", type=int, default=256)
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


def sample_layer(prim, side: int, resolution: int) -> tuple[list[list[float]], float, float]:
    """在 heightfield 的 XZ 平面上采样。

    Houdini 的 heightfield 是 **XZ 平面 + Y 轴为高度**（不是 XY 平面）。
    早期版本在 X 和 Y 上都铺满范围，Y 同时也被当成高度轴，采出来就是竖条。
    这里固定 Y 在包围盒中点，只在 X/Z 上遍历。
    """
    import hou
    bound = prim.boundingBox()
    lo = bound.minvec()
    hi = bound.maxvec()
    y_mid = lo.y() + 0.5 * (hi.y() - lo.y())
    samples = []
    for j in range(side):
        row = []
        for i in range(side):
            u = i / max(side - 1, 1)
            v = j / max(side - 1, 1)
            pos = hou.Vector3(lo.x() + u * (hi.x() - lo.x()),
                              y_mid,
                              lo.z() + v * (hi.z() - lo.z()))
            row.append(float(prim.sample(pos)))
        samples.append(row)
    flat = [value for row in samples for value in row]
    return samples, min(flat), max(flat)


def export_node(node, out_dir: str, prefix: str, side: int, layers: list[str]) -> None:
    import hou
    node.cook(force=True)
    geometry = node.geometry()
    found = {}
    for prim in geometry.prims():
        try:
            name = prim.attribValue("name")
        except Exception:  # noqa: BLE001
            continue
        found[name] = prim
    print(f"EXP {node.path()} layers={sorted(found)}")
    for layer in layers:
        prim = found.get(layer)
        if prim is None:
            continue
        samples, low, high = sample_layer(prim, side, side)
        if high - low < 1e-9:
            print(f"EXP {prefix}_{layer}: constant {low:.4f}")
            continue
        scale = 65535.0 / (high - low)
        rows = [[int((value - low) * scale) for value in row] for row in samples]
        path = os.path.join(out_dir, f"{prefix}_{layer}.png")
        write_gray16_png(path, rows)
        with open(os.path.join(out_dir, f"{prefix}_{layer}.txt"), "w",
                  encoding="utf-8") as stream:
            stream.write(f"layer={layer}\nmin={low}\nmax={high}\n")
        print(f"EXP {prefix}_{layer}: min={low:.4f} max={high:.4f} -> {path}")
    del hou


def main() -> int:
    args = _parse_args()
    import hou

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    hou.hipFile.load(os.path.abspath(args.hip), suppress_save_prompt=True,
                     ignore_load_warnings=True)
    print(f"EXP hip={args.hip}")

    before = hou.node(args.before_node)
    after = hou.node(args.node)
    if before is None or after is None:
        print("EXP ERROR node not found")
        return 1

    export_node(before, out_dir, "before", args.resolution, ["height"])
    export_node(after, out_dir, "after", args.resolution,
                ["height", "water", "debris", "sediment", "bedrock"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

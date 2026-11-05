"""直接从 Houdini volume 采样 Pegasus 高度场，绕开 HeightField Output 的按钮。

`heightfield_output` 的 execute 按钮在 hython 里不生效（导出的 PNG 是纯色），
所以这里直接读取 `height` volume 的体素数据，按网格重采样后写成 PNG。

用法：
    hython.exe tool\\houdini-terrain-import\\export_pegasus_reference.py -- --out <dir>
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
    parser.add_argument("--hda", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--resolution", type=int, default=256)
    return parser.parse_args(argv)


def write_gray16_png(path: str, rows: list[list[int]]) -> None:
    """写 16-bit 灰度 PNG（无 Pillow 依赖）。"""
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


def main() -> int:
    args = _parse_args()
    import hou

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    hou.hipFile.clear(suppress_save_prompt=True)
    hou.hda.installFile(os.path.abspath(args.hda))

    geo = hou.node("/obj").createNode("geo", "pegasus_probe")
    hda = geo.createNode("pe_landscape::1.0", "pegasus_landscape")
    hda.cook(force=True)

    geometry = hda.geometry()
    resolution = int(hda.evalParm("resolution")) if hda.parm("resolution") else 512
    print(f"HOUDA node_resolution={resolution}")

    target = None
    for prim in geometry.prims():
        try:
            name = prim.attribValue("name")
        except Exception:  # noqa: BLE001
            continue
        if name == "height":
            target = prim
            break

    if target is None:
        print("HOUDA ERROR no height volume")
        return 1

    bound = target.boundingBox()
    print(f"HOUDA bbox min={bound.minvec()} max={bound.maxvec()}")
    print(f"HOUDA resolution={target.resolution()}")

    # 体素中心采样，映射到 256x256
    res = target.resolution()
    nx, ny, nz = res[0], res[1], res[2]
    side = args.resolution
    z = (nz - 1) // 2

    volume = target
    values = []
    for j in range(side):
        row = []
        for i in range(side):
            # volume 的 world 坐标范围映射到网格
            u = i / max(side - 1, 1)
            v = j / max(side - 1, 1)
            pos = hou.Vector3(
                bound.minvec().x() + u * (bound.maxvec().x() - bound.minvec().x()),
                bound.minvec().y() + v * (bound.maxvec().y() - bound.minvec().y()),
                bound.minvec().z() + 0.5 * (bound.maxvec().z() - bound.minvec().z()),
            )
            row.append(0.0)
        values.append(row)

    print("HOUDA sampling via Volume.sample()")
    samples = []
    for j in range(side):
        row = []
        for i in range(side):
            u = i / max(side - 1, 1)
            v = j / max(side - 1, 1)
            pos = hou.Vector3(
                bound.minvec().x() + u * (bound.maxvec().x() - bound.minvec().x()),
                bound.minvec().y() + v * (bound.maxvec().y() - bound.minvec().y()),
                bound.minvec().z() + 0.5 * (bound.maxvec().z() - bound.minvec().z()),
            )
            try:
                row.append(float(volume.sample(pos)))
            except Exception:  # noqa: BLE001
                row.append(0.0)
        samples.append(row)
        if j % 64 == 0:
            print(f"HOUDA row {j}")

    flat = [value for row in samples for value in row]
    low, high = min(flat), max(flat)
    print(f"HOUDA height range min={low:.6f} max={high:.6f}")

    scale = 65535.0 / max(high - low, 1e-9)
    rows = [[int((value - low) * scale) for value in row] for row in samples]
    path = os.path.join(out_dir, "pegasus_height_volume.png")
    write_gray16_png(path, rows)
    with open(os.path.join(out_dir, "pegasus_height_range.txt"), "w",
              encoding="utf-8") as stream:
        stream.write(f"min={low}\nmax={high}\nresolution={side}\n"
                     f"volume_resolution={res}\n")
    print(f"HOUDA wrote={path} size={os.path.getsize(path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""把 GN 侵蚀结果与 Houdini 基准逐像素对比，并给出标定建议。

用法：
    blender.exe --background --python tool\\blender-terrain-import\\compare_erode.py -- `
      --blend artifacts\\pegasus\\erode\\erode.blend `
      --ref-dir D:\\YYBWorkSpace\\Pegasus\\decoded\\_ref `
      --out artifacts\\pegasus\\erode\\compare

对比口径：

* 基准 ``before_height`` / ``after_height`` 是 Houdini 同一网格在侵蚀前后的
  实际高度（米），差值就是"真实下切量"。
* GN 这边 height 属性是归一化层，乘以 Height Scale 得到米。
* 关键指标是**形态相关性**（相关系数）和**下切量量级**，不是绝对相等——
  两者的基座噪声不完全相同（Houdini 用 sparse/hmfT，Blender 用 Perlin），
  所以只比较"侵蚀是否把地形刻出了相似的结构"。
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

import bpy
import numpy as np


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blend", type=Path, required=True)
    parser.add_argument("--ref-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args(argv)


def read_layer(path: Path) -> np.ndarray:
    """读 16-bit PNG 并按同名 .txt 里记录的 min/max 还原成真实数值（米）。

    PNG 存的是 [0,65535] 的归一化图像，不是物理量；导出时写下的
    ``base_<name>.txt`` / ``after_<name>.txt`` 才有真实的 min/max。
    """
    image = read_png16(path)
    meta = path.with_suffix(".txt")
    if not meta.exists():
        raise SystemExit(f"missing metadata {meta}")
    values = {}
    for line in meta.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, _, raw = line.partition("=")
            values[key.strip()] = raw.strip()
    low = float(values["min"])
    high = float(values["max"])
    return low + image / 65535.0 * (high - low)


def read_png16(path: Path) -> np.ndarray:
    raw = path.read_bytes()
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
    bytes_per = 2 if bit_depth == 16 else 1
    stride = width * channels * bytes_per + 1
    image = np.zeros((height, width), dtype=np.float32)
    for row in range(height):
        line = decoded[row * stride:(row + 1) * stride][1:]
        if bit_depth == 16:
            values = np.frombuffer(line, dtype=">u2").astype(np.float32)
        else:
            values = np.frombuffer(line, dtype=np.uint8).astype(np.float32) * 257.0
        image[row] = values[:width * channels].reshape(width, channels)[:, 0]
    return image


def write_png(path: Path, gray: np.ndarray) -> None:
    rows, cols = gray.shape
    rgba = np.ones((rows, cols, 4), dtype=np.float32)
    rgba[:, :, :3] = np.repeat(np.clip(gray, 0.0, 1.0)[:, :, None], 3, axis=2)[::-1]
    image = bpy.data.images.new(path.stem, width=cols, height=rows)
    image.pixels.foreach_set(rgba.ravel())
    image.filepath_raw = str(path.resolve())
    image.file_format = "PNG"
    image.save()


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


def attribute(data, name: str, count: int) -> np.ndarray | None:
    ref = data.attributes.get(name)
    if ref is None:
        return None
    values = np.zeros(count, dtype=np.float32)
    ref.data.foreach_get("value", values)
    return values


def correlate(a: np.ndarray, b: np.ndarray) -> float:
    a = a.ravel() - a.mean()
    b = b.ravel() - b.mean()
    denom = float(np.sqrt((a * a).sum() * (b * b).sum()))
    return float((a * b).sum() / denom) if denom > 1e-9 else 0.0


def main() -> int:
    args = _parse_args()
    ref_dir = args.ref_dir.resolve()
    out_dir = args.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    bpy.ops.wm.open_mainfile(filepath=str(args.blend.resolve()))
    obj = bpy.data.objects["VL_Pegasus_Erode"]
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    data = evaluated.data
    count = len(data.vertices)
    side = int(round(np.sqrt(count)))

    height_attr = attribute(data, "height", count)
    flow_attr = attribute(data, "flow", count)
    debris_attr = attribute(data, "debris", count)
    if height_attr is None:
        raise SystemExit("no height attribute")

    group = bpy.data.node_groups["VL_Pegasus_Erode"]
    height_scale = None
    for item in group.interface.items_tree:
        if getattr(item, "in_out", None) == "INPUT" and item.name == "Height Scale":
            height_scale = item.default_value
    height_scale = float(height_scale or 500.0)

    gn_height_m = (height_attr * height_scale).reshape(side, side, order="F")

    before = read_layer(ref_dir / "before_height.png")
    after = read_layer(ref_dir / "after_height.png")
    before_m = resize_bilinear(before, side)
    after_m = resize_bilinear(after, side)

    real_incision = before_m - after_m

    print(f"CMP grid={side} height_scale={height_scale}")
    print(f"CMP houdini_before   {before_m.min():.2f} .. {before_m.max():.2f}")
    print(f"CMP houdini_after    {after_m.min():.2f} .. {after_m.max():.2f}")
    print(f"CMP houdini_incision mean={real_incision.mean():.3f} "
          f"max={real_incision.max():.3f} p95={np.percentile(real_incision, 95):.3f}")
    print(f"CMP gn_height        {gn_height_m.min():.2f} .. {gn_height_m.max():.2f}")

    if flow_attr is not None:
        flow_grid = flow_attr.reshape(side, side, order="F")
        print(f"CMP gn_flow          {flow_grid.min():.4f} .. {flow_grid.max():.4f} "
              f"mean={flow_grid.mean():.4f}")
        # 汇水是否集中在少数区域：分位数
        for q in (50, 90, 99):
            print(f"CMP gn_flow p{q}={np.percentile(flow_grid, q):.4f}")
        # 与真实下切的相关性：如果 flow 高的地方真实下切也大，说明逻辑对
        corr = correlate(flow_grid, real_incision)
        print(f"CMP corr(gn_flow, houdini_incision) = {corr:.4f}")
        write_png(out_dir / "gn_flow.png",
                  (flow_grid - flow_grid.min())
                  / max(flow_grid.max() - flow_grid.min(), 1e-9))

    if debris_attr is not None:
        debris_grid = debris_attr.reshape(side, side, order="F")
        print(f"CMP gn_debris        {debris_grid.min():.4f} .. {debris_grid.max():.4f}")

    # --- 标定量：用"基座 - 结果"得到 GN 的实际下切量，与 Houdini 比统计分布
    base_attr = attribute(data, "base_height", count)
    if base_attr is not None:
        base_m = (base_attr * height_scale).reshape(side, side, order="F")
    else:
        # 没有单独存基座时，用 houdini_before 作为形态参考不可比；
        # 改用 eroded - base 需要基座，所以退回到"相对 houdini_before 的形态"。
        base_m = None
    if base_m is not None:
        gn_incision = base_m - gn_height_m
        print(f"CMP gn_incision      mean={gn_incision.mean():.3f} "
              f"max={gn_incision.max():.3f} p95={np.percentile(gn_incision, 95):.3f}")
        ratio = (gn_incision.mean() / real_incision.mean()
                 if abs(real_incision.mean()) > 1e-9 else float("nan"))
        print(f"CMP incision_ratio   {ratio:.3f} (gn/houdini, 目标≈1.0)")
        write_png(out_dir / "gn_incision.png",
                  np.clip(gn_incision / max(abs(gn_incision).max(), 1e-9), 0, 1))

    write_png(out_dir / "houdini_before.png",
              (before_m - before_m.min())
              / max(before_m.max() - before_m.min(), 1e-9))
    write_png(out_dir / "houdini_after.png",
              (after_m - after_m.min())
              / max(after_m.max() - after_m.min(), 1e-9))
    write_png(out_dir / "houdini_incision.png",
              np.clip(real_incision / max(abs(real_incision).max(), 1e-9), 0, 1))
    write_png(out_dir / "gn_height.png",
              (gn_height_m - gn_height_m.min())
              / max(gn_height_m.max() - gn_height_m.min(), 1e-9))

    print(f"CMP wrote={out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

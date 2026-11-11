"""比较 VL_HF 节点库产出的地形与 Houdini 基准的统计量。

比较项（都是与尺度无关的分布量，避免"基座噪声不同"导致的逐像素误差）：

| 指标 | 含义 |
|---|---|
| 高程范围 | 米，min/max |
| 高程标准差 | 起伏总量 |
| 平均坡度 | 每格高度差 / 格距（度） |
| 坡度 p50 / p95 | 坡度分布 |
| 下切量均值 | 侵蚀前后高度差 |
| 曲率分布 | 二阶差分的 p5/p95，反映沟谷锐度 |
| 分形维数近似 | 不同尺度的坡度衰减比 |

用法：

```powershell
py -3 tool\\houdini-terrain-import\\compare_statistics.py ^
  --gn <gn_height.png> --gn-meta <gn_meta.json> ^
  --ref <ref_height_before.png> <ref_height_after.png> ^
  --ref-meta <ref_manifest.json>
```
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np


def read_png16(path: Path) -> np.ndarray:
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
    bytes_per = 2 if bit_depth == 16 else 1
    stride = width * channels * bytes_per + 1
    image = np.zeros((height, width), dtype=np.float64)
    for row in range(height):
        line = decoded[row * stride:(row + 1) * stride][1:]
        if bit_depth == 16:
            values = np.frombuffer(line, dtype=">u2").astype(np.float64)
        else:
            values = np.frombuffer(line, dtype=np.uint8).astype(np.float64) * 257.0
        image[row] = values[:width * channels].reshape(width, channels)[:, 0]
    return image


def read_meta(path: Path) -> dict:
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, _, raw = line.partition("=")
            result[key.strip()] = raw.strip()
    return result


def metrics(grid_m: np.ndarray, cell_m: float) -> dict:
    """计算一组与尺度无关的地形统计量。"""
    dzdx = np.gradient(grid_m, axis=1) / cell_m
    dzdy = np.gradient(grid_m, axis=0) / cell_m
    gradient = np.hypot(dzdx, dzdy)
    slope_deg = np.degrees(np.arctan(gradient))

    # 曲率：二阶差分（曲率为负 = 凸起/山脊，正 = 凹陷/沟谷）
    laplace = (np.roll(grid_m, 1, axis=0) + np.roll(grid_m, -1, axis=0)
               + np.roll(grid_m, 1, axis=1) + np.roll(grid_m, -1, axis=1)
               - 4.0 * grid_m) / (cell_m * cell_m)

    # 分形维数近似：半分辨率后的坡度衰减
    coarse = grid_m[::2, ::2]
    coarse_grad = np.hypot(np.gradient(coarse, axis=1) / (cell_m * 2),
                           np.gradient(coarse, axis=0) / (cell_m * 2))
    ratio = float(np.mean(coarse_grad) / max(np.mean(gradient), 1e-9))

    return {
        "min": float(grid_m.min()),
        "max": float(grid_m.max()),
        "std": float(grid_m.std()),
        "slope_mean": float(slope_deg.mean()),
        "slope_p50": float(np.percentile(slope_deg, 50)),
        "slope_p95": float(np.percentile(slope_deg, 95)),
        "curvature_p5": float(np.percentile(laplace, 5)),
        "curvature_p95": float(np.percentile(laplace, 95)),
        "coarse_slope_ratio": ratio,
    }


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gn", type=Path, required=True,
                        help="节点库产出的 heightmap PNG")
    parser.add_argument("--gn-meta", type=Path, required=True,
                        help="节点库 manifest（含 sourceLow/sourceHigh）")
    parser.add_argument("--ref-before", type=Path, required=True)
    parser.add_argument("--ref-after", type=Path, required=True)
    parser.add_argument("--ref-meta-before", type=Path, required=True)
    parser.add_argument("--ref-meta-after", type=Path, required=True)
    parser.add_argument("--size-m", type=float, default=4000.0)
    parser.add_argument("--json", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    # --- 节点库产出：PNG 是 [0,1] 归一化，按 manifest 的 low/high 还原
    manifest = json.loads(args.gn_meta.read_text(encoding="utf-8"))
    gn_low = float(manifest["sourceLow"])
    gn_high = float(manifest["sourceHigh"])
    gn_raw = read_png16(args.gn)
    gn_m = gn_low + gn_raw / 65535.0 * (gn_high - gn_low)
    gn_cell = args.size_m / max(gn_m.shape[0] - 1, 1)

    # --- Houdini 基准
    before_meta = read_meta(args.ref_meta_before)
    after_meta = read_meta(args.ref_meta_after)
    before_raw = read_png16(args.ref_before)
    after_raw = read_png16(args.ref_after)
    before_low, before_high = float(before_meta["min"]), float(before_meta["max"])
    after_low, after_high = float(after_meta["min"]), float(after_meta["max"])
    before_m = before_low + before_raw / 65535.0 * (before_high - before_low)
    after_m = after_low + after_raw / 65535.0 * (after_high - after_low)
    ref_cell = args.size_m / max(before_m.shape[0] - 1, 1)

    gn = metrics(gn_m, gn_cell)
    ref_before = metrics(before_m, ref_cell)
    ref_after = metrics(after_m, ref_cell)
    houdini_incision = float((before_m - after_m).mean())

    rows = [
        ("高程 min (m)", gn["min"], ref_after["min"]),
        ("高程 max (m)", gn["max"], ref_after["max"]),
        ("高程 std (m)", gn["std"], ref_after["std"]),
        ("平均坡度 (°)", gn["slope_mean"], ref_after["slope_mean"]),
        ("坡度 p50 (°)", gn["slope_p50"], ref_after["slope_p50"]),
        ("坡度 p95 (°)", gn["slope_p95"], ref_after["slope_p95"]),
        ("曲率 p5", gn["curvature_p5"], ref_after["curvature_p5"]),
        ("曲率 p95", gn["curvature_p95"], ref_after["curvature_p95"]),
        ("粗尺度坡度比", gn["coarse_slope_ratio"], ref_after["coarse_slope_ratio"]),
    ]

    print(f"{'指标':<16}{'VL_HF':>12}{'Houdini':>12}{'比值':>10}")
    print("-" * 52)
    for name, gn_value, ref_value in rows:
        ratio = (gn_value / ref_value) if abs(ref_value) > 1e-9 else float("nan")
        print(f"{name:<16}{gn_value:>12.4f}{ref_value:>12.4f}{ratio:>10.3f}")

    # 侵蚀量：GN 没有单独的 base_height，用基准的 before/after 对比说明量级
    print()
    print(f"Houdini 下切量均值: {houdini_incision:.4f} m")
    print(f"Houdini 侵蚀前 std: {ref_before['std']:.4f} m -> "
          f"侵蚀后 {ref_after['std']:.4f} m")

    if args.json:
        payload = {
            "gn": gn,
            "houdiniBefore": ref_before,
            "houdiniAfter": ref_after,
            "houdiniIncisionMean": houdini_incision,
        }
        args.json.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                             encoding="utf-8")
        print(f"JSON {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

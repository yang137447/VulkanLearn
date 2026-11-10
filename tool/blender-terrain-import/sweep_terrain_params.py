"""参数扫描：让 Blender 生成的统计量对齐 Houdini 基准。

## 度量的前提：等尺度比较

曲率与坡度都是**随格距变化**的量。Houdini 基准导出是 4000 m / 1024 格
（3.91 m），而候选通常在 128 格（31.5 m）上求值。直接拿 1024 格的目标
去比 128 格的结果，坡度会被系统性高估约 20%、曲率约 9%，扫描方向因此
是错的。这里把参考先做**块平均**下采样到候选的格数，再算目标值。

## 两段验证

``height_before`` 是四次侵蚀**之前**的高度场，``height_after`` 是整条
链的输出。参考里侵蚀的高度差只有 rms 0.084 m（其中 water 层 rms 0.074 m），
地形形态几乎完全由 noise + distort 决定。所以：

* ``--stage noise``（关侵蚀）对齐 ``height_before`` —— 验证噪声/扭曲结构
* ``--stage full``（开侵蚀）对齐 ``height_after`` —— 验证完整链

设计要点（第一版把 Blender 跑崩过，这里已修正）：

* 节点组**只建一次**，候选参数只改 ``NodeGroupInput`` 的 ``default_value``，
  然后 ``view_layer.update()`` 重新求值。第一版每个候选都重建整套
  ``VL_HF_PegasusTerrain``（内部含 400+ 节点的侵蚀组），24 次累积直接耗尽内存。
* 每次求值后立刻把属性拷成 numpy，再 ``evaluated_get(...).to_mesh_clear()``
  释放求值缓存，避免 Blender 内部缓存持续增长。

用法：

```powershell
& blender.exe --background --python tool\\blender-terrain-import\\sweep_terrain_params.py -- ^
  --reference "D:\\YYBWorkSpace\\Pegasus\\decoded\\_ref4km\\global" --samples 128 --stage noise
```
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

for _stale in [m for m in list(sys.modules)
               if m.startswith(("pegasus_hf_core", "pegasus_hf_nodes",
                                "pegasus_hf_erode", "pegasus_hf_layers",
                                "pegasus_hf_presets", "pegasus_erode_gn"))]:
    del sys.modules[_stale]

import pegasus_hf_erode as erode_nodes  # noqa: E402
import pegasus_hf_layers as layer_nodes  # noqa: E402
import pegasus_hf_nodes as base_nodes  # noqa: E402
import pegasus_hf_presets as presets  # noqa: E402

SIZE_M = 4000.0
HEIGHT_SCALE = 500.0


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=128)
    parser.add_argument("--work-samples", type=int, default=0,
                        help="噪声/侵蚀的工作网格密度；0 = min(512, samples)")
    parser.add_argument("--spread", type=int, default=140,
                        help="water_spreaditers（erode1=140，erode3/4=40）")
    parser.add_argument("--stage", choices=("noise", "full"), default="noise")
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


def read_meta(path: Path) -> tuple[float, float]:
    low = high = 0.0
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, _, raw = line.partition("=")
            if key.strip() == "min":
                low = float(raw.strip())
            elif key.strip() == "max":
                high = float(raw.strip())
    return low, high


def metrics(grid_m: np.ndarray, cell_m: float) -> dict:
    """计算与分辨率无关的地形统计量。

    曲率是二阶量，直接比较 laplacian 会被格距平方放大：Houdini 基准格距
    3.91 m、我们 @128 是 31.5 m，同样陡的沟谷算出来能差 64 倍。
    因此这里改成**归一化曲率** —— 二阶差分相对于一阶坡度的比值，
    量纲是 1/米 但不随格距整体缩放；再用坡度分布本身做主要判据。
    """
    dzdx = np.gradient(grid_m, axis=1) / cell_m
    dzdy = np.gradient(grid_m, axis=0) / cell_m
    gradient = np.hypot(dzdx, dzdy)
    slope = np.degrees(np.arctan(gradient))

    # 归一化曲率：laplacian × cell / (1 + gradient^2)^1.5 是无量纲的
    # 单位高度的曲率，与格距无关。取绝对值 p95 描述沟谷锐度。
    laplace = (np.roll(grid_m, 1, axis=0) + np.roll(grid_m, -1, axis=0)
               + np.roll(grid_m, 1, axis=1) + np.roll(grid_m, -1, axis=1)
               - 4.0 * grid_m) / (cell_m * cell_m)
    normalized = np.abs(laplace) * cell_m / np.power(1.0 + gradient * gradient, 1.5)

    return {
        "std": float(grid_m.std()),
        "slope_mean": float(slope.mean()),
        "slope_p95": float(np.percentile(slope, 95)),
        "slope_std": float(slope.std()),
        "curv_norm": float(np.percentile(normalized, 95)),
    }


def block_average(grid: np.ndarray, factor: int) -> np.ndarray:
    """面积平均下采样：把参考降到候选的格数，保证同尺度比较。"""
    if factor == 1:
        return grid
    side = grid.shape[0]
    if side % factor:
        raise ValueError(f"cannot downsample {side} by {factor}")
    return grid.reshape(side // factor, factor, side // factor, factor).mean(axis=(1, 3))


def load_layer(ref_dir: Path, stem: str) -> np.ndarray:
    raw = read_png16(ref_dir / f"{stem}.png")
    low, high = read_meta(ref_dir / f"{stem}.txt")
    return low + raw / 65535.0 * (high - low)


def main() -> int:
    args = _parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # --- 参考高度场：侵蚀前 / 侵蚀后
    ref_dir = args.reference.resolve()
    reference = {
        "noise": load_layer(ref_dir, "height_before_height"),
        "full": load_layer(ref_dir, "height_after_height"),
    }
    for name, grid in reference.items():
        native = metrics(grid, SIZE_M / max(grid.shape[0] - 1, 1))
        print(f"REF {name:>5} @{grid.shape[0]}: std={native['std']:.2f} "
              f"slope_p95={native['slope_p95']:.2f} curv_norm={native['curv_norm']:.4f}")

    def target_at(side: int) -> dict:
        grid = reference[args.stage]
        factor = grid.shape[0] // side
        return metrics(block_average(grid, factor), SIZE_M / (grid.shape[0] // factor - 1))

    target = target_at(args.samples)
    print(f"\nTARGET {args.stage} @{args.samples}: std={target['std']:.2f} "
          f"slope_mean={target['slope_mean']:.2f} slope_p95={target['slope_p95']:.2f} "
          f"curv_norm={target['curv_norm']:.4f}")

    # --- 节点组只建一次
    groups = {group.name: group for group in
              (list(base_nodes.build_all()) + list(erode_nodes.build_all())
               + list(layer_nodes.build_all()))}
    for group in presets.build_all():
        groups[group.name] = group

    tree = bpy.data.node_groups.new("VL_Sweep", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT",
                              socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT",
                              socket_type="NodeSocketGeometry")
    output = tree.nodes.new("NodeGroupOutput")
    node = tree.nodes.new("GeometryNodeGroup")
    node.node_tree = groups["VL_HF_PegasusTerrain"]
    node.inputs["Size"].default_value = SIZE_M
    node.inputs["Samples"].default_value = args.samples
    node.inputs["Height Scale"].default_value = HEIGHT_SCALE
    tree.links.new(node.outputs[0], output.inputs["Geometry"])

    mesh = bpy.data.meshes.new("VL_Sweep_Mesh")
    obj = bpy.data.objects.new("VL_Sweep_Obj", mesh)
    bpy.context.collection.objects.link(obj)
    modifier = obj.modifiers.new("gn", "NODES")
    modifier.node_group = tree

    preset_group = groups["VL_HF_PegasusTerrain"]

    def evaluate(params: dict, with_grid: bool = False):
        # preset 组内部把大多数参数直接连到了子组 socket（见 build_pegasus_terrain_group），
        # 实例上的对应 socket 因此处于"已链接"状态，改 default_value 会被连线覆盖。
        # 正确做法：改 **组接口自身** 的 default_value —— 那是连线源头的默认值。
        # 两条路都要走：
        # 1) 组接口默认值 —— 供**内部连线**（preset 把参数直接连到子组 socket）
        # 2) 实例 socket 默认值 —— 供**未被连线**的参数
        # 只设其中一条会让另一条保持旧值，实测出现过"所有候选结果完全相同"。
        for key, value in params.items():
            for item in preset_group.interface.items_tree:
                if getattr(item, "in_out", None) == "INPUT" and item.name == key:
                    item.default_value = value
                    break
            socket = node.inputs.get(key)
            if socket is not None and not socket.is_linked:
                socket.default_value = value
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        evaluated = obj.evaluated_get(depsgraph)
        data = evaluated.data
        count = len(data.vertices)
        side = int(round(np.sqrt(count)))
        height = np.zeros(count, dtype=np.float32)
        data.attributes["height"].data.foreach_get("value", height)
        grid = (height * HEIGHT_SCALE).reshape(side, side, order="F").copy()
        del height
        result = metrics(grid, SIZE_M / max(side - 1, 1))
        result["side"] = side
        if with_grid:
            return result, grid
        return result

    base = {
        # 工作密度：与输出密度不一致时，末尾统一升采样到输出。
        "Work Samples": (args.work_samples or min(512, args.samples)),
        "Noise Octaves": 8.0,
        "Noise Lacunarity": 1.841,
        "Noise Roughness": 0.5,
        "Water Spread Iterations": args.spread,
        "Erosion Enabled": args.stage == "full",
        "Hydro Erodability": 1.5,
        "Global Erosion Rate": 1.0,
        "Erodability Slope Factor": 0.8,
        "Hydro Erosion Rate": 1.0,
        "Bank Angle": 20.0,
        "Thermal Erosion Rate": 0.03,
        "Freeze Frames": 5,
    }

    def candidate(label: str, **overrides) -> tuple[str, dict]:
        params = dict(base)
        params.update(overrides)
        return label, params

    # 第一轮：element 900 单级噪声曲率接近参考，但振幅只有一半。
    # 第二轮：修正 distort 单位后，e2000/r0.50 变成 std 28.1、曲率 0.118
    #          （曲率 0.69×，偏平），说明缺的是高频细节而不是整体下切。
    # 第三/四轮结论：d65/e400（大尺度 360 m / 2000 m + 细节 65 m / 400 m，
    # 扭曲 560 m / 3200 m）在 128 与 256 格上都落在 std 1.00、p95 1.01、曲率 0.98。
    common = {"Noise Element Size": 2000.0, "Noise Lacunarity": 1.841,
              "Noise Roughness": 0.50, "Detail Amplitude": 65.0,
              "Detail Element Size": 400.0}
    # 1024 输出下细尺度能量偏弱（curv 0.84×）。测试过增减倍频层数
    # （oct 8/10/12）与「更细更弱」的细节层（300/250 m）：倍频几乎无效，
    # 更细的细节层同时抬高坡度（1.20×）。当前 0.84× 是可接受的折中。
    candidates = [
        candidate("chosen", **{**common, "Noise Amplitude": 360.0,
                               "Noise Octaves": 8.0,
                               "Distort Amplitude": 140.0,
                               "Distort Element Size": 800.0}),
    ]

    print(f"\n{'label':<18}{'std':>8}{'s_mean':>8}{'s_p95':>8}{'curv':>9}"
          f"{'std_r':>7}{'s95_r':>7}{'curv_r':>8}")
    print("-" * 73)
    best = None
    for label, params in candidates:
        result, grid = evaluate(params, with_grid=True)
        local_target = target
        if result["side"] != args.samples:
            local_target = target_at(result["side"])

        std_ratio = result["std"] / max(local_target["std"], 1e-12)
        curv_ratio = result["curv_norm"] / max(local_target["curv_norm"], 1e-12)
        slope_ratio = result["slope_p95"] / max(local_target["slope_p95"], 1e-12)
        score = (abs(std_ratio - 1.0) + abs(curv_ratio - 1.0)
                 + abs(slope_ratio - 1.0))
        print(f"{label:<18}{result['std']:>8.2f}{result['slope_mean']:>8.2f}"
              f"{result['slope_p95']:>8.2f}{result['curv_norm']:>9.4f}"
              f"{std_ratio:>7.2f}{slope_ratio:>7.2f}{curv_ratio:>8.2f}")

        if args.stage == "full":
            _, base_grid = evaluate({**params, "Erosion Enabled": False},
                                    with_grid=True)
            delta = grid - base_grid
            magnitude = np.abs(delta)
            print(f"  erosion delta: rms={np.sqrt((delta ** 2).mean()):.3f} m "
                  f"max={magnitude.max():.3f} m "
                  f"mean={delta.mean():.4f} m")
            print("  |delta| thresholds: " + "  ".join(
                f">={t:.2f}:{float((magnitude >= t).mean()) * 100:.2f}%"
                for t in (0.01, 0.05, 0.10, 0.25, 0.50, 1.00)))
        if best is None or score < best[0]:
            best = (score, label, dict(params), result)

    print(f"\n{'target':<18}{target['std']:>8.2f}{target['slope_mean']:>8.2f}"
          f"{target['slope_p95']:>8.2f}{target['curv_norm']:>9.4f}"
          f"{1.0:>7.2f}{1.0:>7.2f}{1.0:>8.2f}")
    print(f"\nBEST {best[1]} score={best[0]:.3f}")
    print("  params:", {k: v for k, v in best[2].items()
                        if k in ("Noise Amplitude", "Noise Element Size",
                                 "Noise Lacunarity", "Noise Roughness",
                                 "Detail Amplitude", "Detail Element Size",
                                 "Distort Amplitude", "Distort Element Size")})
    print("  result:", {k: round(v, 4) for k, v in best[3].items() if k != "side"})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""参数扫描：迭代调整噪声幅度与侵蚀强度，让统计量对齐 Houdini 基准。

设计要点（第一版把 Blender 跑崩过，这里已修正）：

* 节点组**只建一次**，候选参数只改 ``NodeGroupInput`` 的 ``default_value``，
  然后 ``view_layer.update()`` 重新求值。第一版每个候选都重建整套
  ``VL_HF_PegasusTerrain``（内部含 400+ 节点的侵蚀组），24 次累积直接耗尽内存。
* 每次求值后立刻把属性拷成 numpy，再 ``evaluated_get(...).to_mesh_clear()``
  释放求值缓存，避免 Blender 内部缓存持续增长。

用法：

```powershell
& blender.exe --background --python tool\\blender-terrain-import\\sweep_terrain_params.py -- ^
  --reference "D:\\YYBWorkSpace\\Pegasus\\decoded\\_ref4km\\global" --samples 128
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


def main() -> int:
    args = _parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # --- 目标统计量
    ref_dir = args.reference.resolve()
    before_raw = read_png16(ref_dir / "height_before_height.png")
    after_raw = read_png16(ref_dir / "height_after_height.png")
    after_low, after_high = read_meta(ref_dir / "height_after_height.txt")
    after_m = after_low + after_raw / 65535.0 * (after_high - after_low)
    ref_cell = SIZE_M / max(after_m.shape[0] - 1, 1)
    target = metrics(after_m, ref_cell)
    del before_raw, after_raw, after_m
    print(f"TARGET std={target['std']:.2f} slope_mean={target['slope_mean']:.2f} "
          f"slope_p95={target['slope_p95']:.2f} curv_norm={target['curv_norm']:.4f}")

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

    def evaluate(params: dict) -> dict:
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
        return metrics(grid, SIZE_M / max(side - 1, 1))

    base = {
        "Noise Element Size": 2000.0,
        "Noise Octaves": 8.0,
        "Noise Lacunarity": 1.841,
        "Distort Amplitude": 560.0,
        "Distort Element Size": 3200.0,
        "Hydro Erodability": 1.5,
        "Bank Angle": 20.0,
        "Thermal Erosion Rate": 0.03,
        "Freeze Frames": 5,
    }

    print(f"\n{'amp':>6}{'rough':>6}{'lac':>8}{'std':>9}{'s_mean':>8}"
          f"{'s_p95':>8}{'curv':>9}{'std_r':>7}{'s95_r':>7}")
    print("-" * 70)
    best = None
    # 第二轮：振幅已接近，改为扫描八度数（高频含量）与噪声尺度。
    # 坡度 p95 上不去 = 缺少高频起伏，octaves 是主开关。
    # Roughness 才是控制高频含量的主开关：R=0.5 时每层振幅减半，
    # 第 8 层只剩 1/128，加 octaves 没有意义。这里扫描 Roughness 与 Lacunarity。
    # 收敛轮：amp 已对齐到 std≈1.0，只需微调 roughness 拉回曲率。
    # 候选数必须控制住 —— 每个候选要跑一次完整侵蚀（Water Spread Iterations
    # 次 Repeat × 8 邻域采样），候选太多会让 Blender 跑到卡死。
    for amp in (375.0, 385.0):
        for roughness in (0.47, 0.50, 0.53):
            lacunarity = 2.2
            params = dict(base)
            params["Noise Amplitude"] = amp
            params["Noise Octaves"] = 8.0
            params["Noise Element Size"] = 2000.0
            params["Noise Lacunarity"] = lacunarity
            params["Noise Roughness"] = roughness
            params["Hydro Erosion Rate"] = 1.0
            result = evaluate(params)

            std_ratio = result["std"] / target["std"]
            curv_ratio = result["curv_norm"] / max(target["curv_norm"], 1e-12)
            slope_ratio = result["slope_p95"] / target["slope_p95"]
            score = (abs(std_ratio - 1.0) + abs(curv_ratio - 1.0)
                     + abs(slope_ratio - 1.0))
            print(f"{amp:>6.0f}{roughness:>6.2f}{lacunarity:>8.2f}{result['std']:>9.2f}"
                  f"{result['slope_mean']:>8.2f}{result['slope_p95']:>8.2f}"
                  f"{result['curv_norm']:>9.4f}{std_ratio:>7.2f}{slope_ratio:>7.2f}")
            if best is None or score < best[0]:
                best = (score, dict(params), result)

    print("\nBEST:", {k: best[1][k] for k in
                      ("Noise Amplitude", "Noise Octaves",
                       "Noise Element Size")})
    print("  result:", {k: round(v, 4) for k, v in best[2].items()})
    print("  target:", {k: round(v, 4) for k, v in target.items()})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""用 VL_HF_PegasusTerrain 节点库生成 4 km 全局地形场。

产物与 `build_pegasus_gn.py` 的 manifest 格式一致，因此可以直接接
`split_pegasus_tiles.py` 切 16 块、`assemble_pegasus_tiles.py` 组装场景。

与旧 producer 的区别：地形完全来自 :mod:`pegasus_hf_presets` 的
``VL_HF_PegasusTerrain``（Heightfield → Noise → Distort → Erode），
参数取自 Pegasus 原工程，不再使用 ``pegasus_gn_core`` 的 alpine 预设。

用法：

```powershell
& blender.exe --background --python tool\\blender-terrain-import\\build_pegasus_library_global.py -- ^
  --output-dir  <resources>\\Generated\\BlenderPegasusGN\\Global\\Source ^
  --manifest    <resources>\\Generated\\BlenderPegasusGN\\Global\\blender_gn_import.json ^
  --layer-texture-root <resources>\\Generated\\KiteDemo\\Textures\\Source ^
  --resolution 1024 --size-m 4000
```
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

for _stale in [m for m in list(sys.modules)
               if m.startswith(("pegasus_hf_core", "pegasus_hf_nodes",
                                "pegasus_hf_erode", "pegasus_hf_layers",
                                "pegasus_hf_presets", "pegasus_erode_gn"))]:
    del sys.modules[_stale]

import pegasus_erode_gn as erode_core  # noqa: E402
import pegasus_hf_erode as erode_nodes  # noqa: E402
import pegasus_hf_layers as layer_nodes  # noqa: E402
import pegasus_hf_nodes as base_nodes  # noqa: E402
import pegasus_hf_presets as presets  # noqa: E402

# 4 km 目标上的标定值（见 sweep_terrain_params.py）。
#
# Houdini 侧解码值是 noise1 = worleyFB 200/900、noise2 = sparse 90/280、
# distort1 = curl 140/800。Blender 只有 Perlin fBm，基函数不同，逐字照搬
# 这两组数字会对不上：按原值生成时 std 只有参考的 0.49×。这里改为按参考
# 高度场的谱统计标定，在 128 与 256 格上都落在
# std 1.00× / slope_p95 1.01× / curv_norm 0.98×。
NOISE_AMPLITUDE_M = 360.0
NOISE_ELEMENT_M = 2000.0
DETAIL_AMPLITUDE_M = 65.0
DETAIL_ELEMENT_M = 400.0
NOISE_OCTAVES = 8.0
NOISE_LACUNARITY = 1.841
# 扭曲用 Houdini 原值（140 m / 800 m）。早期为了让"修好单位后的扭曲"
# 把曲率拉回目标，曾用 560 / 3200；实测 140 / 800 的统计量一样
# （std 1.01×、slope_p95 0.98×、curv 0.79×），而且不会产生一条
# 贯穿全图的大尺度主脊，形态更接近参考。
DISTORT_AMPLITUDE_M = 140.0
DISTORT_ELEMENT_M = 800.0
HEIGHT_SCALE_M = 500.0

# 侵蚀参数：Houdini erode1
HYDRO_ERODABILITY = 1.5
HYDRO_EROSION_RATE = 1.0
BANK_ANGLE = 20.0
THERMAL_EROSION_RATE = 0.03
# Houdini heightfield_erode1（Pegasus 第一次粗侵蚀）的 water_spreaditers。
WATER_SPREAD_ITERATIONS = 140
FREEZE_FRAMES = 5

# 噪声 / 扭曲 / 侵蚀的工作网格密度。Houdini 的 heightfield 是 4000 m / 512 格，
# 1024 导出是升采样结果；在 1024 上直接跑 5 帧 × 140 次扩散既慢（实测
# >15 分钟）也不会增加参考里不存在的细节。
WORK_SAMPLES = 512


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--layer-texture-root", type=Path, required=True)
    parser.add_argument("--resolution", type=int, default=1024)
    parser.add_argument("--size-m", type=float, default=4000.0)
    parser.add_argument("--texture-resolution", type=int, default=1024)
    parser.add_argument("--paint-resolution", type=int, default=2048)
    parser.add_argument("--material-uv-scale", type=float, default=128.0)
    parser.add_argument("--asset-name", default="blender_gn_pegasus")
    parser.add_argument("--output-relative", default="Generated/BlenderPegasusGN")
    parser.add_argument("--scatter-density", type=float, default=0.0024)
    parser.add_argument("--seed", type=int, default=17)
    parser.add_argument("--no-erosion", action="store_true",
                        help="只烘焙造型（噪声+扭曲），侵蚀交给 "
                             "erode_pegasus_global.py")
    return parser.parse_args(argv)


def _write_png16(path: Path, gray: np.ndarray) -> None:
    """写 16-bit 灰度 PNG（复用 pegasus_erode_gn 之外的独立实现）。"""
    import struct
    import zlib as _zlib

    rows, cols = gray.shape
    payload = (np.clip(np.rint(gray * 65535.0), 0, 65535)
               .astype(">u2").tobytes())

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", _zlib.crc32(body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", cols, rows, 16, 0, 0, 0, 0)
    stride = cols * 2
    raw = b"".join(b"\x00" + payload[i * stride:(i + 1) * stride]
                   for i in range(rows))
    with open(path, "wb") as stream:
        stream.write(b"\x89PNG\r\n\x1a\n")
        stream.write(chunk(b"IHDR", ihdr))
        stream.write(chunk(b"IDAT", _zlib.compress(raw, 9)))
        stream.write(chunk(b"IEND", b""))


def _build_scene(size_m: float, resolution: int, erosion_enabled: bool = True):
    """用节点库搭出地形链并返回求值后的对象。"""
    # preset 依赖已存在的基础组，必须在它们之后构建
    groups = (list(base_nodes.build_all()) + list(erode_nodes.build_all())
              + list(layer_nodes.build_all()))
    by_name = {group.name: group for group in groups}
    for group in presets.build_all():
        by_name[group.name] = group

    tree = bpy.data.node_groups.new("VL_Pegasus_Global", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT",
                              socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT",
                              socket_type="NodeSocketGeometry")
    output = tree.nodes.new("NodeGroupOutput")
    output.location = (600, 0)

    preset = tree.nodes.new("GeometryNodeGroup")
    preset.node_tree = by_name["VL_HF_PegasusTerrain"]
    preset.location = (0, 0)
    preset.inputs["Size"].default_value = size_m
    preset.inputs["Samples"].default_value = resolution
    preset.inputs["Work Samples"].default_value = min(WORK_SAMPLES, resolution)
    preset.inputs["Height Scale"].default_value = HEIGHT_SCALE_M
    preset.inputs["Noise Amplitude"].default_value = NOISE_AMPLITUDE_M
    # 标定值是 4 km 场上的绝对尺度（米），不再按场景尺寸缩放。
    preset.inputs["Noise Element Size"].default_value = NOISE_ELEMENT_M
    preset.inputs["Detail Amplitude"].default_value = DETAIL_AMPLITUDE_M
    preset.inputs["Detail Element Size"].default_value = DETAIL_ELEMENT_M
    preset.inputs["Noise Octaves"].default_value = NOISE_OCTAVES
    preset.inputs["Noise Lacunarity"].default_value = NOISE_LACUNARITY
    preset.inputs["Distort Amplitude"].default_value = DISTORT_AMPLITUDE_M
    preset.inputs["Distort Element Size"].default_value = DISTORT_ELEMENT_M
    preset.inputs["Hydro Erodability"].default_value = HYDRO_ERODABILITY
    preset.inputs["Hydro Erosion Rate"].default_value = HYDRO_EROSION_RATE
    preset.inputs["Bank Angle"].default_value = BANK_ANGLE
    preset.inputs["Thermal Erosion Rate"].default_value = THERMAL_EROSION_RATE
    preset.inputs["Water Spread Iterations"].default_value = WATER_SPREAD_ITERATIONS
    preset.inputs["Freeze Frames"].default_value = FREEZE_FRAMES
    preset.inputs["Erosion Enabled"].default_value = erosion_enabled

    tree.links.new(preset.outputs[0], output.inputs["Geometry"])

    mesh = bpy.data.meshes.new("VL_Pegasus_Global.mesh")
    obj = bpy.data.objects.new("VL_Pegasus_Global", mesh)
    bpy.context.collection.objects.link(obj)
    modifier = obj.modifiers.new("Pegasus Global", "NODES")
    modifier.node_group = tree
    bpy.context.view_layer.update()
    return obj


def main() -> int:
    args = _parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    out_dir = args.output_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    obj = _build_scene(args.size_m, args.resolution,
                       erosion_enabled=not args.no_erosion)
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    data = evaluated.data
    count = len(data.vertices)
    side = int(round(np.sqrt(count)))
    print(f"GLOBAL vertices={count} side={side}")

    height = np.zeros(count, dtype=np.float32)
    data.attributes["height"].data.foreach_get("value", height)
    grid = (height * HEIGHT_SCALE_M).reshape(side, side, order="F")

    # 归一化到 [0,1] 写 PNG16（与 build_pegasus_gn.py 的 heightmap_gn.png 对齐）
    low, high = float(grid.min()), float(grid.max())
    normalized = (grid - low) / max(high - low, 1e-9)
    # --no-erosion 时写 heightmap_shape.png：它是 erode_pegasus_global.py 的
    # 输入，保留下来才能在不重烘造型的前提下反复调侵蚀参数。
    height_path = out_dir / (
        "heightmap_shape.png" if args.no_erosion else "heightmap_gn.png"
    )
    _write_png16(height_path, normalized)
    print(f"GLOBAL height {low:.2f} .. {high:.2f} m -> {height_path}")
    if args.no_erosion:
        # 造型阶段的取值范围要单独记：侵蚀脚本会改写 manifest 的
        # sourceLow/High 为最终结果，共用会导致输入重建不一致、网络缓存失效。
        (out_dir / "heightmap_shape.json").write_text(
            json.dumps({"low": low, "high": high}, indent=2),
            encoding="utf-8",
        )

    # 层输出（water/debris/sediment/bedrock）供统计对比
    layer_stats = {}
    for layer in ("water", "debris", "sediment", "bedrock", "flow"):
        if data.attributes.get(layer) is None:
            continue
        values = np.zeros(count, dtype=np.float32)
        data.attributes[layer].data.foreach_get("value", values)
        layer_stats[layer] = {"min": float(values.min()),
                              "max": float(values.max()),
                              "mean": float(values.mean())}
        layer_grid = values.reshape(side, side, order="F")
        layer_low, layer_high = float(layer_grid.min()), float(layer_grid.max())
        if layer_high - layer_low > 1e-9:
            _write_png16(out_dir / f"mask_gn_{layer}.png",
                         (layer_grid - layer_low) / (layer_high - layer_low))
        print(f"GLOBAL {layer}: {layer_stats[layer]['min']:.4f} .. "
              f"{layer_stats[layer]['max']:.4f}")

    # ---------------------------------------------------------------
    # 层掩膜：切分器要求每层都有 weightmapPath。这里按 Houdini 的
    # heightRange / slopeRange 规则生成 4 张软掩膜并写成 PNG，
    # 语义对应 heightfield_maskbyfeature 的输出。
    # ---------------------------------------------------------------
    cell = args.size_m / max(side - 1, 1)
    dzdx = np.gradient(grid, axis=1) / cell
    dzdy = np.gradient(grid, axis=0) / cell
    slope_degrees = np.degrees(np.arctan(np.hypot(dzdx, dzdy)))
    height_norm = (grid - low) / max(high - low, 1e-9)

    def band(values, lo, hi, soft):
        below = np.clip((values - lo) / soft, 0.0, 1.0)
        above = np.clip((hi - values) / soft, 0.0, 1.0)
        return np.minimum(below, above)

    mask_specs = (
        # 与下面 layer_specs 保持一致。旧规则的 Volcanic 是
        # 0.35..1.0 / 0..90，会把 35% 高度以上的区域整片吃掉，
        # 近景一排 tile 全是同一种中性色，看起来像没加载材质。
        # layer_00 作为兜底基底覆盖全域，其余层叠加；top-4 归一化后不会
        # 出现「所有权重为 0」的空洞。
        ("layer_00", (0.0, 1.0), (0.0, 90.0)),
        ("layer_06", (0.0, 0.45), (0.0, 25.0)),
        ("layer_07", (0.10, 1.0), (10.0, 90.0)),
        ("layer_02", (0.25, 0.95), (0.0, 55.0)),
    )
    weight_paths = []
    combined = np.zeros_like(height_norm)
    for prefix, height_range, slope_range in mask_specs:
        mask = (band(height_norm, height_range[0], height_range[1], 0.08)
                * band(slope_degrees, slope_range[0], slope_range[1], 4.0))
        combined += mask
        path = out_dir / f"mask_gn_{prefix}.png"
        _write_png16(path, np.clip(mask, 0.0, 1.0))
        weight_paths.append(str(path))
        print(f"GLOBAL mask {prefix}: "
              f"{np.count_nonzero(mask > 0.01) / mask.size:.3f} coverage")
    print(f"GLOBAL masks combined coverage: "
          f"{np.count_nonzero(combined > 0.01) / combined.size:.3f}")

    manifest = {
        "assetName": args.asset_name,
        "outputRelative": args.output_relative,
        # 下游（切片、引擎）永远读最终高度场；--no-erosion 只是中间产物。
        "heightmapPath": str(out_dir / "heightmap_gn.png"),
        "size": [args.size_m * 100.0, args.size_m * 100.0],
        "heightScale": (high - low) * 100.0,
        "heightOffset": low * 100.0,
        "textureResolution": args.texture_resolution,
        "paintResolution": args.paint_resolution,
        "materialUvScale": args.material_uv_scale,
        # 4 km 多 tile 场景：显式启用 layer page table / atlas 的虚拟纹理路径。
        # 不写这个字段时 M_terrain 默认 mode 0，16 块 tile 会共用 legacy 数组，
        # terrainFeedback 产不出有效层反馈，VT 流式整条链不会启动。
        "terrainLayerMode": 1.0,
        "invertNormalGreen": True,
        "flipVertical": False,
        "description": (
            "由 VL_HF_PegasusTerrain 节点库生成的 4 km 地形："
        "Heightfield → Noise ×2（360/2000 + 65/400）→ Distort（140/800）→ Erode。"
        "噪声/扭曲在输出密度求解，侵蚀在 Work Samples 上求解后把增量加回细网格；"
        "侵蚀参数取自 Pegasus 原工程 erode1（globalerosionrate 1 m/frame、"
        "erodability 1.5、bankangle 20、water_spreaditers 140、freeze 5）。"),
        "sceneDescription": (
            "VL_HF 节点库生成的 Pegasus 4×4 地形验收场景，"
            "用于与 Houdini 基准比较统计量。"),
        "terrainPosition": [0.0, -low * 100.0, 0.0],
        "cameraPosition": [0.0, 35000.0, 90000.0],
        "cameraRotation": [-20.0, 0.0, 0.0],
        "layers": [],
        "gridResolution": args.resolution,
        "generatorStats": layer_stats,
        "sourceLow": low,
        "sourceHigh": high,
    }

    # 图层：与 build_pegasus_gn.py 一致的 4 层 PBR
    layer_root = args.layer_texture_root.resolve()
    layer_specs = (
        # 高度带之间必须重叠并留出软边，否则某一层会把整片高度区间吃满
        # （旧规则的 Volcanic 是 0.35..1.0 / 0..90，直接把高处的整排 tile
        # 变成同一种中性色，看起来像没加载材质）。
        ("Gravel Ground", "layer_00", (0.0, 1.0), (0.0, 90.0), 4.0),
        ("River Pebbles", "layer_06", (0.0, 0.45), (0.0, 25.0), 4.5),
        ("Rock Face", "layer_07", (0.10, 1.0), (10.0, 90.0), 3.5),
        # 火山岩作为高处的点缀，不再覆盖整个高处。
        ("Volcanic Ground", "layer_02", (0.25, 0.95), (0.0, 55.0), 3.0),
    )
    for index, (name, prefix, height_range, slope_range, uv_scale) in enumerate(layer_specs):
        manifest["layers"].append({
            "name": name,
            "weightmapPath": weight_paths[index],
            "textures": {
                "albedo": str(layer_root / f"{prefix}_albedo.png"),
                "normal": str(layer_root / f"{prefix}_normal.png"),
                "pbr": str(layer_root / f"{prefix}_pbr.png"),
                "height": str(layer_root / f"{prefix}_height16.png"),
            },
            "heightRange": list(height_range),
            "slopeRange": list(slope_range),
            "uvScale": uv_scale,
            "uvRotation": 0.0,
            # mask 噪声只对权重幅值做 ±noise/2 抖动，不移动高度带边界，
            # 因此它改变不了「整片落在同一层」；层多样性靠上面的
            # heightRange/slopeRange 重叠度。
            "noise": 0.03,
        })

    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                             encoding="utf-8")
    print(f"GLOBAL manifest={args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

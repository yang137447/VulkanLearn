"""Pegasus Erode 2.0 复刻的 Blender 入口：建节点组、生成 .blend。

```powershell
& blender.exe --background --python tool\\blender-terrain-import\\build_pegasus_erode.py -- `
  --blend artifacts\\pegasus\\erode\\erode.blend --resolution 128
```

参数默认值取自 Houdini 原工程的 `heightfield_erode1`（Pegasus 第一段侵蚀）。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))

for _stale in [m for m in list(sys.modules) if m.startswith("pegasus_erode_gn")]:
    del sys.modules[_stale]

import pegasus_erode_gn as erode  # noqa: E402

# 名字、类型、默认、最小、最大、说明。默认值 = Houdini erode1 的真实参数。
INPUTS = [
    ("Height Scale", "NodeSocketFloat", 100.0, 1.0, 5000.0,
     "高度单位换算（米）。Houdini heightfield 的 size.y。"),
    ("Cell Size", "NodeSocketFloat", 4.0, 0.01, 1000.0,
     "格距（米）。由 size_m / (resolution - 1) 自动设定。"),
    ("Rain Amount", "NodeSocketFloat", 0.9, 0.0, 5.0,
     "hydro rain_amount：每帧降水量。过大反而整片淹没不侵蚀。"),
    ("Rain Density", "NodeSocketFloat", 0.05, 0.001, 1.0,
     "hydro rain_density：雨点疏密。低=窄河道，高=宽河道。"),
    ("Rain Evaporation", "NodeSocketFloat", 0.04, 0.0, 1.0,
     "蒸发量（蒸发节点 rain_amount = -0.04）。"),
    ("Water Spread Iterations", "NodeSocketInt", 40, 0, 200,
     "hydro water_spreaditers：水扩散迭代数。多=更长更稀疏的切槽。"),
    ("Water Spread Rate", "NodeSocketFloat", 0.55, 0.0, 1.0,
     "每次扩散的传递比例。"),
    ("Hydro Erodability", "NodeSocketFloat", 1.1, 0.0, 5.0,
     "hydro hydro_erodability：可蚀性（岩层软硬）。"),
    ("Hydro Erosion Rate", "NodeSocketFloat", 1.0, 0.0, 5.0,
     "hydro hydro_erosionrate：可蚀性的乘数。erode1=1, erode3/4=0.4。"),
    ("Bank Angle", "NodeSocketFloat", 20.0, 0.0, 90.0,
     "hydro hydro_bankangle：河道横截面坡角。20=窄深，70=宽缓。"),
    ("Removal Rate", "NodeSocketFloat", 0.1, 0.0, 1.0,
     "hydro hydro_removalrate：物质被彻底带走的比例，唯一让山变矮的量。"),
    ("Max Debris Depth", "NodeSocketFloat", 5.0, 0.0, 50.0,
     "hydro hydro_maxdebrisdepth。"),
    ("Bed Erosion Factor", "NodeSocketFloat", 1.0, 0.0, 5.0,
     "hydro bed_erosionratefactor：河床乘数。"),
    ("Bank Erosion Factor", "NodeSocketFloat", 1.0, 0.0, 5.0,
     "hydro bank_erosionratefactor：河岸乘数。"),
    ("Bank Water Ratio", "NodeSocketFloat", 0.5, 0.0, 5.0,
     "hydro bank_maxbankbedwaterratio：水柱高度比超过它判为河床。"),
    ("Sediment Capacity", "NodeSocketFloat", 10.0, 0.0, 100.0,
     "hydro bed_sedimentcap：单位水流挟沙量，超出即沉积。"),
    ("Bed Deposition Rate", "NodeSocketFloat", 0.01, 0.0, 1.0,
     "hydro bed_depositionrate：多余沉积物转 debris 的速率。"),
    ("Thermal Erosion Rate", "NodeSocketFloat", 0.03, 0.0, 1.0,
     "thermal erosionrate。0 = 关闭该段（erode4 就是 0 之外的关闭）。"),
    ("Thermal Removal Rate", "NodeSocketFloat", 0.7, 0.0, 1.0,
     "thermal removalrate。erode4 段 thermal_erodability=0 时整体关闭。"),
    ("Repose Angle", "NodeSocketFloat", 15.0, 0.0, 89.0,
     "debris_reposeangle：碎屑休止角（沙 30–45°，水 0）。"),
    ("Cut Angle", "NodeSocketFloat", 35.0, 0.0, 89.0,
     "thermal cutangle。"),
]


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blend", type=Path, default=None)
    parser.add_argument("--resolution", type=int, default=128)
    parser.add_argument("--size-m", type=float, default=1000.0)
    parser.add_argument("--iterations", type=int, default=1,
                        help="外层帧数（Houdini freezeframe）")
    return parser.parse_args(argv)


def build_scene(size_m: float, resolution: int, iterations: int):
    """建对象 + 挂节点组，并在组内部生成输入地形。

    节点组自带 Geometry 输入/输出，所以地形必须在**组内部**生成：
    先建组，再往组里加网格与初始层，最后接到组的 Geometry 输出上游。
    返回 (组, 对象)。
    """
    specification = list(INPUTS)
    cell = size_m / max(resolution - 1, 1)
    specification = [
        (name, kind, (cell if name == "Cell Size" else default), low, high,
         description)
        for name, kind, default, low, high, description in specification]

    group = erode.build_erode_group(size_m, resolution, specification, iterations)
    print(f"ERODE group={group.name} nodes={len(group.nodes)}")

    tree = group
    grid = tree.nodes.new("GeometryNodeMeshGrid")
    grid.name = "VL_Input_Grid"
    grid.inputs["Size X"].default_value = size_m
    grid.inputs["Size Y"].default_value = size_m
    grid.inputs["Vertices X"].default_value = resolution
    grid.inputs["Vertices Y"].default_value = resolution
    grid.location = (-2400, 2600)

    # 初始地形：两层噪声，对应 Houdini 的 heightfield_noise1（amp 200、
    # element 900）+ heightfield_noise2（amp 90、element 280）。
    #
    # 注意尺度：Blender 的 Noise.Scale 是"每单位长度的重复次数"，而网格边长是
    # size_m 米。用 scale = size_m / element 才能得到与 Houdini element 对应的
    # 波长。早期把 scale 固定成 3.0，在 1 km 场景上等于 330 m 波长，结果全是
    # 高频碎屑，与基准的平滑大尺度山体完全不符。
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (-2400, 2400)

    # Blender 的 Noise.Scale 是"世界坐标每单位长度的重复次数"。网格边长是
    # size_m 米，直接用米制坐标时 scale=size_m/element 会让噪声在整张图上
    # 只重复一两次，配合 Detail 就变成逐格纯噪声（实测相邻格差异占总振幅
    # 16%，hillshade 全是碎渣）。所以先把位置归一化到 [0,1] 的 UV，
    # 再让 Scale = size_m / element 表达真实波长。
    normalized = erode.vector_math(tree, "DIVIDE", position.outputs["Position"],
                                   (size_m, size_m, 1.0), (-2250, 2400))
    normalized_xy = erode.combine_xyz(tree, None, None, 0.0, (-2100, 2400))
    # 取 XY 分量、把 Z 归零，避免高度方向影响噪声。
    separate = tree.nodes.new("ShaderNodeSeparateXYZ")
    separate.location = (-2150, 2400)
    tree.links.new(normalized.outputs["Vector"], separate.inputs["Vector"])
    uv = erode.combine_xyz(tree, separate.outputs["X"], separate.outputs["Y"],
                           0.0, (-2000, 2400))

    # UV 归一化到 [0,1] 之后，Noise.Scale 的含义变成"整张图上重复几次"，
    # 所以正确的频率是 size_m / element（场景越大、同一 element 重复越多次），
    # 而不是再乘一次 size_m。早期两次都乘了 size_m，噪声频率被压到接近 1，
    # 逐格差异占总振幅 16%，整张图就是纯噪声。
    large_noise = tree.nodes.new("ShaderNodeTexNoise")
    large_noise.name = "VL_Base_Noise_Large"
    large_noise.location = (-1850, 2500)
    large_noise.noise_dimensions = "4D"
    large_noise.inputs["Scale"].default_value = 1.0
    large_noise.inputs["Detail"].default_value = 3.0
    large_noise.inputs["Roughness"].default_value = 0.45
    large_noise.inputs["W"].default_value = 17.0
    tree.links.new(uv.outputs["Vector"], large_noise.inputs["Vector"])

    small_noise = tree.nodes.new("ShaderNodeTexNoise")
    small_noise.name = "VL_Base_Noise_Small"
    small_noise.location = (-1850, 2280)
    small_noise.noise_dimensions = "4D"
    small_noise.inputs["Scale"].default_value = 2.0
    small_noise.inputs["Detail"].default_value = 2.0
    small_noise.inputs["Roughness"].default_value = 0.5
    small_noise.inputs["W"].default_value = 41.0
    tree.links.new(uv.outputs["Vector"], small_noise.inputs["Vector"])

    # amp 200 : 90 → 权重 0.69 : 0.31
    large_term = erode.math_node(tree, "MULTIPLY", large_noise.outputs["Fac"],
                                 0.69, (-2000, 2500))
    small_term = erode.math_node(tree, "MULTIPLY", small_noise.outputs["Fac"],
                                 0.31, (-2000, 2280))
    combined = erode.math_node(tree, "ADD", large_term.outputs[0],
                               small_term.outputs[0], (-1800, 2400))
    # 居中到 0 附近：height 是归一化层，正负都允许（Houdini 同样如此）。
    #
    # 幅度必须压到"山体总落差 ≈ 1 个单位"的尺度：height 之后要乘 Height Scale
    # （米）。早期直接减 0.5 就交给侵蚀，噪声逐格抖动被放大成陡坡，hillshade
    # 全是碎渣。Houdini 的 heightfield 本身就是连续场，这里补一个低通平滑来
    # 模拟它的连续尺度。
    centered = erode.math_node(tree, "SUBTRACT", combined.outputs[0], 0.5,
                               (-1600, 2400))
    # 低通：再采一遍更低频的噪声，把逐格抖动平滑掉（等价于 heightfield 的
    # 连续插值采样），并按山体高度预算缩放。
    smooth_noise = tree.nodes.new("ShaderNodeTexNoise")
    smooth_noise.name = "VL_Base_Smooth"
    smooth_noise.location = (-2000, 2100)
    smooth_noise.noise_dimensions = "4D"
    smooth_noise.inputs["Scale"].default_value = size_m / 2200.0
    smooth_noise.inputs["Detail"].default_value = 2.0
    smooth_noise.inputs["Roughness"].default_value = 0.5
    smooth_noise.inputs["W"].default_value = 7.0
    tree.links.new(position.outputs["Position"], smooth_noise.inputs["Vector"])
    smooth_centered = erode.math_node(tree, "SUBTRACT",
                                      smooth_noise.outputs["Fac"], 0.5,
                                      (-1800, 2100))
    # 大尺度轮廓 0.75 + 细节 0.25，整体压到 ±0.15（约 ±15 m 的基座起伏，
    # 剩余幅度留给侵蚀去塑造）。
    smooth_scaled = erode.math_node(tree, "MULTIPLY", smooth_centered.outputs[0],
                                    0.30, (-1600, 2100))
    detail_scaled = erode.math_node(tree, "MULTIPLY", centered.outputs[0],
                                    0.08, (-1400, 2400))
    relief = erode.math_node(tree, "ADD", smooth_scaled.outputs[0],
                             detail_scaled.outputs[0], (-1200, 2300))
    centered = relief

    geometry = grid.outputs["Mesh"]
    for index, (layer, value) in enumerate(
            (("height", centered.outputs[0]), ("water", None),
             ("debris", None), ("sediment", None), ("bedrock", None))):
        node = erode.store_attribute(tree, geometry, layer, value,
                                     (-1800 + index * 200, 2400))
        geometry = node.outputs["Geometry"]

    # 把输入地形接到帧循环的入口几何槽（RepeatInput.inputs[1]）。
    consumers = [node for node in tree.nodes
                 if node.bl_idname == "GeometryNodeRepeatInput"]
    if not consumers:
        raise RuntimeError("no RepeatInput found to feed the input terrain")
    frame_in = consumers[0]
    tree.links.new(geometry, frame_in.inputs[1])

    # 把 height 属性变成真实位移，否则视口里是平板、hillshade 全灰。
    # 必须放在帧循环之后：侵蚀改的是 height 属性，位移要读最终值。
    outputs = [node for node in tree.nodes
               if node.bl_idname == "NodeGroupOutput"]
    pattern_output = outputs[0]
    frame_out = next(node for node in tree.nodes
                     if node.bl_idname == "GeometryNodeRepeatOutput")
    final_height = erode.named_attribute(tree, "height", (14200, 600))
    group_input_node = next(node for node in tree.nodes
                            if node.bl_idname == "NodeGroupInput")
    height_m = erode.math_node(tree, "MULTIPLY", final_height,
                               group_input_node.outputs["Height Scale"],
                               location=(14400, 600))
    offset = erode.combine_xyz(tree, 0.0, 0.0, height_m.outputs[0],
                               (14600, 600))
    positioned = erode.set_position(tree, frame_out.outputs[0],
                                    offset.outputs["Vector"], (14700, 300))
    # 断开原来 帧循环 → 输出 的连线，改接位移后的几何。
    for existing in list(pattern_output.inputs["Geometry"].links):
        tree.links.remove(existing)
    tree.links.new(positioned.outputs["Geometry"],
                   pattern_output.inputs["Geometry"])

    mesh = bpy.data.meshes.new("VL_Pegasus_Erode.mesh")
    obj = bpy.data.objects.new("VL_Pegasus_Erode", mesh)
    bpy.context.collection.objects.link(obj)
    modifier = obj.modifiers.new("Pegasus Erode GN", "NODES")
    modifier.node_group = tree
    bpy.context.view_layer.update()
    return tree, obj


def main() -> int:
    args = _parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    group, obj = build_scene(args.size_m, args.resolution, args.iterations)

    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    print(f"ERODE vertices={len(evaluated.data.vertices)} "
          f"attributes={[a.name for a in evaluated.data.attributes]}")

    if args.blend:
        args.blend.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.blend.resolve()))
        print(f"ERODE blend={args.blend}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

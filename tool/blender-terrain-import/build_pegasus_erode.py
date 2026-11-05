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

# ---------------------------------------------------------------- 基座常量
# 全部来自 hython 直读原工程 `/obj/Erosion_Examples/heightfield_noise1`：
#   amp = 500, elementsize = 500, oct = 8, lac = 1.841, rough = 0.5,
#   basis = sparse, centernoise = on。
BASE_AMP_M = 500.0
BASE_ELEMENT_SIZE = 500.0
BASE_OCTAVES = 8.0
BASE_LACUNARITY = 1.841
BASE_ROUGH = 0.5
BASE_SEED = 0.0
# height 属性的参考尺度：Houdini 场景里高度实际落在 ±80 m 量级，
# 用 500 m（= amp）作为归一化基准，使属性值落在 ±1 内。
BASE_REFERENCE_SCALE = 500.0

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
    # ---------------------------------------------------------------
    # 基座地形：严格按 Houdini heightfield_noise1 的真实参数重建
    #
    # hython 直读 `/obj/Erosion_Examples/heightfield_noise1` 得到的值：
    #   amp = 500 m, elementsize = 500 m, oct = 8, lac = 1.841,
    #   rough = 0.5, basis = sparse, fractal = hmfT, centernoise = on,
    #   场景 1000×1000 m / 512 采样。
    #
    # 场景只有 1 km 而 element 是 500 m ⇒ 整张图只有约 2 个大团块。早期用
    # element 900 + oct 3 + 未归一化的坐标，得到的是逐格纯噪声，与基准的
    # 平滑团块完全相反。
    #
    # 归一化约定（关键）：Houdini 的 element 是"噪声特征尺寸（米）"，在边长
    # size_m 的网格上对应 size_m / element 次重复。Blender 的 Noise.Scale 在
    # [0,1] UV 空间里就是"重复次数"，所以直接用 size_m / element。
    # ---------------------------------------------------------------
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (-2400, 2400)

    separate = tree.nodes.new("ShaderNodeSeparateXYZ")
    separate.location = (-2200, 2400)
    tree.links.new(position.outputs["Position"], separate.inputs["Vector"])
    # UV = position / size_m，范围 [-0.5, 0.5]
    uv_x = erode.math_node(tree, "DIVIDE", separate.outputs["X"], size_m,
                           (-2000, 2450))
    uv_y = erode.math_node(tree, "DIVIDE", separate.outputs["Y"], size_m,
                           (-2000, 2330))
    uv = erode.combine_xyz(tree, uv_x.outputs[0], uv_y.outputs[0], 0.0,
                           (-1850, 2400))

    # 频率 = size_m / elementsize：1000 / 500 = 2 次重复。
    base_noise = tree.nodes.new("ShaderNodeTexNoise")
    base_noise.name = "VL_Base_Noise_Sparse"
    base_noise.location = (-1650, 2400)
    base_noise.noise_dimensions = "4D"
    base_noise.inputs["Scale"].default_value = size_m / BASE_ELEMENT_SIZE
    base_noise.inputs["Detail"].default_value = BASE_OCTAVES
    base_noise.inputs["Roughness"].default_value = BASE_ROUGH
    base_noise.inputs["Lacunarity"].default_value = BASE_LACUNARITY
    base_noise.inputs["W"].default_value = BASE_SEED
    # sparse 噪声对应的 Perlin 基底；Distortion 保留一点点（Houdini 的
    # disp = 0.1405，latticewarp 关闭）。
    base_noise.inputs["Distortion"].default_value = 0.1405
    tree.links.new(uv.outputs["Vector"], base_noise.inputs["Vector"])

    # centernoise = on ⇒ 噪声以 0 为中心，振幅 ±amp。
    centered = erode.math_node(tree, "SUBTRACT", base_noise.outputs["Fac"], 0.5,
                               (-1450, 2400))
    # 振幅换算成归一化层：amp 米 / Height Scale 米。
    # height 属性是"米 / Height Scale"的归一化量，后面再乘回 Height Scale。
    relief = erode.math_node(tree, "MULTIPLY", centered.outputs[0],
                             BASE_AMP_M / BASE_REFERENCE_SCALE,
                             (-1250, 2400))

    geometry = grid.outputs["Mesh"]
    for index, (layer, value) in enumerate(
            (("height", relief.outputs[0]), ("water", None),
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

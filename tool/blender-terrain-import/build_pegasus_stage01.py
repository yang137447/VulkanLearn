"""阶段 01 主入口：在 Blender 里搭出 Pegasus 高度场基座 + 一轮侵蚀的可交互节点组。

运行（后台，生成 .blend 与预览图）::

    & blender.exe --background --python tool\\blender-terrain-import\\build_pegasus_stage01.py -- ^
      --blend artifacts\\pegasus\\stage01\\stage01.blend

或者在 Blender 里打开 Scripting -> 运行此文件，会直接在当前场景生成对象与节点组，
可以立刻在节点编辑器里调参数。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))

# 重新导入，避免 Blender 里残留上次运行的旧模块。
for _stale in [m for m in list(sys.modules) if m.startswith("pegasus_stage01_gn")]:
    del sys.modules[_stale]

import pegasus_stage01_gn as stage01  # noqa: E402

DEFAULT_GROUP_NAME = "VL_Pegasus_Stage01_Heightfield_Erode"

# 组输入：名字、类型、默认、最小、最大、说明。
# 默认值取自 sop_pe_landscape.1.0.hda 的实际参数（见 README 的阶段 01 表）。
INPUTS = [
    ("Height Scale", "NodeSocketFloat", 800.0, 1.0, 8000.0,
     "最终地形高度（米）。Houdini 侧对应 heightfield 的 Size.y。"),
    ("Sea Level", "NodeSocketFloat", 0.0, -2000.0, 2000.0,
     "海平面在高度层里的位置；低于它会被压平。"),
    ("Noise 1 Amplitude", "NodeSocketFloat", 0.62, 0.0, 2.0,
     "heightfield_noise1 amp=200 归一化后的振幅（大尺度山体）。"),
    ("Noise 1 Element", "NodeSocketFloat", 2400.0, 10.0, 20000.0,
     "heightfield_noise1 elementsize=900（米）。8km 场景要放大到 2400 才有"
     "粗尺度山体，否则整岛是均匀噪声。"),
    ("Noise 2 Amplitude", "NodeSocketFloat", 0.16, 0.0, 2.0,
     "heightfield_noise2 amp=90 归一化后的振幅（中尺度山脊）。"),
    ("Noise 2 Element", "NodeSocketFloat", 280.0, 10.0, 20000.0,
     "heightfield_noise2 elementsize=280（米）。"),
    ("Coast Wobble", "NodeSocketFloat", 0.10, 0.0, 0.4,
     "海岸线径向扰动强度；0 = 正圆岛。"),
    ("Distort Element", "NodeSocketFloat", 800.0, 10.0, 20000.0,
     "heightfield_distort element_size=800（米）。"),
    ("Flow Merge", "NodeSocketFloat", 0.85, 0.0, 1.0,
     "每轮从下坡邻居吸收的汇水比例；越大河道越集中。"),
    ("Flow Exponent", "NodeSocketFloat", 1.0, 0.1, 4.0,
     "stream-power 的 A^m 指数。"),
    ("Slope Exponent", "NodeSocketFloat", 1.0, 0.1, 4.0,
     "stream-power 的 S^n 指数。"),
    ("Erosion Strength", "NodeSocketFloat", 0.35, 0.0, 4.0,
     "下切强度 K；对应 Houdini hydro_erosionrate=1 的相对比例。"),
    ("Max Incision", "NodeSocketFloat", 120.0, 0.0, 2000.0,
     "单步最大下切（米），防止噪声点被削穿。"),
    ("Talus Angle", "NodeSocketFloat", 50.0, 1.0, 89.0,
     "热力塌落的休止角（度）；Houdini thermal_cutangle=50。"),
    ("Thermal Factor", "NodeSocketFloat", 0.5, 0.0, 1.0,
     "超角高差向邻域转移的比例；Houdini thermal_removalrate=0.7。"),
]


def _parse_args() -> argparse.Namespace:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blend", type=Path, default=None,
                        help="保存 .blend 的路径；不传则不保存")
    parser.add_argument("--resolution", type=int, default=256,
                        help="网格顶点数（每边）")
    parser.add_argument("--size-m", type=float, default=8000.0)
    parser.add_argument("--name", default="stage01")
    parser.add_argument("--cut", default="full",
                        choices=("base", "erosion", "full"),
                        help="调试用：输出到哪一级")
    return parser.parse_args(argv)


def build_group(size_m: float, resolution: int, cut: str = "full"):
    tree = bpy.data.node_groups.new(DEFAULT_GROUP_NAME, "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT",
                              socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT",
                              socket_type="NodeSocketGeometry")
    stage01.add_inputs(tree, INPUTS)

    group_input = tree.nodes.new("NodeGroupInput")
    group_input.location = (-2600, -600)
    ui = {}
    for item in tree.interface.items_tree:
        if getattr(item, "in_out", None) == "INPUT" and \
                item.socket_type != "NodeSocketGeometry":
            ui[item.name] = group_input.outputs[item.name]

    group_output = tree.nodes.new("NodeGroupOutput")
    group_output.location = (5800, 400)

    grid, base_height, uv, coast = stage01.build_base_heightfield(
        tree, ui, group_input, size_m, resolution)

    # 顺序很重要：
    # 1. 先在**未位移**的网格上写 height / erode_flow / vl_coast。
    #    coast 是 Position 的字段，位置一改它就会重新求值成别的值；实测把
    #    vl_coast 写进 SetPosition 之后，整条属性的关系就断了。
    # 2. 再 SetPosition 把网格抬到基座高度，供侵蚀阶段邻域采样。
    # 3. 位移后的几何只用来做采样，属性值仍来自第 1 步存下的副本。
    seed_geometry = stage01.store_attribute(
        tree, grid, "height", base_height, (-600, 400))
    store_flow = stage01.store_attribute(
        tree, seed_geometry.outputs["Geometry"], "erode_flow", None, (-420, 300))
    store_coast = stage01.store_attribute(
        tree, store_flow.outputs["Geometry"], "vl_coast", coast, (-240, 200))

    seed = stage01.math_node(tree, "MULTIPLY", base_height,
                             group_input.outputs["Height Scale"],
                             location=(-600, 700))
    seed_combine = tree.nodes.new("ShaderNodeCombineXYZ")
    seed_combine.location = (-600, 560)
    stage01.link(tree, seed.outputs[0], seed_combine.inputs["Z"])
    seed_position = stage01.set_position(
        tree, store_coast.outputs["Geometry"], seed_combine.outputs["Vector"],
        (-60, 400))

    if cut == "base":
        stage01.link(tree, store_coast.outputs["Geometry"],
                     group_output.inputs["Geometry"])
        return tree

    # 侵蚀内部按“归一化高度层”比较，所以这里取回属性而不是米制坐标。
    height = stage01.named_attribute(tree, "height", (120, 700))
    eroded_geometry, eroded_height, flow = stage01.build_one_erosion_pass(
        tree, ui, group_input, seed_position.outputs["Geometry"], height,
        size_m, resolution)

    # 侵蚀结果写回 height 属性；网格已经带着终末位移，这里不再继续抬升，
    # 否则高度会被加两次。
    store_final = stage01.store_attribute(
        tree, eroded_geometry, "height", eroded_height, (5560, 400))
    if cut == "erosion":
        stage01.link(tree, store_final.outputs["Geometry"],
                     group_output.inputs["Geometry"])
        return tree

    scaled = stage01.math_node(tree, "MULTIPLY", eroded_height,
                               group_input.outputs["Height Scale"],
                               location=(5560, 600))
    combine = tree.nodes.new("ShaderNodeCombineXYZ")
    combine.location = (5740, 600)
    stage01.link(tree, scaled.outputs[0], combine.inputs["Z"])
    set_position = stage01.set_position(
        tree, store_final.outputs["Geometry"], combine.outputs["Vector"],
        (5740, 200))
    stage01.link(tree, set_position.outputs["Geometry"], group_output.inputs["Geometry"])
    return tree


def make_object(group):
    mesh = bpy.data.meshes.new("VL_Pegasus_Stage01.mesh")
    obj = bpy.data.objects.new("VL_Pegasus_Stage01", mesh)
    bpy.context.collection.objects.link(obj)
    modifier = obj.modifiers.new("Pegasus Stage01 GN", "NODES")
    modifier.node_group = group
    bpy.context.view_layer.update()
    return obj


def main() -> int:
    args = _parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    group = build_group(args.size_m, args.resolution, args.cut)
    obj = make_object(group)

    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    print(f"STAGE01 cut={args.cut} vertices={len(mesh.vertices)} "
          f"attributes={[a.name for a in mesh.attributes]}")
    evaluated.to_mesh_clear()

    if args.blend:
        args.blend.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.blend.resolve()))
        print(f"STAGE01 blend={args.blend}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

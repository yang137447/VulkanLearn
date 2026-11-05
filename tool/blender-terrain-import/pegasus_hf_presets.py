"""地形节点组库 —— 组合组（Preset）。

把基础算子拼成 Pegasus 实际用的地形链，方便直接调取：

| 组名 | 内容 |
|---|---|
| ``VL_HF_PegasusTerrain`` | Noise → Distort → Erode，Pegasus 主地形链 |
| ``VL_HF_ErosionExample`` | HFErodeExamples.hip 里的最小可复现链 |

组合组内部只嵌套 ``VL_HF_*`` 子组，不直接放裸节点，保证替换实现时上层不变。
"""

from __future__ import annotations

import bpy

import pegasus_hf_core as core
import pegasus_hf_erode as erode_nodes
import pegasus_hf_nodes as base_nodes


def _instance(tree, group, location, params=None):
    node = tree.nodes.new("GeometryNodeGroup")
    node.node_tree = group
    node.location = location
    if params:
        for socket in node.inputs:
            if socket.name in params:
                socket.default_value = params[socket.name]
    return node


def build_pegasus_terrain_group():
    """Pegasus 主地形链：噪声基座 → 扭曲 → 侵蚀。

    参数默认值来自原工程：
    ``heightfield_noise1``（amp 500 / element 500 / oct 8 / lac 1.841）、
    ``heightfield_distort1``、``heightfield_erode1``。
    """
    tree = core.new_group(
        "PegasusTerrain",
        "Pegasus 主地形链：Heightfield → Noise（大尺度山体）→ Distort → Erode。"
        "参数默认值取自 Pegasus 原工程 heightfield_noise1 / distort1 / erode1。")
    group_input = core.add_params(tree, [
        ("Size", "NodeSocketFloat", 1000.0, 1.0, 100000.0,
         "地形边长（米）。"),
        ("Samples", "NodeSocketInt", 256, 2, 2048,
         "每边顶点数。"),
        ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
         "米与归一化层的换算基准。"),
        ("Noise Amplitude", "NodeSocketFloat", 385.0, 0.0, 5000.0,
         "噪声振幅（米），对应 amp。"),
        ("Noise Element Size", "NodeSocketFloat", 500.0, 1.0, 100000.0,
         "噪声特征尺寸（米），对应 elementsize。"),
        ("Noise Octaves", "NodeSocketFloat", 8.0, 1.0, 16.0,
         "分形层数，对应 oct。"),
        ("Noise Lacunarity", "NodeSocketFloat", 2.2, 1.0, 4.0,
         "层间频率倍率，对应 lac。"),
        ("Noise Roughness", "NodeSocketFloat", 0.5, 0.0, 1.0,
         "层间振幅衰减，对应 rough。这是控制高频含量与坡度的主开关："
         "0.5 时每层减半、第 8 层只剩 1/128；调高会显著增加陡坡与沟谷。"),
        ("Distort Amplitude", "NodeSocketFloat", 140.0, 0.0, 5000.0,
         "扭曲强度（米），对应 distort amp。"),
        ("Distort Element Size", "NodeSocketFloat", 800.0, 1.0, 100000.0,
         "扭曲噪声尺寸（米），对应 element_size。"),
        ("Erosion Enabled", "NodeSocketBool", True, None, None,
         "是否执行侵蚀（关闭时只输出基座地形，便于对比）。"),
        ("Water Spread Iterations", "NodeSocketInt", 120, 0, 400,
         "水扩散迭代数，对应 water_spreaditers。"),
        ("Hydro Erodability", "NodeSocketFloat", 1.5, 0.0, 100.0,
         "可蚀性，对应 hydro_erodability。"),
        ("Hydro Erosion Rate", "NodeSocketFloat", 1.0, 0.0, 100.0,
         "可蚀性乘数，对应 hydro_erosionrate。"),
        ("Bank Angle", "NodeSocketFloat", 20.0, 0.0, 90.0,
         "河道横截面坡角，对应 hydro_bankangle。"),
        ("Thermal Erosion Rate", "NodeSocketFloat", 0.03, 0.0, 1.0,
         "热力侵蚀速率，对应 thermal_erosionrate。"),
        ("Freeze Frames", "NodeSocketInt", 1, 1, 100,
         "冻结帧数，对应 freezeframe。"),
    ])
    p = core.params(group_input)

    # 复用 build_hf_library 已建好的子组；不重新构建，
    # 否则同名组会被删除重建，外层的引用全部失效。
    needed = ("VL_HF_Heightfield", "VL_HF_Noise", "VL_HF_Distort", "VL_HF_Erode")
    groups = {}
    for name in needed:
        existing = bpy.data.node_groups.get(name)
        if existing is None:
            raise RuntimeError(f"preset needs {name}, build it first")
        groups[name] = existing

    heightfield = _instance(tree, groups["VL_HF_Heightfield"], (-2200, 0))
    core.link(tree, p["Size"], heightfield.inputs["Size"])
    core.link(tree, p["Samples"], heightfield.inputs["Samples"])

    noise = _instance(tree, groups["VL_HF_Noise"], (-1700, 0))
    core.link(tree, p["Size"], noise.inputs["Size"])
    core.link(tree, p["Height Scale"], noise.inputs["Height Scale"])
    core.link(tree, p["Noise Amplitude"], noise.inputs["Amplitude"])
    core.link(tree, p["Noise Element Size"], noise.inputs["Element Size"])
    core.link(tree, p["Noise Octaves"], noise.inputs["Octaves"])
    core.link(tree, p["Noise Lacunarity"], noise.inputs["Lacunarity"])
    core.link(tree, p["Noise Roughness"], noise.inputs["Roughness"])

    distort = _instance(tree, groups["VL_HF_Distort"], (-1200, 0))
    core.link(tree, p["Size"], distort.inputs["Size"])
    core.link(tree, p["Height Scale"], distort.inputs["Height Scale"])
    core.link(tree, p["Distort Amplitude"], distort.inputs["Amplitude"])
    core.link(tree, p["Distort Element Size"], distort.inputs["Element Size"])

    erode = _instance(tree, groups["VL_HF_Erode"], (-700, 0))
    core.link(tree, p["Height Scale"], erode.inputs["Height Scale"])
    core.link(tree, p["Water Spread Iterations"],
              erode.inputs["Water Spread Iterations"])
    core.link(tree, p["Hydro Erodability"], erode.inputs["Hydro Erodability"])
    core.link(tree, p["Hydro Erosion Rate"], erode.inputs["Hydro Erosion Rate"])
    core.link(tree, p["Bank Angle"], erode.inputs["Bank Angle"])
    core.link(tree, p["Thermal Erosion Rate"], erode.inputs["Thermal Erosion Rate"])
    core.link(tree, p["Freeze Frames"], erode.inputs["Freeze Frames"])

    # 格距 = Size / (Samples - 1)
    cell = core.math_node(tree, "SUBTRACT", p["Samples"], 1.0, (-1200, -400))
    cell_div = core.math_node(tree, "DIVIDE", p["Size"], cell.outputs[0],
                              (-1000, -400))
    core.link(tree, cell_div.outputs[0], erode.inputs["Cell Size"])

    core.link(tree, group_input.outputs["Geometry"], heightfield.inputs["Geometry"])
    core.link(tree, heightfield.outputs["Geometry"], noise.inputs["Geometry"])
    core.link(tree, noise.outputs["Geometry"], distort.inputs["Geometry"])
    core.link(tree, distort.outputs["Geometry"], erode.inputs["Geometry"])

    # Erosion Enabled 关闭时直接输出扭曲后的地形
    switch = tree.nodes.new("GeometryNodeSwitch")
    switch.location = (-200, 0)
    switch.input_type = "GEOMETRY"
    core.link(tree, p["Erosion Enabled"], switch.inputs["Switch"])
    core.link(tree, distort.outputs["Geometry"], switch.inputs["False"])
    core.link(tree, erode.outputs["Geometry"], switch.inputs["True"])

    core.finish_group(tree, group_input, switch.outputs[0], (200, 0))
    return tree


BUILDERS = (build_pegasus_terrain_group,)


def build_all():
    return [builder() for builder in BUILDERS]

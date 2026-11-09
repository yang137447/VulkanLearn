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
        "【造型层】Heightfield → Noise ×2 → Distort。\n"
        "资产烘焙只跑这一段（build_pegasus_library_global.py --no-erosion）。\n"
        "水文（填洼/汇流/stream power/坡面）在 tool/blender-terrain-import/"
        "terrain_erosion.py，由 erode_pegasus_global.py 调用：\n"
        "  K = --erodibility [1/yr]，dt = --dt-years，步数 = --steps，\n"
        "  U = --uplift-mm-per-kyr，m/n/flow-method 也在那里。\n"
        "本组内的 Erosion* 参数只影响 GN 内的实验性侵蚀，不参与烘焙。")
    # 节点树里也留一块说明，避免只看节点的人以为侵蚀在这里跑。
    note = tree.nodes.new("NodeFrame")
    note.label = (
        "造型层：Noise ×2 → Distort。水文不在这里 —— "
        "见 terrain_erosion.py / erode_pegasus_global.py（K、dt、steps、uplift）"
    )
    note.label_size = 20
    note.use_custom_color = True
    note.color = (0.18, 0.22, 0.30)
    group_input = core.add_params(tree, [
        ("Size", "NodeSocketFloat", 1000.0, 1.0, 100000.0,
         "地形边长（米）。"),
        ("Samples", "NodeSocketInt", 256, 2, 2048,
         "每边顶点数。"),
        ("Work Samples", "NodeSocketInt", 512, 2, 2048,
         "噪声 / 扭曲 / 侵蚀的工作网格密度。Houdini 的 heightfield 是 4000 m "
         "512 格，最终 1024 导出只是升采样；在 1024 上跑 5 帧 × 140 次扩散"
         "既慢（实测 >15 分钟）也不会增加参考里不存在的细节。"),
        ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
         "米与归一化层的换算基准。"),
        ("Noise Amplitude", "NodeSocketFloat", 385.0, 0.0, 5000.0,
         "噪声振幅（米），对应 amp。"),
        ("Noise Element Size", "NodeSocketFloat", 500.0, 1.0, 100000.0,
         "噪声特征尺寸（米），对应 elementsize。"),
        ("Noise Seed", "NodeSocketFloat", 0.0, 0.0, 1000.0,
         "噪声 4D W 种子；不同 seed 会移动中心特征，避免固定放射纹。"),
        ("Noise Distortion", "NodeSocketFloat", 0.1405, 0.0, 4.0,
         "噪声坐标扭曲；Perlin 基函数下过大会产生旋涡/放射纹。"),
        ("Rhythm Strength", "NodeSocketFloat", 0.85, 0.0, 1.0,
         "细节疏密强度；0 = 全局平铺，1 = 细节只留在低频 mask 的高值带。"),
        ("Rhythm Element Size", "NodeSocketFloat", 3000.0, 100.0, 100000.0,
         "低频疏密 mask 的特征尺寸（米），控制大块疏/密区域。"),
        ("Noise Octaves", "NodeSocketFloat", 8.0, 1.0, 16.0,
         "分形层数，对应 oct。"),
        ("Noise Lacunarity", "NodeSocketFloat", 2.2, 1.0, 4.0,
         "层间频率倍率，对应 lac。"),
        ("Noise Roughness", "NodeSocketFloat", 0.5, 0.0, 1.0,
         "层间振幅衰减，对应 rough。这是控制高频含量与坡度的主开关："
         "0.5 时每层减半、第 8 层只剩 1/128；调高会显著增加陡坡与沟谷。"),
        ("Detail Amplitude", "NodeSocketFloat", 90.0, 0.0, 5000.0,
         "第二级噪声振幅（米），对应 Pegasus 的 heightfield_noise2。"
         "设为 0 即退化为单级噪声。"),
        ("Detail Element Size", "NodeSocketFloat", 280.0, 1.0, 100000.0,
         "第二级噪声特征尺寸（米），对应 heightfield_noise2 elementsize。"),
        ("Distort Amplitude", "NodeSocketFloat", 140.0, 0.0, 5000.0,
         "扭曲强度（米），对应 distort amp。作用在世界坐标上，单位是米。"),
        ("Distort Element Size", "NodeSocketFloat", 800.0, 1.0, 100000.0,
         "扭曲噪声尺寸（米），对应 element_size。"),
        ("Erosion Enabled", "NodeSocketBool", True, None, None,
         "是否执行侵蚀（关闭时只输出基座地形，便于对比）。"),
        ("Water Spread Iterations", "NodeSocketInt", 140, 0, 400,
         "[GN 实验侵蚀] 水扩散迭代数，对应 water_spreaditers。"
         "烘焙不走这里，水文参数在 erode_pegasus_global.py。"),
        ("Global Erosion Rate", "NodeSocketFloat", 1.0, 0.0, 50.0,
         "[GN 实验侵蚀] 每帧侵蚀米数，对应 globalerosionrate。"
         "烘焙用 --erodibility K [1/yr] + --dt-years。"),
        ("Erodability Slope Factor", "NodeSocketFloat", 0.8, 0.0, 5.0,
         "[GN 实验侵蚀] 坡度影响系数，对应 erodability_slopefactor。"),
        ("Hydro Erodability", "NodeSocketFloat", 1.5, 0.0, 100.0,
         "[GN 实验侵蚀] 可蚀性，对应 hydro_erodability。"
         "烘焙对应 --erodibility。"),
        ("Hydro Erosion Rate", "NodeSocketFloat", 1.0, 0.0, 100.0,
         "[GN 实验侵蚀] 可蚀性乘数，对应 hydro_erosionrate。"),
        ("Bank Angle", "NodeSocketFloat", 20.0, 0.0, 90.0,
         "[GN 实验侵蚀] 河道横截面坡角，对应 hydro_bankangle。"),
        ("Thermal Erosion Rate", "NodeSocketFloat", 0.03, 0.0, 1.0,
         "[GN 实验侵蚀] 坡面松弛速率；烘焙对应 --thermal-rate。"),
        ("Freeze Frames", "NodeSocketInt", 1, 1, 100,
         "[GN 实验侵蚀] 帧循环次数；烘焙对应 --steps。"),
    ])
    p = core.params(group_input)

    # 复用 build_hf_library 已建好的子组；不重新构建，
    # 否则同名组会被删除重建，外层的引用全部失效。
    needed = ("VL_HF_Heightfield", "VL_HF_Noise", "VL_HF_Distort",
              "VL_HF_Erode", "VL_HF_Resample")
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
    core.link(tree, p["Noise Seed"], noise.inputs["Seed"])
    core.link(tree, p["Noise Distortion"], noise.inputs["Distortion"])
    core.link(tree, p["Noise Octaves"], noise.inputs["Octaves"])
    core.link(tree, p["Noise Lacunarity"], noise.inputs["Lacunarity"])
    core.link(tree, p["Noise Roughness"], noise.inputs["Roughness"])

    # Pegasus 主链是 heightfield_noise1 → heightfield_noise2 两级叠加：
    # 大尺度山体（amp 200 / element 900）之上再叠中尺度细节（amp 90 / element 280）。
    # 两级共用 oct/lac/rough，与解码出的参数一致。
    detail = _instance(tree, groups["VL_HF_Noise"], (-1450, 0))
    core.link(tree, p["Size"], detail.inputs["Size"])
    core.link(tree, p["Height Scale"], detail.inputs["Height Scale"])
    core.link(tree, p["Detail Amplitude"], detail.inputs["Amplitude"])
    core.link(tree, p["Detail Element Size"], detail.inputs["Element Size"])
    detail_seed = core.math_node(
        tree, "ADD", p["Noise Seed"], 37.0, (-1600, -220))
    core.link(tree, detail_seed.outputs[0], detail.inputs["Seed"])
    core.link(tree, p["Noise Distortion"], detail.inputs["Distortion"])
    core.link(tree, p["Noise Octaves"], detail.inputs["Octaves"])
    core.link(tree, p["Noise Lacunarity"], detail.inputs["Lacunarity"])
    core.link(tree, p["Noise Roughness"], detail.inputs["Roughness"])

    # 低频 rhythm mask：把 detail 从“全图平铺”改成有疏密带的叠加。
    rhythm_pos = tree.nodes.new("GeometryNodeInputPosition")
    rhythm_pos.location = (-1900, -420)
    rhythm_sep = tree.nodes.new("ShaderNodeSeparateXYZ")
    rhythm_sep.location = (-1750, -420)
    core.link(tree, rhythm_pos.outputs["Position"], rhythm_sep.inputs["Vector"])
    rhythm_uv_x = core.math_node(
        tree, "DIVIDE", rhythm_sep.outputs["X"], p["Size"], (-1600, -380))
    rhythm_uv_y = core.math_node(
        tree, "DIVIDE", rhythm_sep.outputs["Y"], p["Size"], (-1600, -500))
    rhythm_uv = core.combine_xyz(
        tree, rhythm_uv_x.outputs[0], rhythm_uv_y.outputs[0], 0.0,
        (-1450, -420))
    rhythm_scale = core.math_node(
        tree, "DIVIDE", p["Size"], p["Rhythm Element Size"], (-1600, -620))
    rhythm_noise = tree.nodes.new("ShaderNodeTexNoise")
    rhythm_noise.location = (-1250, -420)
    rhythm_noise.noise_dimensions = "4D"
    rhythm_noise.inputs["Detail"].default_value = 2.0
    rhythm_noise.inputs["Roughness"].default_value = 0.5
    core.link(tree, rhythm_scale.outputs[0], rhythm_noise.inputs["Scale"])
    core.link(tree, rhythm_uv.outputs["Vector"], rhythm_noise.inputs["Vector"])
    rhythm_seed = core.math_node(
        tree, "ADD", p["Noise Seed"], 101.0, (-1450, -620))
    core.link(tree, rhythm_seed.outputs[0], rhythm_noise.inputs["W"])
    rhythm_raw = core.math_node(
        tree, "SUBTRACT", rhythm_noise.outputs["Fac"], 0.35, (-1050, -380))
    rhythm_t = core.math_node(
        tree, "DIVIDE", rhythm_raw.outputs[0], 0.30, (-900, -380))
    rhythm_clamped = core.math_node(
        tree, "MAXIMUM", rhythm_t.outputs[0], 0.0, (-750, -380))
    rhythm_clamped = core.math_node(
        tree, "MINIMUM", rhythm_clamped.outputs[0], 1.0, (-600, -380))
    rhythm_inv = core.math_node(
        tree, "SUBTRACT", 1.0, rhythm_clamped.outputs[0], (-450, -380))
    rhythm_scaled = core.math_node(
        tree, "MULTIPLY", rhythm_inv.outputs[0], p["Rhythm Strength"],
        (-300, -380))
    rhythm_mask = core.math_node(
        tree, "SUBTRACT", 1.0, rhythm_scaled.outputs[0], (-150, -380))
    core.link(tree, rhythm_mask.outputs[0], detail.inputs["Mask"])

    distort = _instance(tree, groups["VL_HF_Distort"], (-1200, 0))
    core.link(tree, p["Size"], distort.inputs["Size"])
    core.link(tree, p["Distort Amplitude"], distort.inputs["Amplitude"])
    core.link(tree, p["Distort Element Size"], distort.inputs["Element Size"])

    erode = _instance(tree, groups["VL_HF_Erode"], (-700, 0))
    core.link(tree, p["Height Scale"], erode.inputs["Height Scale"])
    core.link(tree, p["Water Spread Iterations"],
              erode.inputs["Water Spread Iterations"])
    core.link(tree, p["Global Erosion Rate"], erode.inputs["Global Erosion Rate"])
    core.link(tree, p["Erodability Slope Factor"],
              erode.inputs["Erodability Slope Factor"])
    core.link(tree, p["Hydro Erodability"], erode.inputs["Hydro Erodability"])
    core.link(tree, p["Hydro Erosion Rate"], erode.inputs["Hydro Erosion Rate"])
    core.link(tree, p["Bank Angle"], erode.inputs["Bank Angle"])
    core.link(tree, p["Thermal Erosion Rate"], erode.inputs["Thermal Erosion Rate"])
    core.link(tree, p["Freeze Frames"], erode.inputs["Freeze Frames"])
    # 汇水面积归一化需要工作网格的总格数：A / (Work Samples²)。
    grid_cells = core.math_node(tree, "MULTIPLY", p["Work Samples"],
                                p["Work Samples"],
                                (-1000, -600))
    core.link(tree, grid_cells.outputs[0], erode.inputs["Grid Cells"])

    # 格距必须跟着工作网格走，而不是输出分辨率。
    cell = core.math_node(tree, "SUBTRACT", p["Work Samples"], 1.0,
                          (-1200, -400))
    cell_div = core.math_node(tree, "DIVIDE", p["Size"], cell.outputs[0],
                              (-1000, -400))
    core.link(tree, cell_div.outputs[0], erode.inputs["Cell Size"])

    # --- 分阶段求解 ---------------------------------------------------------
    #
    # 噪声/扭曲留在**输出密度**（参考的 1024 导出确实带 1024 尺度的细节：
    # 升采样自 512 的场 curv_norm 只有 0.048，参考是 0.157）。
    # 侵蚀只在**工作密度**上解，然后把侵蚀产生的**增量**加回细网格。
    #
    # 早期版本把整张场降到工作密度再升采样，等于把所有高频细节一起抹掉，
    # 曲率直接掉到目标的三分之一。
    down = _instance(tree, groups["VL_HF_Resample"], (-950, 0))
    core.link(tree, p["Size"], down.inputs["Size"])
    core.link(tree, p["Work Samples"], down.inputs["Samples"])

    before_up = _instance(tree, groups["VL_HF_Resample"], (-450, 400))
    core.link(tree, p["Size"], before_up.inputs["Size"])
    core.link(tree, p["Samples"], before_up.inputs["Samples"])
    core.link(tree, down.outputs["Geometry"], before_up.inputs["Geometry"])

    after_up = _instance(tree, groups["VL_HF_Resample"], (-200, 0))
    core.link(tree, p["Size"], after_up.inputs["Size"])
    core.link(tree, p["Samples"], after_up.inputs["Samples"])
    core.link(tree, erode.outputs["Geometry"], after_up.inputs["Geometry"])

    core.link(tree, group_input.outputs["Geometry"], heightfield.inputs["Geometry"])
    core.link(tree, heightfield.outputs["Geometry"], noise.inputs["Geometry"])
    core.link(tree, noise.outputs["Geometry"], detail.inputs["Geometry"])
    core.link(tree, detail.outputs["Geometry"], distort.inputs["Geometry"])
    core.link(tree, distort.outputs["Geometry"], down.inputs["Geometry"])
    core.link(tree, down.outputs["Geometry"], erode.inputs["Geometry"])

    fine_position = tree.nodes.new("GeometryNodeInputPosition")
    fine_position.location = (200, 700)
    fine_height = core.read_layer(tree, "height", (200, 560))

    def sample_height(geometry_socket, location):
        sampler = tree.nodes.new("GeometryNodeSampleNearestSurface")
        sampler.location = location
        sampler.data_type = "FLOAT"
        core.link(tree, geometry_socket, sampler.inputs["Mesh"])
        core.link(tree, core.read_layer(tree, "height", (location[0] - 200, location[1] - 120)),
                  sampler.inputs["Value"])
        core.link(tree, fine_position.outputs["Position"],
                  sampler.inputs["Sample Position"])
        return sampler.outputs["Value"]

    before_height = sample_height(before_up.outputs["Geometry"], (400, 760))
    after_height = sample_height(after_up.outputs["Geometry"], (400, 560))
    delta = core.math_node(tree, "SUBTRACT", after_height, before_height,
                           (600, 660))
    merged_height = core.math_node(tree, "ADD", fine_height, delta.outputs[0],
                                   (760, 620))

    geometry = core.write_layer(tree, distort.outputs["Geometry"], "height",
                                merged_height.outputs[0], (900, 620))
    # 其余层（water/debris/sediment/flow/bedrock）来自已侵蚀的工作网格；
    # 它们只作为掩膜使用，工作密度足够，不需要额外细节。
    for index, layer in enumerate(("water", "debris", "sediment", "flow", "bedrock")):
        sampler = tree.nodes.new("GeometryNodeSampleNearestSurface")
        sampler.location = (900, 380 - index * 160)
        sampler.data_type = "FLOAT"
        core.link(tree, after_up.outputs["Geometry"], sampler.inputs["Mesh"])
        core.link(tree, core.read_layer(tree, layer, (700, 380 - index * 160)),
                  sampler.inputs["Value"])
        core.link(tree, fine_position.outputs["Position"],
                  sampler.inputs["Sample Position"])
        geometry = core.write_layer(tree, geometry.outputs["Geometry"], layer,
                                    sampler.outputs["Value"],
                                    (1050, 380 - index * 160))

    switch = tree.nodes.new("GeometryNodeSwitch")
    switch.location = (1250, 400)
    switch.input_type = "GEOMETRY"
    core.link(tree, p["Erosion Enabled"], switch.inputs["Switch"])
    core.link(tree, distort.outputs["Geometry"], switch.inputs["False"])
    core.link(tree, geometry.outputs["Geometry"], switch.inputs["True"])

    core.finish_group(tree, group_input, switch.outputs[0], (1450, 400))
    return tree


BUILDERS = (build_pegasus_terrain_group,)


def build_all():
    return [builder() for builder in BUILDERS]

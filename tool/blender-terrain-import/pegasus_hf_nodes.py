"""地形算子组库 —— 基础层。

每个组对应一个 Houdini heightfield 节点，参数名沿用 Houdini 语汇：

| 组名 | Houdini 节点 | 说明 |
|---|---|---|
| ``VL_HF_Heightfield`` | ``heightfield`` | 建地形网格并初始化各层 |
| ``VL_HF_Noise`` | ``heightfield_noise`` | 叠加噪声（amp/element/basis/oct/lac/rough） |
| ``VL_HF_Distort`` | ``heightfield_distort`` | 用噪声扭曲层（对应 curl warp） |
| ``VL_HF_Remap`` | ``heightfield_remap`` | 层数值区间重映射 |
| ``VL_HF_Resample`` | ``heightfield_resample`` | 改变网格分辨率 |
| ``VL_HF_Blur`` | ``heightfield_blur`` | 模糊 / 收缩 / 膨胀 |

这一层只依赖 :mod:`pegasus_hf_core`，不引用任何具体场景。
"""

from __future__ import annotations

import math

import bpy

import pegasus_hf_core as core


# ---------------------------------------------------------------- Heightfield


def build_heightfield():
    """建地形网格并初始化全部数据层。

    Houdini ``heightfield`` 的参数：``sizex`` / ``sizey`` / ``gridsamples`` /
    ``gridspacing`` / ``initialheight``。这里保留 Size / Samples 两项，
    其余层初值固定为 0（与 Houdini 一致）。
    """
    tree = core.new_group(
        "Heightfield",
        "对应 Houdini heightfield 节点：建立地形网格，并初始化 "
        "height/water/debris/sediment/bedrock/flow 六个地形层。")
    group_input = core.add_params(tree, [
        ("Size", "NodeSocketFloat", 1000.0, 1.0, 100000.0,
         "地形边长（米），对应 Houdini sizex/sizey。"),
        ("Samples", "NodeSocketInt", 256, 2, 2048,
         "每边顶点数，对应 Houdini gridsamples。"),
        ("Initial Height", "NodeSocketFloat", 0.0, -1000.0, 1000.0,
         "初始高度（归一化层），对应 Houdini initialheight。"),
    ])
    p = core.params(group_input)

    grid = tree.nodes.new("GeometryNodeMeshGrid")
    grid.location = (-1800, 600)
    core.link(tree, p["Size"], grid.inputs["Size X"])
    core.link(tree, p["Size"], grid.inputs["Size Y"])
    core.link(tree, p["Samples"], grid.inputs["Vertices X"])
    core.link(tree, p["Samples"], grid.inputs["Vertices Y"])

    geometry = grid.outputs["Mesh"]
    for index, layer in enumerate(core.TERRAIN_LAYERS):
        value = p["Initial Height"] if layer == "height" else None
        node = core.write_layer(tree, geometry, layer, value,
                                (-1500 + index * 220, 600))
        geometry = node.outputs["Geometry"]

    core.finish_group(tree, group_input, geometry, (400, 600))
    return tree


# ---------------------------------------------------------------- Noise


def build_noise():
    """噪声叠加，对应 Houdini ``heightfield_noise``。

    默认值取自 Pegasus 的 ``/obj/Erosion_Examples/heightfield_noise1``：
    amp 500、elementsize 500、basis sparse、oct 8、lac 1.841、rough 0.5、
    centernoise on。

    频率换算：Houdini 的 ``elementsize`` 是"噪声特征尺寸（米）"，在边长
    ``Size`` 的网格上对应 ``Size / Element Size`` 次重复。Blender 的
    ``Noise.Scale`` 在 [0,1] UV 空间里正好就是"重复次数"，所以直接相除。
    """
    tree = core.new_group(
        "Noise",
        "对应 Houdini heightfield_noise：向指定地形层叠加噪声。"
        "默认参数取自 Pegasus 的 heightfield_noise1（amp 500 / element 500 / sparse）。")
    group_input = core.add_params(tree, [
        ("Size", "NodeSocketFloat", 1000.0, 1.0, 100000.0,
         "地形边长（米），用于把 Element Size 换成噪声频率。"),
        ("Layer", "NodeSocketString", "height", None, None,
         "目标地形层，对应 Houdini layer。"),
        ("Amplitude", "NodeSocketFloat", 500.0, 0.0, 5000.0,
         "噪声振幅（米），对应 Houdini amp。写入层时除以 Height Scale。"),
        ("Element Size", "NodeSocketFloat", 500.0, 1.0, 100000.0,
         "噪声特征尺寸（米），对应 Houdini elementsize。"),
        ("Octaves", "NodeSocketFloat", 8.0, 1.0, 16.0,
         "分形层数，对应 Houdini oct。"),
        ("Lacunarity", "NodeSocketFloat", 1.841, 1.0, 4.0,
         "层间频率倍率，对应 Houdini lac。"),
        ("Roughness", "NodeSocketFloat", 0.5, 0.0, 1.0,
         "层间振幅衰减，对应 Houdini rough。"),
        ("Distortion", "NodeSocketFloat", 0.1405, 0.0, 4.0,
         "噪声坐标扭曲，对应 Houdini disp。"),
        ("Seed", "NodeSocketFloat", 0.0, 0.0, 1000.0,
         "噪声种子（4D W 通道）。"),
        ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
         "把米换算成归一化层时使用的参考高度。"),
        ("Center Noise", "NodeSocketBool", True, None, None,
         "噪声以 0 为中心（±amp），对应 Houdini centernoise。"),
    ])
    p = core.params(group_input)

    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (-1800, 900)
    separate = tree.nodes.new("ShaderNodeSeparateXYZ")
    separate.location = (-1600, 900)
    core.link(tree, position.outputs["Position"], separate.inputs["Vector"])

    # UV = position / Size，落在 [-0.5, 0.5]
    uv_x = core.math_node(tree, "DIVIDE", separate.outputs["X"], p["Size"],
                          (-1400, 950))
    uv_y = core.math_node(tree, "DIVIDE", separate.outputs["Y"], p["Size"],
                          (-1400, 830))
    uv = core.combine_xyz(tree, uv_x.outputs[0], uv_y.outputs[0], 0.0,
                          (-1200, 900))

    # 频率 = Size / Element Size
    frequency = core.math_node(tree, "DIVIDE", p["Size"], p["Element Size"],
                               (-1400, 700))

    noise = tree.nodes.new("ShaderNodeTexNoise")
    noise.location = (-1000, 900)
    noise.noise_dimensions = "4D"
    core.link(tree, frequency.outputs[0], noise.inputs["Scale"])
    core.link(tree, p["Octaves"], noise.inputs["Detail"])
    core.link(tree, p["Roughness"], noise.inputs["Roughness"])
    core.link(tree, p["Lacunarity"], noise.inputs["Lacunarity"])
    core.link(tree, p["Distortion"], noise.inputs["Distortion"])
    core.link(tree, p["Seed"], noise.inputs["W"])
    core.link(tree, uv.outputs["Vector"], noise.inputs["Vector"])

    signal = noise.outputs["Fac"]
    centered = core.math_node(tree, "SUBTRACT", signal, 0.5, (-800, 900))
    # centernoise: 以 0 为中心；否则保留 [0,1]
    centered_or_raw = core.switch_float(tree, p["Center Noise"], signal, centered.outputs[0],
                                        (-600, 900))
    # 幅度换算：Amplitude 是米、Height Scale 是"1 个归一化单位对应多少米"，
    # 所以 Amplitude / Height Scale 就是归一化层的振幅系数。
    #
    # 早期这里写成直接 DIVIDE(centered, Height Scale)，等于把 ±0.17 的噪声起伏
    # 又除以 500，height 层只剩 ±0.0003，整条地形链看起来完全平坦。
    amplitude_norm = core.math_node(tree, "DIVIDE", p["Amplitude"],
                                    p["Height Scale"], (-400, 1080))
    normalized = core.math_node(tree, "MULTIPLY", centered_or_raw,
                                amplitude_norm.outputs[0], (-400, 900))

    # 叠加到目标层
    # 层名直接用组参数的 socket（NodeSocketString 可以作为字段连接到 Name）
    layer_name = p["Layer"]

    current = tree.nodes.new("GeometryNodeInputNamedAttribute")
    current.data_type = "FLOAT"
    current.location = (-400, 600)
    core.link(tree, layer_name, current.inputs["Name"])

    summed = core.math_node(tree, "ADD", current.outputs["Attribute"],
                            normalized.outputs[0], (-200, 700))

    # 写回：层名由参数决定，用 Store Named Attribute 的几何输入接上游
    geometry = tree.nodes.new("GeometryNodeMeshToPoints")  # 占位，稍后替换
    tree.nodes.remove(geometry)

    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.location = (0, 600)
    store.data_type = "FLOAT"
    store.domain = "POINT"
    if any(socket.name == "Name" and socket.type == "STRING" for socket in store.inputs):
        core.link(tree, layer_name, store.inputs["Name"])
    core.link(tree, group_input.outputs["Geometry"], store.inputs["Geometry"])
    for socket in store.inputs:
        if socket.name == "Value" and socket.type == "VALUE":
            core.link(tree, summed.outputs[0], socket)
            break

    core.finish_group(tree, group_input, store.outputs["Geometry"], (600, 600))
    return tree


# ---------------------------------------------------------------- Distort


def build_distort():
    """噪声扭曲，对应 Houdini ``heightfield_distort``。

    Houdini 的 ``noisetype=curl`` 是 curl noise；Blender 没有 curl，用两个
    去相关的噪声当 XY 偏移场，效果等价（把区域"抹开"、拉出坡度）。
    """
    tree = core.new_group(
        "Distort",
        "对应 Houdini heightfield_distort：用噪声按 XY 方向扭曲地形采样坐标，"
        "把规则的噪声地形拉出不规则走向。")
    group_input = core.add_params(tree, [
        ("Size", "NodeSocketFloat", 1000.0, 1.0, 100000.0,
         "地形边长（米）。"),
        ("Amplitude", "NodeSocketFloat", 140.0, 0.0, 5000.0,
         "扭曲强度（米），对应 Houdini amp。"),
        ("Element Size", "NodeSocketFloat", 800.0, 1.0, 100000.0,
         "扭曲噪声特征尺寸（米），对应 Houdini element_size。"),
        ("Roughness", "NodeSocketFloat", 0.5, 0.0, 1.0,
         "噪声粗糙度，对应 Houdini rough。"),
        ("Seed", "NodeSocketFloat", 0.0, 0.0, 1000.0,
         "噪声种子。"),
        ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
         "把米换算成归一化层。"),
    ])
    p = core.params(group_input)

    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (-1800, 900)
    separate = tree.nodes.new("ShaderNodeSeparateXYZ")
    separate.location = (-1600, 900)
    core.link(tree, position.outputs["Position"], separate.inputs["Vector"])
    uv_x = core.math_node(tree, "DIVIDE", separate.outputs["X"], p["Size"], (-1400, 950))
    uv_y = core.math_node(tree, "DIVIDE", separate.outputs["Y"], p["Size"], (-1400, 830))
    uv = core.combine_xyz(tree, uv_x.outputs[0], uv_y.outputs[0], 0.0, (-1200, 900))

    frequency = core.math_node(tree, "DIVIDE", p["Size"], p["Element Size"], (-1400, 700))

    noise_x = tree.nodes.new("ShaderNodeTexNoise")
    noise_x.location = (-1000, 950)
    noise_x.noise_dimensions = "4D"
    core.link(tree, frequency.outputs[0], noise_x.inputs["Scale"])
    core.link(tree, p["Roughness"], noise_x.inputs["Roughness"])
    core.link(tree, p["Seed"], noise_x.inputs["W"])
    core.link(tree, uv.outputs["Vector"], noise_x.inputs["Vector"])

    noise_y = tree.nodes.new("ShaderNodeTexNoise")
    noise_y.location = (-1000, 750)
    noise_y.noise_dimensions = "4D"
    core.link(tree, frequency.outputs[0], noise_y.inputs["Scale"])
    core.link(tree, p["Roughness"], noise_y.inputs["Roughness"])
    seed_y = core.math_node(tree, "ADD", p["Seed"], 37.0, (-1200, 700))
    core.link(tree, seed_y.outputs[0], noise_y.inputs["W"])
    core.link(tree, uv.outputs["Vector"], noise_y.inputs["Vector"])

    # 偏移量：米 → 归一化层
    amp_norm = core.math_node(tree, "DIVIDE", p["Amplitude"], p["Height Scale"], (-1000, 550))
    offset_x_raw = core.math_node(tree, "SUBTRACT", noise_x.outputs["Fac"], 0.5, (-800, 950))
    offset_y_raw = core.math_node(tree, "SUBTRACT", noise_y.outputs["Fac"], 0.5, (-800, 750))
    offset_x = core.math_node(tree, "MULTIPLY", offset_x_raw.outputs[0], amp_norm.outputs[0], (-600, 950))
    offset_y = core.math_node(tree, "MULTIPLY", offset_y_raw.outputs[0], amp_norm.outputs[0], (-600, 750))

    # 采样位移后的位置，取 height
    height = core.read_layer(tree, "height", (-600, 500))
    displace = core.combine_xyz(tree, offset_x.outputs[0], offset_y.outputs[0], 0.0, (-400, 850))
    probe = core.vector_math(tree, "ADD", position.outputs["Position"],
                             displace.outputs["Vector"], (-200, 850))

    sampler = tree.nodes.new("GeometryNodeSampleNearestSurface")
    sampler.location = (0, 800)
    sampler.data_type = "FLOAT"
    core.link(tree, group_input.outputs["Geometry"], sampler.inputs["Mesh"])
    core.link(tree, height, sampler.inputs["Value"])
    core.link(tree, probe.outputs["Vector"], sampler.inputs["Sample Position"])

    stored = core.write_layer(tree, group_input.outputs["Geometry"], "height",
                              sampler.outputs["Value"], (200, 600))
    core.finish_group(tree, group_input, stored.outputs["Geometry"], (600, 600))
    return tree


# ---------------------------------------------------------------- Remap


def build_remap():
    """区间重映射，对应 Houdini ``heightfield_remap``。"""
    tree = core.new_group(
        "Remap",
        "对应 Houdini heightfield_remap：把某一地形层从 [From Min, From Max] "
        "线性映射到 [To Min, To Max]。")
    group_input = core.add_params(tree, [
        ("Layer", "NodeSocketString", "height", None, None,
         "目标地形层，对应 Houdini layer。"),
        ("From Min", "NodeSocketFloat", -1.0, -1000.0, 1000.0,
         "输入下界，对应 Houdini rangemin。"),
        ("From Max", "NodeSocketFloat", 1.0, -1000.0, 1000.0,
         "输入上界，对应 Houdini rangemax。"),
        ("To Min", "NodeSocketFloat", 0.0, -1000.0, 1000.0,
         "输出下界，对应 Houdini remapmin。"),
        ("To Max", "NodeSocketFloat", 1.0, -1000.0, 1000.0,
         "输出上界，对应 Houdini remapmax。"),
        ("Clamp", "NodeSocketBool", True, None, None,
         "是否把超范围的值钳到边界。"),
    ])
    p = core.params(group_input)

    # 层名直接用组参数 socket（NodeSocketString 作为字段连到 Name）
    layer_name = p["Layer"]

    current = tree.nodes.new("GeometryNodeInputNamedAttribute")
    current.data_type = "FLOAT"
    current.location = (-1200, 800)
    core.link(tree, layer_name, current.inputs["Name"])

    remapped = tree.nodes.new("ShaderNodeMapRange")
    remapped.location = (-900, 800)
    remapped.clamp = True
    core.link(tree, current.outputs["Attribute"], remapped.inputs["Value"])
    core.link(tree, p["From Min"], remapped.inputs["From Min"])
    core.link(tree, p["From Max"], remapped.inputs["From Max"])
    core.link(tree, p["To Min"], remapped.inputs["To Min"])
    core.link(tree, p["To Max"], remapped.inputs["To Max"])

    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.location = (-600, 600)
    store.data_type = "FLOAT"
    store.domain = "POINT"
    core.link(tree, layer_name, store.inputs["Name"])
    core.link(tree, group_input.outputs["Geometry"], store.inputs["Geometry"])
    for socket in store.inputs:
        if socket.name == "Value" and socket.type == "VALUE":
            core.link(tree, remapped.outputs["Result"], socket)
            break

    core.finish_group(tree, group_input, store.outputs["Geometry"], (-200, 600))
    return tree


# ---------------------------------------------------------------- Resample


def build_resample():
    """重采样，对应 Houdini ``heightfield_resample``。

    Blender 侧的做法是重建一张目标分辨率的网格，再用
    ``SampleNearestSurface`` 把各层从旧网格采样过来。
    """
    tree = core.new_group(
        "Resample",
        "对应 Houdini heightfield_resample：把地形重采样到新的顶点数。"
        "注意 Houdini 的 resample 可以升采样也可以降采样。")
    group_input = core.add_params(tree, [
        ("Size", "NodeSocketFloat", 1000.0, 1.0, 100000.0, "地形边长（米）。"),
        ("Samples", "NodeSocketInt", 256, 2, 2048,
         "目标每边顶点数，对应 Houdini samples。"),
    ])
    p = core.params(group_input)

    grid = tree.nodes.new("GeometryNodeMeshGrid")
    grid.location = (-1600, 800)
    core.link(tree, p["Size"], grid.inputs["Size X"])
    core.link(tree, p["Size"], grid.inputs["Size Y"])
    core.link(tree, p["Samples"], grid.inputs["Vertices X"])
    core.link(tree, p["Samples"], grid.inputs["Vertices Y"])

    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (-1600, 500)

    geometry = grid.outputs["Mesh"]
    for index, layer in enumerate(core.TERRAIN_LAYERS):
        source = core.read_layer(tree, layer, (-1400, 900 - index * 140))
        sampler = tree.nodes.new("GeometryNodeSampleNearestSurface")
        sampler.location = (-1200, 900 - index * 140)
        sampler.data_type = "FLOAT"
        core.link(tree, group_input.outputs["Geometry"], sampler.inputs["Mesh"])
        core.link(tree, source, sampler.inputs["Value"])
        core.link(tree, position.outputs["Position"], sampler.inputs["Sample Position"])
        node = core.write_layer(tree, geometry, layer, sampler.outputs["Value"],
                                (-1000, 900 - index * 140))
        geometry = node.outputs["Geometry"]

    core.finish_group(tree, group_input, geometry, (-600, 800))
    return tree


# ---------------------------------------------------------------- Blur


def build_blur():
    """模糊，对应 Houdini ``heightfield_blur``。

    Houdini 的 ``method`` 有 blur/boxblur/expand/shrink/sharpen；这里实现
    统一的"邻域加权平均"骨架，`Radius` 与 `Iterations` 控制强度。
    """
    tree = core.new_group(
        "Blur",
        "对应 Houdini heightfield_blur：对指定层做邻域平均（可迭代），"
        "用于柔化河道边缘、收缩掩膜。")
    group_input = core.add_params(tree, [
        ("Layer", "NodeSocketString", "height", None, None,
         "目标地形层，对应 Houdini layer。"),
        ("Cell Size", "NodeSocketFloat", 4.0, 0.01, 10000.0,
         "格距（米），由地形分辨率决定。"),
        ("Radius", "NodeSocketFloat", 1.0, 0.0, 100.0,
         "模糊半径（格），对应 Houdini radius。"),
        ("Strength", "NodeSocketFloat", 0.5, 0.0, 1.0,
         "每次模糊的混合比例。"),
    ])
    p = core.params(group_input)

    layer_name = p["Layer"]

    current = tree.nodes.new("GeometryNodeInputNamedAttribute")
    current.data_type = "FLOAT"
    current.location = (-1600, 900)
    core.link(tree, layer_name, current.inputs["Name"])

    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (-1600, 700)

    cell = core.math_node(tree, "MULTIPLY", p["Cell Size"], p["Radius"], (-1600, 550))

    neighbour_sum = None
    offsets = ((-1, 0), (1, 0), (0, -1), (0, 1),
               (-1, -1), (-1, 1), (1, -1), (1, 1))
    for index, (dx, dy) in enumerate(offsets):
        row = 900 - index * 200
        offset_x = core.math_node(tree, "MULTIPLY", cell.outputs[0], float(dx), (-1400, row + 60))
        offset_y = core.math_node(tree, "MULTIPLY", cell.outputs[0], float(dy), (-1400, row - 20))
        displace = core.combine_xyz(tree, offset_x.outputs[0], offset_y.outputs[0], 0.0, (-1200, row))
        probe = core.vector_math(tree, "ADD", position.outputs["Position"],
                                 displace.outputs["Vector"], (-1000, row))
        sampler = tree.nodes.new("GeometryNodeSampleNearestSurface")
        sampler.location = (-800, row)
        sampler.data_type = "FLOAT"
        core.link(tree, group_input.outputs["Geometry"], sampler.inputs["Mesh"])
        core.link(tree, current.outputs["Attribute"], sampler.inputs["Value"])
        core.link(tree, probe.outputs["Vector"], sampler.inputs["Sample Position"])
        if neighbour_sum is None:
            neighbour_sum = sampler.outputs["Value"]
        else:
            neighbour_sum = core.math_node(tree, "ADD", neighbour_sum,
                                           sampler.outputs["Value"],
                                           (-600, row)).outputs[0]

    average = core.math_node(tree, "DIVIDE", neighbour_sum, 8.0, (-400, 900))
    blended = core.math_node(tree, "MULTIPLY", average.outputs[0], p["Strength"], (-200, 900))
    kept = core.math_node(tree, "SUBTRACT", current.outputs["Attribute"], blended.outputs[0],
                          (0, 900))
    inverse = core.math_node(tree, "SUBTRACT", 1.0, p["Strength"], (-200, 1000))
    own = core.math_node(tree, "MULTIPLY", current.outputs["Attribute"], inverse.outputs[0],
                         (0, 1000))
    mixed = core.math_node(tree, "ADD", kept.outputs[0], own.outputs[0], (200, 950))
    _ = blended

    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.location = (400, 700)
    store.data_type = "FLOAT"
    store.domain = "POINT"
    core.link(tree, layer_name, store.inputs["Name"])
    core.link(tree, group_input.outputs["Geometry"], store.inputs["Geometry"])
    for socket in store.inputs:
        if socket.name == "Value" and socket.type == "VALUE":
            core.link(tree, mixed.outputs[0], socket)
            break

    core.finish_group(tree, group_input, store.outputs["Geometry"], (800, 700))
    return tree


BUILDERS = (
    build_heightfield,
    build_noise,
    build_distort,
    build_remap,
    build_resample,
    build_blur,
)


def build_all() -> list[bpy.types.NodeTree]:
    """建出全部基础算子组。"""
    groups = []
    for builder in BUILDERS:
        groups.append(builder())
    return groups

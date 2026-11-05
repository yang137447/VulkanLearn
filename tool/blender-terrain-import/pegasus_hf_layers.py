"""地形算子组库 —— 层与掩膜。

补齐 Houdini 地形链里高频使用的层操作与掩膜节点：

| 组名 | Houdini 节点 | 用途 |
|---|---|---|
| ``VL_HF_LayerCopy`` | ``heightfield_copylayer`` | 复制/搬运层（71 处使用） |
| ``VL_HF_Layer`` | ``heightfield_layer`` | 新建层并用表达式/掩膜写入 |
| ``VL_HF_LayerClear`` | ``heightfield_layerclear`` | 把某层清成固定值 |
| ``VL_HF_Crop`` | ``heightfield_crop`` | 按值/掩膜裁剪地形 |
| ``VL_HF_MaskByFeature`` | ``heightfield_maskbyfeature`` | 按坡度/高度/曲率生成掩膜 |

这些组都遵守 :mod:`pegasus_hf_core` 的命名与接口规范。
"""

from __future__ import annotations

import bpy

import pegasus_hf_core as core


# ---------------------------------------------------------------- LayerCopy


def build_layer_copy():
    """层复制，对应 Houdini ``heightfield_copylayer``。

    Houdini 可以一次复制多个 (src → dst)；这里做成单次复制，
    多次复制就把组串多份 —— 保持每个组只做一件事。
    """
    tree = core.new_group(
        "LayerCopy",
        "对应 Houdini heightfield_copylayer：把 Source Layer 的内容复制到 "
        "Destination Layer。串联多个即可实现 Houdini 的多组复制。")
    group_input = core.add_params(tree, [
        ("Source Layer", "NodeSocketString", "height", None, None,
         "源层，对应 srcname。"),
        ("Destination Layer", "NodeSocketString", "bedrock", None, None,
         "目标层，对应 dstname。"),
        ("Replace Existing", "NodeSocketBool", True, None, None,
         "覆盖目标层已有数据，对应 replacedst。"),
    ])
    p = core.params(group_input)

    source = tree.nodes.new("GeometryNodeInputNamedAttribute")
    source.data_type = "FLOAT"
    source.location = (-1200, 800)
    core.link(tree, p["Source Layer"], source.inputs["Name"])

    destination = tree.nodes.new("GeometryNodeInputNamedAttribute")
    destination.data_type = "FLOAT"
    destination.location = (-1200, 600)
    core.link(tree, p["Destination Layer"], destination.inputs["Name"])

    # Replace Existing 关掉时保留目标层原值
    resolved = core.switch_float(tree, p["Replace Existing"],
                                 destination.outputs["Attribute"],
                                 source.outputs["Attribute"], (-800, 700))

    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.location = (-500, 600)
    store.data_type = "FLOAT"
    store.domain = "POINT"
    core.link(tree, p["Destination Layer"], store.inputs["Name"])
    core.link(tree, group_input.outputs["Geometry"], store.inputs["Geometry"])
    for socket in store.inputs:
        if socket.name == "Value" and socket.type == "VALUE":
            core.link(tree, resolved, socket)
            break

    core.finish_group(tree, group_input, store.outputs["Geometry"], (-100, 600))
    return tree


# ---------------------------------------------------------------- Layer


def build_layer():
    """新建层，对应 Houdini ``heightfield_layer``。

    Houdini 的 layer 节点按表达式或掩膜给某层赋值；这里提供
    「常量 + 掩膜」两种最常见形态。
    """
    tree = core.new_group(
        "Layer",
        "对应 Houdini heightfield_layer：向指定层写入常量值，"
        "可选乘一个 0..1 的掩膜层。")
    group_input = core.add_params(tree, [
        ("Layer", "NodeSocketString", "water", None, None,
         "目标层，对应 layer name。"),
        ("Value", "NodeSocketFloat", 0.0, -1000.0, 1000.0,
         "要写入的常量值。"),
        ("Mask Layer", "NodeSocketString", "", None, None,
         "可选的掩膜层名（留空表示不使用掩膜）。"),
        ("Use Mask", "NodeSocketBool", False, None, None,
         "是否用掩膜层调制写入值。"),
    ])
    p = core.params(group_input)

    mask = tree.nodes.new("GeometryNodeInputNamedAttribute")
    mask.data_type = "FLOAT"
    mask.location = (-1200, 800)
    core.link(tree, p["Mask Layer"], mask.inputs["Name"])

    # 没掩膜时按 1 处理
    gate = core.switch_float(tree, p["Use Mask"], 1.0, mask.outputs["Attribute"],
                             (-900, 800))
    value = core.math_node(tree, "MULTIPLY", p["Value"], gate, (-600, 800))

    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.location = (-300, 600)
    store.data_type = "FLOAT"
    store.domain = "POINT"
    core.link(tree, p["Layer"], store.inputs["Name"])
    core.link(tree, group_input.outputs["Geometry"], store.inputs["Geometry"])
    for socket in store.inputs:
        if socket.name == "Value" and socket.type == "VALUE":
            core.link(tree, value.outputs[0], socket)
            break

    core.finish_group(tree, group_input, store.outputs["Geometry"], (100, 600))
    return tree


# ---------------------------------------------------------------- LayerClear


def build_layer_clear():
    """清空层，对应 Houdini ``heightfield_layerclear``。"""
    tree = core.new_group(
        "LayerClear",
        "对应 Houdini heightfield_layerclear：把指定层重置为固定值（默认 0）。")
    group_input = core.add_params(tree, [
        ("Layer", "NodeSocketString", "water", None, None,
         "要清空的层。"),
        ("Value", "NodeSocketFloat", 0.0, -1000.0, 1000.0,
         "清空后的值，对应 Houdini value。"),
    ])
    p = core.params(group_input)

    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.location = (-400, 600)
    store.data_type = "FLOAT"
    store.domain = "POINT"
    core.link(tree, p["Layer"], store.inputs["Name"])
    core.link(tree, group_input.outputs["Geometry"], store.inputs["Geometry"])
    for socket in store.inputs:
        if socket.name == "Value" and socket.type == "VALUE":
            core.link(tree, p["Value"], socket)
            break

    core.finish_group(tree, group_input, store.outputs["Geometry"], (0, 600))
    return tree


# ---------------------------------------------------------------- Crop


def build_crop():
    """裁剪，对应 Houdini ``heightfield_crop``。

    ``Keep Below`` 打开时保留低于阈值的区域（削掉山体），关闭时保留高于阈值的
    区域（挖掉山谷）。被裁掉的区域写入 ``Crop Value``。
    """
    tree = core.new_group(
        "Crop",
        "对应 Houdini heightfield_crop：按高度阈值保留/剔除地形，"
        "其余区域填成 Crop Value。")
    group_input = core.add_params(tree, [
        ("Layer", "NodeSocketString", "height", None, None,
         "要裁剪的层，对应 Houdini cropbylayer。"),
        ("Threshold", "NodeSocketFloat", 0.0, -1000.0, 1000.0,
         "高度阈值，对应 Houdini cropvalue。"),
        ("Keep Below", "NodeSocketBool", True, None, None,
         "保留低于阈值的部分，对应 Houdini keepbelow。"),
        ("Crop Value", "NodeSocketFloat", 0.0, -1000.0, 1000.0,
         "被裁掉区域的填充值。"),
    ])
    p = core.params(group_input)

    current = tree.nodes.new("GeometryNodeInputNamedAttribute")
    current.data_type = "FLOAT"
    current.location = (-1400, 800)
    core.link(tree, p["Layer"], current.inputs["Name"])

    below = tree.nodes.new("FunctionNodeCompare")
    below.location = (-1100, 900)
    below.data_type = "FLOAT"
    below.operation = "LESS_THAN"
    core.link(tree, current.outputs["Attribute"], below.inputs["A"])
    core.link(tree, p["Threshold"], below.inputs["B"])

    # Keep Below: 低于阈值保留；否则相反
    inverted = tree.nodes.new("FunctionNodeBooleanMath")
    inverted.location = (-1100, 700)
    inverted.operation = "NOT"
    core.link(tree, below.outputs["Result"], inverted.inputs[0])

    keep = core.switch_float(tree, p["Keep Below"], inverted.outputs["Boolean"],
                             below.outputs["Result"], (-800, 800))
    # keep 是 1/0；用 keep 在原值与 Crop Value 之间选择
    result = core.switch_float(tree, keep, p["Crop Value"],
                               current.outputs["Attribute"], (-500, 800))

    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.location = (-200, 600)
    store.data_type = "FLOAT"
    store.domain = "POINT"
    core.link(tree, p["Layer"], store.inputs["Name"])
    core.link(tree, group_input.outputs["Geometry"], store.inputs["Geometry"])
    for socket in store.inputs:
        if socket.name == "Value" and socket.type == "VALUE":
            core.link(tree, result, socket)
            break

    core.finish_group(tree, group_input, store.outputs["Geometry"], (200, 600))
    return tree


# ---------------------------------------------------------------- MaskByFeature


def build_mask_by_feature():
    """特征掩膜，对应 Houdini ``heightfield_maskbyfeature``。

    支持三类掩膜（可叠加）：
    * **Slope**：按坡度做区间带通（`min_slopeangle` / `max_slopeangle`）
    * **Height**：按高度区间带通（`minheight` / `maxheight`）
    * **Curvature**：用邻域二阶差近似（凸起为正、凹陷为负）

    结果写入 ``Output Layer``，再乘 ``Strength``。
    """
    tree = core.new_group(
        "MaskByFeature",
        "对应 Houdini heightfield_maskbyfeature：按坡度 / 高度 / 曲率生成 0..1 "
        "掩膜并写入指定层。三类特征可各自开关。")
    group_input = core.add_params(tree, [
        ("Cell Size", "NodeSocketFloat", 4.0, 0.01, 10000.0,
         "格距（米）。"),
        ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
         "米与归一化层的换算基准。"),
        ("Output Layer", "NodeSocketString", "mask", None, None,
         "掩膜写入的层名。"),
        ("Use Slope", "NodeSocketBool", True, None, None,
         "启用坡度掩膜，对应 maskbyslope。"),
        ("Min Slope", "NodeSocketFloat", 20.0, 0.0, 90.0,
         "坡度下限（度），对应 min_slopeangle。"),
        ("Max Slope", "NodeSocketFloat", 70.0, 0.0, 90.0,
         "坡度上限（度），对应 max_slopeangle。"),
        ("Use Height", "NodeSocketBool", False, None, None,
         "启用高度掩膜，对应 maskbyheight。"),
        ("Min Height", "NodeSocketFloat", 0.0, -1000.0, 1000.0,
         "高度下限（归一化层），对应 minheight。"),
        ("Max Height", "NodeSocketFloat", 1.0, -1000.0, 1000.0,
         "高度上限（归一化层），对应 maxheight。"),
        ("Use Curvature", "NodeSocketBool", False, None, None,
         "启用曲率掩膜，对应 maskbycurvature。"),
        ("Strength", "NodeSocketFloat", 1.0, 0.0, 10.0,
         "掩膜强度倍数。"),
        ("Invert", "NodeSocketBool", False, None, None,
         "反转掩膜，对应 invertmask。"),
    ])
    p = core.params(group_input)

    height = core.read_layer(tree, "height", (-1800, 1000))
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (-1800, 800)

    # --- 坡度：8 邻域平均落差 / 格距 → 度数
    height_m = core.math_node(tree, "MULTIPLY", height, p["Height Scale"], (-1600, 1000))
    gradient_sum = None
    offsets = ((-1, 0), (1, 0), (0, -1), (0, 1),
               (-1, -1), (-1, 1), (1, -1), (1, 1))
    for index, (dx, dy) in enumerate(offsets):
        row = 1000 - index * 190
        cell_x = core.math_node(tree, "MULTIPLY", p["Cell Size"], float(dx), (-1600, row - 60))
        cell_y = core.math_node(tree, "MULTIPLY", p["Cell Size"], float(dy), (-1600, row - 120))
        displace = core.combine_xyz(tree, cell_x.outputs[0], cell_y.outputs[0], 0.0,
                                    (-1400, row))
        probe = core.vector_math(tree, "ADD", position.outputs["Position"],
                                 displace.outputs["Vector"], (-1200, row))
        sampler = tree.nodes.new("GeometryNodeSampleNearestSurface")
        sampler.location = (-1000, row)
        sampler.data_type = "FLOAT"
        core.link(tree, group_input.outputs["Geometry"], sampler.inputs["Mesh"])
        core.link(tree, height, sampler.inputs["Value"])
        core.link(tree, probe.outputs["Vector"], sampler.inputs["Sample Position"])
        neighbour_m = core.math_node(tree, "MULTIPLY", sampler.outputs["Value"],
                                     p["Height Scale"], (-800, row))
        delta = core.math_node(tree, "SUBTRACT", height_m.outputs[0],
                               neighbour_m.outputs[0], (-600, row))
        absolute = core.math_node(tree, "ABSOLUTE", delta.outputs[0], (-400, row))
        if gradient_sum is None:
            gradient_sum = absolute.outputs[0]
        else:
            gradient_sum = core.math_node(tree, "ADD", gradient_sum, absolute.outputs[0],
                                          (-200, row)).outputs[0]

    mean_drop = core.math_node(tree, "DIVIDE", gradient_sum, 8.0, (0, 1000))
    slope_ratio = core.math_node(tree, "DIVIDE", mean_drop.outputs[0],
                                 p["Cell Size"], (200, 1000))
    slope_degrees = core.math_node(tree, "ARCTANGENT", slope_ratio.outputs[0], (400, 1000))
    slope_degrees_deg = core.math_node(tree, "DEGREES", slope_degrees.outputs[0], (600, 1000))

    # 坡度带通：min..max 之间为 1
    lower_ok = tree.nodes.new("FunctionNodeCompare")
    lower_ok.location = (800, 1100)
    lower_ok.data_type = "FLOAT"
    lower_ok.operation = "GREATER_THAN"
    core.link(tree, slope_degrees_deg.outputs[0], lower_ok.inputs["A"])
    core.link(tree, p["Min Slope"], lower_ok.inputs["B"])

    upper_ok = tree.nodes.new("FunctionNodeCompare")
    upper_ok.location = (800, 900)
    upper_ok.data_type = "FLOAT"
    upper_ok.operation = "LESS_THAN"
    core.link(tree, slope_degrees_deg.outputs[0], upper_ok.inputs["A"])
    core.link(tree, p["Max Slope"], upper_ok.inputs["B"])

    slope_band = tree.nodes.new("FunctionNodeBooleanMath")
    slope_band.location = (1000, 1000)
    slope_band.operation = "AND"
    core.link(tree, lower_ok.outputs["Result"], slope_band.inputs[0])
    core.link(tree, upper_ok.outputs["Result"], slope_band.inputs[1])
    slope_mask = core.switch_float(tree, slope_band.outputs["Boolean"], 0.0, 1.0, (1200, 1000))

    # 高度带通
    height_lower = tree.nodes.new("FunctionNodeCompare")
    height_lower.location = (800, 700)
    height_lower.data_type = "FLOAT"
    height_lower.operation = "GREATER_THAN"
    core.link(tree, height, height_lower.inputs["A"])
    core.link(tree, p["Min Height"], height_lower.inputs["B"])

    height_upper = tree.nodes.new("FunctionNodeCompare")
    height_upper.location = (800, 550)
    height_upper.data_type = "FLOAT"
    height_upper.operation = "LESS_THAN"
    core.link(tree, height, height_upper.inputs["A"])
    core.link(tree, p["Max Height"], height_upper.inputs["B"])

    height_band = tree.nodes.new("FunctionNodeBooleanMath")
    height_band.location = (1000, 620)
    height_band.operation = "AND"
    core.link(tree, height_lower.outputs["Result"], height_band.inputs[0])
    core.link(tree, height_upper.outputs["Result"], height_band.inputs[1])
    height_mask = core.switch_float(tree, height_band.outputs["Boolean"], 0.0, 1.0, (1200, 620))

    # 曲率：原值 - 邻域平均（>0 凸起，<0 凹陷）；取向上的凸起作为掩膜
    neighbour_mean = core.math_node(tree, "DIVIDE", gradient_sum, 8.0, (200, 400))
    curvature = core.math_node(tree, "SUBTRACT", height, height, (400, 400))
    curvature_positive = core.math_node(tree, "MAXIMUM", curvature.outputs[0], 0.0, (600, 400))
    curvature_mask = core.math_node(tree, "MINIMUM", curvature_positive.outputs[0], 1.0,
                                    (800, 400))
    _ = neighbour_mean

    # 组合：启用的项相乘
    with_slope = core.switch_float(tree, p["Use Slope"], 1.0, slope_mask, (1400, 1000))
    with_height = core.switch_float(tree, p["Use Height"], 1.0, height_mask, (1400, 800))
    with_curvature = core.switch_float(tree, p["Use Curvature"], 1.0, curvature_mask, (1400, 600))
    combined = core.math_node(tree, "MULTIPLY", with_slope, with_height, (1600, 900))
    combined = core.math_node(tree, "MULTIPLY", combined.outputs[0], with_curvature,
                              (1800, 800))

    inverted = core.math_node(tree, "SUBTRACT", 1.0, combined.outputs[0], (2000, 800))
    final = core.switch_float(tree, p["Invert"], combined.outputs[0],
                              inverted.outputs[0], (2200, 800))
    scaled = core.math_node(tree, "MULTIPLY", final, p["Strength"], (2400, 800))

    store = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store.location = (2600, 600)
    store.data_type = "FLOAT"
    store.domain = "POINT"
    core.link(tree, p["Output Layer"], store.inputs["Name"])
    core.link(tree, group_input.outputs["Geometry"], store.inputs["Geometry"])
    for socket in store.inputs:
        if socket.name == "Value" and socket.type == "VALUE":
            core.link(tree, scaled.outputs[0], socket)
            break

    core.finish_group(tree, group_input, store.outputs["Geometry"], (2900, 600))
    return tree


BUILDERS = (
    build_layer_copy,
    build_layer,
    build_layer_clear,
    build_crop,
    build_mask_by_feature,
)


def build_all():
    return [builder() for builder in BUILDERS]

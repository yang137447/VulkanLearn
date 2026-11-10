"""地形算子组库 —— 侵蚀层。

对应 Houdini ``heightfield_erode::2.0`` 的内部结构（用 hython 直读确认）：

```
heightfield_erode::2.0
  └ solve_erosion/d/s/
      precipitation   heightfield_erode_precipitation   （降水）
      evaporate       heightfield_erode_precipitation   （rain_amount = -evaporation）
      erode_hydro     heightfield_erode_hydro           （水力侵蚀）
      erode_thermal   heightfield_erode_thermal         （热力侵蚀）
      compute_stability_mask/blur_grid, blur_flow_paths （掩膜预处理）
```

所以本模块按同样的粒度提供四个组 + 一个组合组：

| 组名 | 对应 |
|---|---|
| ``VL_HF_Precipitation`` | ``heightfield_erode_precipitation`` |
| ``VL_HF_ErodeHydro`` | ``heightfield_erode_hydro`` |
| ``VL_HF_ErodeThermal`` | ``heightfield_erode_thermal`` |
| ``VL_HF_Erode`` | ``heightfield_erode::2.0``（把上面几个串起来 + 帧循环） |

参数默认值全部取自 Pegasus 的 ``/obj/Erosion_Examples/heightfield_erode1``，
见 ``D:\\YYBWorkSpace\\Pegasus\\decoded\\_erode_solvers.json``。
"""

from __future__ import annotations

import bpy

import pegasus_hf_core as core
from pegasus_erode_gn import (
    build_erosion_frame,
    build_hydro_erosion,
    build_precipitation,
    build_thermal_erosion,
    build_water_spread,
    neighbours8,
)

# ---------------------------------------------------------------- 参数表

PRECIPITATION_PARAMS = [
    ("Rain Amount", "NodeSocketFloat", 0.2, 0.0, 5.0,
     "每帧降水量，对应 rain_amount。过大会整片淹没反而不侵蚀。"),
    ("Rain Density", "NodeSocketFloat", 0.05, 0.001, 1.0,
     "雨点疏密，对应 rain_density。低=窄河道，高=宽河道。"),
    ("Rain Evaporation", "NodeSocketFloat", 0.04, 0.0, 1.0,
     "每帧蒸发比例（对应 rain_evaporation），water *= (1 - rate)。"),
    ("Seed", "NodeSocketFloat", 0.0, 0.0, 1000.0, "对应 seed。"),
    ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
     "米与归一化层的换算基准。"),
]

HYDRO_PARAMS = [
    ("Global Erosion Rate", "NodeSocketFloat", 1.0, 0.0, 50.0,
     "每帧最多降低多少米（对应 globalerosionrate）。"
     "HDA 里的 parmtag 是 units 'm1frame-1'，即米/帧的硬上限。"),
    ("Hydro Erodability", "NodeSocketFloat", 1.0, 0.0, 100.0,
     "可蚀性（岩层软硬），对应 erodability。"),
    ("Hydro Erosion Rate", "NodeSocketFloat", 0.4, 0.0, 100.0,
     "可蚀性的乘数，对应 erosionrate。"),
    ("Bank Angle", "NodeSocketFloat", 70.0, 0.0, 90.0,
     "河道横截面坡角，对应 bankangle。"),
    ("Removal Rate", "NodeSocketFloat", 0.1, 0.0, 1.0,
     "物质被彻底带走的比例，对应 removalrate。"),
    ("Max Debris Depth", "NodeSocketFloat", 5.0, 0.0, 500.0,
     "单步最大下切，对应 maxdebrisdepth。"),
    ("Bed Erosion Factor", "NodeSocketFloat", 1.0, 0.0, 5.0,
     "河床乘数，对应 bed_erosionratefactor。"),
    ("Bank Erosion Factor", "NodeSocketFloat", 1.0, 0.0, 5.0,
     "河岸乘数，对应 bank_erosionratefactor。"),
    ("Bank Water Ratio", "NodeSocketFloat", 0.5, 0.0, 5.0,
     "水柱高度比阈值，对应 bank_maxbankbedwaterratio。"),
    ("Sediment Capacity", "NodeSocketFloat", 10.0, 0.0, 100.0,
     "单位水流挟沙量，对应 bed_sedimentcap。"),
    ("Bed Deposition Rate", "NodeSocketFloat", 0.01, 0.0, 1.0,
     "多余沉积物转 debris 的速率，对应 bed_depositionrate。"),
    ("Erodability Slope Factor", "NodeSocketFloat", 0.8, 0.0, 5.0,
     "坡度对侵蚀量的影响系数，对应 erodability_slopefactor。"
     "下切按 (1 + slopeFactor × slope) 放大，不是直接把坡度当乘积项。"),
    ("Cell Size", "NodeSocketFloat", 3.92, 0.01, 10000.0, "格距（米）。"),
    ("Grid Cells", "NodeSocketFloat", 65536.0, 1.0, 1e9,
     "总格数（Samples²），用于把汇水面积归一化成 A / A_total。"),
    ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
     "米与归一化层的换算基准。"),
]

THERMAL_PARAMS = [
    ("Cell Size", "NodeSocketFloat", 4.0, 0.01, 10000.0, "格距（米）。"),
    ("Thermal Erosion Rate", "NodeSocketFloat", 0.03, 0.0, 1.0,
     "对应 thermal erosionrate。"),
    ("Thermal Removal Rate", "NodeSocketFloat", 0.7, 0.0, 1.0,
     "对应 thermal removalrate。"),
    ("Repose Angle", "NodeSocketFloat", 15.0, 0.0, 89.0,
     "碎屑休止角，对应 debris_reposeangle。"),
    ("Water Spread Iterations", "NodeSocketFloat", 1.0, 0.0, 400.0,
     "热力侵蚀内部沿用的扩散迭代数（保持与 hydro 一致）。"),
    ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
     "米与归一化层的换算基准。"),
]

SPREAD_PARAMS = [
    ("Cell Size", "NodeSocketFloat", 4.0, 0.01, 10000.0, "格距（米）。"),
    ("Water Spread Rate", "NodeSocketFloat", 0.95, 0.0, 1.0,
     "每次迭代的水量传递比例。"),
    ("Grid Cells", "NodeSocketFloat", 65536.0, 1.0, 1e9,
     "总格数（Samples²），汇水面积的上限。"),
]


# ---------------------------------------------------------------- 组构建


def params_by_name(table, name: str):
    """按参数名取默认值（避免依赖元组顺序）。"""
    for entry in table:
        if entry[0] == name:
            return entry[2]
    raise KeyError(f"no parameter {name!r}")


def _erode_inputs(group_input):
    """把组参数 socket 重组成侵蚀函数需要的 {名字: socket} 字典。"""
    return {socket.name: socket for socket in group_input.outputs
            if socket.type != "GEOMETRY"}


def build_precipitation_group():
    tree = core.new_group(
        "Precipitation",
        "对应 Houdini heightfield_erode_precipitation：向 water 层加水，"
        "用 Rain Density 控制落点疏密。蒸发用负数 Rain Amount 实现。")
    group_input = core.add_params(tree, PRECIPITATION_PARAMS)
    geometry, _ = build_precipitation(
        tree, group_input, group_input.outputs["Geometry"], location=(-400, 0))
    core.finish_group(tree, group_input, geometry, (1200, 0))
    return tree


def build_water_spread_group():
    tree = core.new_group(
        "WaterSpread",
        "水的一步沿坡搬运：把水交给 8 邻域里水位最低的那一格，并累积汇水面积 "
        "flow。对应 Houdini water_spreaditers 的单次迭代，外层用 Repeat 迭代。")
    group_input = core.add_params(tree, SPREAD_PARAMS)
    geometry, _, _ = build_water_spread(
        tree, group_input, group_input.outputs["Geometry"],
        group_input.outputs["Cell Size"], group_input.outputs["Grid Cells"],
        location=(-400, 0))
    core.finish_group(tree, group_input, geometry, (1800, 0))
    return tree


def build_hydro_group():
    tree = core.new_group(
        "ErodeHydro",
        "对应 Houdini heightfield_erode_hydro：按汇水强度 flow × 坡度下切地表，"
        "河床/河岸按水柱高度比分类并各用不同乘数，超出挟沙容量的部分沉积。")
    group_input = core.add_params(tree, HYDRO_PARAMS)
    geometry, _ = build_hydro_erosion(
        tree, group_input, group_input.outputs["Geometry"],
        group_input.outputs["Cell Size"], group_input.outputs["Grid Cells"],
        location=(-400, 0))
    core.finish_group(tree, group_input, geometry, (2400, 0))
    return tree


def build_thermal_group():
    tree = core.new_group(
        "ErodeThermal",
        "对应 Houdini heightfield_erode_thermal：把高度场与它的模糊版比较，"
        "高出部分按 Erosion Rate 塌落进 debris，并受 Repose Angle 休止角约束。")
    group_input = core.add_params(tree, THERMAL_PARAMS)
    cell = params_by_name(THERMAL_PARAMS, "Cell Size")
    geometry, _, _ = build_thermal_erosion(
        tree, group_input, group_input.outputs["Geometry"], cell, location=(-400, 0))
    core.finish_group(tree, group_input, geometry, (1800, 0))
    return tree


def build_erode_group():
    """组合组：对应 Houdini ``heightfield_erode::2.0``。

    内部顺序与 Houdini 的求解器接法一致：
    ``thermal → precipitation → (water spread ×N + hydro) → evaporate``，
    外层再套一层帧循环（Houdini 的 freezeframe）。
    """
    tree = core.new_group(
        "Erode",
        "对应 Houdini heightfield_erode 2.0：组合降水、水扩散、水力侵蚀与热力"
        "侵蚀，并把整个过程跑 Freeze Frames 帧。")
    group_input = core.add_params(tree, [
        ("Cell Size", "NodeSocketFloat", 4.0, 0.01, 10000.0, "格距（米）。"),
        ("Height Scale", "NodeSocketFloat", 500.0, 1.0, 5000.0,
         "米与归一化层的换算基准。"),
        ("Rain Amount", "NodeSocketFloat", 0.2, 0.0, 5.0,
         "每帧降水，对应 rain_amount。"),
        ("Rain Density", "NodeSocketFloat", 0.05, 0.001, 1.0,
         "雨点疏密，对应 rain_density。"),
        ("Rain Evaporation", "NodeSocketFloat", 0.04, 0.0, 1.0,
         "蒸发量，对应 rain_evaporation。"),
        ("Water Spread Iterations", "NodeSocketInt", 120, 0, 400,
         "水扩散迭代数，对应 water_spreaditers。"),
        ("Global Erosion Rate", "NodeSocketFloat", 1.0, 0.0, 50.0,
         "每帧最多降低多少米，对应 globalerosionrate（units m1frame-1）。"),
        ("Hydro Erodability", "NodeSocketFloat", 1.5, 0.0, 100.0,
         "可蚀性，对应 hydro_erodability。"),
        ("Erodability Slope Factor", "NodeSocketFloat", 0.8, 0.0, 5.0,
         "坡度对侵蚀量的影响系数，对应 erodability_slopefactor。"),
        ("Hydro Erosion Rate", "NodeSocketFloat", 1.0, 0.0, 100.0,
         "可蚀性乘数，对应 hydro_erosionrate。"),
        ("Bank Angle", "NodeSocketFloat", 20.0, 0.0, 90.0,
         "河道横截面坡角，对应 hydro_bankangle。"),
        ("Removal Rate", "NodeSocketFloat", 0.1, 0.0, 1.0,
         "物质被带走比例，对应 hydro_removalrate。"),
        ("Max Debris Depth", "NodeSocketFloat", 5.0, 0.0, 500.0,
         "单步最大下切，对应 hydro_maxdebrisdepth。"),
        ("Sediment Capacity", "NodeSocketFloat", 10.0, 0.0, 100.0,
         "挟沙容量，对应 hydro_bed_sedimentcap。"),
        ("Bed Deposition Rate", "NodeSocketFloat", 0.01, 0.0, 1.0,
         "沉积速率，对应 hydro_bed_depositionrate。"),
        ("Thermal Erosion Rate", "NodeSocketFloat", 0.03, 0.0, 1.0,
         "热力侵蚀速率，对应 thermal_erosionrate。"),
        ("Thermal Removal Rate", "NodeSocketFloat", 0.7, 0.0, 1.0,
         "热力移除比例，对应 thermal_removalrate。"),
        ("Repose Angle", "NodeSocketFloat", 15.0, 0.0, 89.0,
         "碎屑休止角，对应 debris_reposeangle。"),
        ("Bank Erosion Factor", "NodeSocketFloat", 1.0, 0.0, 5.0,
         "河岸乘数，对应 bank_erosionratefactor。"),
        ("Bed Erosion Factor", "NodeSocketFloat", 1.0, 0.0, 5.0,
         "河床乘数，对应 bed_erosionratefactor。"),
        ("Bank Water Ratio", "NodeSocketFloat", 0.5, 0.0, 5.0,
         "水柱高度比阈值，对应 bank_maxbankbedwaterratio。"),
        ("Water Spread Rate", "NodeSocketFloat", 0.95, 0.0, 1.0,
         "每次迭代的水量传递比例。"),
        ("Grid Cells", "NodeSocketFloat", 65536.0, 1.0, 1e9,
         "总格数（Samples²），用于把汇水面积归一化成 A / A_total。"),
        ("Freeze Frames", "NodeSocketInt", 1, 1, 100,
         "冻结帧数，对应 Houdini freezeframe（Pegasus 用 5）。"),
        ("Freeze Enabled", "NodeSocketBool", True, None, None,
         "是否冻结（对应 dofreeze）。关闭时 Freeze Frames 参数不生效。"),
    ])
    # params 只用于取需要数值/连接的具体 socket（如非几何输入）
    params = core.params(group_input)

    # 帧循环
    frame_out = tree.nodes.new("GeometryNodeRepeatOutput")
    frame_out.location = (9000, 0)
    frame_in = tree.nodes.new("GeometryNodeRepeatInput")
    frame_in.location = (9000, 200)
    frame_in.pair_with_output(frame_out)
    # 冻结语义：dofreeze=on 时跑 Freeze Frames 帧并保留结果；
    # 关闭时只跑一帧（等价于 Houdini 的 freezeframe 不生效）。
    one_frame = core.switch_float(tree, params["Freeze Enabled"],
                                  1.0, params["Freeze Frames"], (7600, 1600))
    core.link(tree, one_frame, frame_in.inputs["Iterations"])
    core.link(tree, group_input.outputs["Geometry"], frame_in.inputs[1])

    # 一帧内部：thermal → rain → [spread × N + hydro] → evaporate。
    # 复用 pegasus_erode_gn.build_erosion_frame，避免这里再维护一份
    # 顺序/格距/归一化都不一致的副本。
    frame_geometry = build_erosion_frame(
        tree, group_input, frame_in.outputs[1],
        params["Cell Size"], params["Grid Cells"], location=(-1200, 0))
    core.link(tree, frame_geometry, frame_out.inputs[0])

    # 层算子只改属性、不改几何位置。
    #
    # 之前这里把 height 写成 Z 位移"方便直接看到地形"，但预设要把侵蚀结果
    # 升采样回输出分辨率：SampleNearestSurface 是 3D 最近邻，面对一张
    # ±60 m 起伏的网格，平面探针会取到错误的顶点，升采样结果完全错乱
    # （实测侵蚀高度差从 0.09 m 变成 0.49 m）。高度位移应该由最终场景
    # 构建负责，而不是侵蚀组。
    core.finish_group(tree, group_input, frame_out.outputs["Geometry"],
                      (10200, 300))
    return tree


BUILDERS = (
    build_precipitation_group,
    build_water_spread_group,
    build_hydro_group,
    build_thermal_group,
    build_erode_group,
)


def build_all() -> list:
    return [builder() for builder in BUILDERS]

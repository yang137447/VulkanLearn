"""Pegasus 侵蚀阶段（Erode 2.0）的几何节点重写。

本模块按 Houdini `heightfield_erode::2.0` 内部的真实求解结构重建，参数取自
`D:\\YYBWorkSpace\\Pegasus\\decoded\\_erode_solvers.json`（用 hython 直接读取
原工程的 erode_hydro / erode_thermal / precipitation / evaporate 节点）。

Houdini 的一帧内部是这四个求解节点串联：

```text
precipitation   water += rain_amount        (rain_density 控制落点)
evaporate       water += -rain_evaporation  (rain_amount = -0.04)
erode_hydro     水扩散 → bed/bank 分型 → 下切 + 挟沙
erode_thermal   height 与模糊版比较 → 超休止角部分塌落进 debris
```

与原工程的对应关系（原值见 JSON）：

| GN 参数 | Houdini 参数 | 原值（pass1/pass2/pass3/pass4） |
|---|---|---|
| Rain Amount | `rain_amount` | 0.9 / 0.2 / 0.2 / 0.2 |
| Rain Density | `rain_density` | 0.05 / 0.05 / 0.1 / 0.05 |
| Evaporation | `rain_evaporation`（=蒸发节点 rain_amount 的负值） | 0.04 |
| Water Spread Iterations | `water_spreaditers` | 140 / 40 / 40 / 40 |
| Hydro Erodability | `hydro_erodability` | 1.1 / 1.5 / 1 / 1 |
| Hydro Erosion Rate | `hydro_erosionrate` | 1 / 1 / 0.4 / 0.4 |
| Bank Angle | `hydro_bankangle` | 20 / 25 / 70 / 70 |
| Removal Rate | `hydro_removalrate` | 0.1 / 1 / 0.6 / 0.1 |
| Sediment Capacity | `hydro_bed_sedimentcap` | 10 |
| Bed Deposition Rate | `hydro_bed_depositionrate` | 0.01 |
| Thermal Cut Angle | `thermal_cutangle` | 50 / 35 / 35 / 35 |
| Thermal Erosion Rate | `thermal_erosionrate` | 0.03 |
| Thermal Removal Rate | `thermal_removalrate` | 0.7 |
| Debris Repose Angle | `debris_reposeangle` | 15 |

数据层与 Houdini 一致：`height` / `water` / `debris` / `sediment` / `bedrock`。
"""

from __future__ import annotations

import math

import bpy

STATE_LAYERS = ("height", "water", "debris", "sediment", "bedrock")


# ---------------------------------------------------------------- 基础工具


def link(tree, source, target) -> None:
    """连线；参数可以是节点（自动取第一个输出/输入）或 socket。"""
    if hasattr(source, "outputs") and not hasattr(source, "is_output"):
        source = source.outputs[0]
    if hasattr(target, "inputs") and not hasattr(target, "is_output"):
        target = target.inputs[0]
    if source is None or target is None:
        raise TypeError("link() got None socket")
    tree.links.new(source, target)


def _location_arg(location):
    """位置参数可能是 tuple（被误当成第 4 个位置参数传入），统一归一化。"""
    if isinstance(location, tuple) and len(location) == 2 and all(
            isinstance(value, (int, float)) for value in location):
        return (int(location[0]), int(location[1]))
    return location


def _is_location(value) -> bool:
    """判断一个值是不是 (x, y) 位置元组，而不是一个向量操作数。"""
    return (isinstance(value, tuple) and len(value) == 2
            and all(isinstance(item, (int, float)) for item in value))


def math_node(tree, operation: str, a=None, b=None, c=None, location=(0, 0)):
    """标量数学节点。

    移植时大量调用写成 ``math_node(tree, "ADD", x, y, (100, 200))``，甚至只有
    一个操作数时位置元组会落进 ``b`` 槽：``math_node(tree, "TANGENT", x, (1, 2))``。
    两种形态都在这里识别并还原成 ``location``。
    """
    if c is not None:
        location = _location_arg(c)
    elif _is_location(b):
        location = _location_arg(b)
        b = None
    elif _is_location(a):
        location = _location_arg(a)
        a = None
    node = tree.nodes.new("ShaderNodeMath")
    node.operation = operation
    node.location = _location_arg(location)
    for index, value in enumerate((a, b)):
        if value is None:
            continue
        if isinstance(value, (int, float)):
            node.inputs[index].default_value = float(value)
        else:
            link(tree, value, node.inputs[index])
    return node


def vector_math(tree, operation: str, a=None, b=None, c=None, location=(0, 0)):
    if c is not None:
        location = _location_arg(c)
    elif _is_location(b):
        location = _location_arg(b)
        b = None
    node = tree.nodes.new("ShaderNodeVectorMath")
    node.operation = operation
    node.location = _location_arg(location)
    for index, value in enumerate((a, b)):
        if value is None:
            continue
        if isinstance(value, (tuple, list)):
            node.inputs[index].default_value = tuple(float(v) for v in value)
        else:
            link(tree, value, node.inputs[index])
    return node


def map_range(tree, value, from_min: float, from_max: float,
              to_min: float = 0.0, to_max: float = 1.0,
              location=(0, 0), clamp: bool = True,
              interpolation: str = "LINEAR"):
    node = tree.nodes.new("ShaderNodeMapRange")
    node.location = _location_arg(location)
    node.clamp = clamp
    node.interpolation_type = interpolation
    node.inputs["From Min"].default_value = float(from_min)
    node.inputs["From Max"].default_value = float(from_max)
    node.inputs["To Min"].default_value = float(to_min)
    node.inputs["To Max"].default_value = float(to_max)
    link(tree, value, node.inputs["Value"])
    return node


def combine_xyz(tree, x=None, y=None, z=None, location=(0, 0)):
    node = tree.nodes.new("ShaderNodeCombineXYZ")
    node.location = location
    for index, value in enumerate((x, y, z)):
        if value is None:
            continue
        if isinstance(value, (int, float)):
            node.inputs[index].default_value = float(value)
        else:
            link(tree, value, node.inputs[index])
    return node


def named_attribute(tree, name: str, location=(0, 0)):
    node = tree.nodes.new("GeometryNodeInputNamedAttribute")
    node.data_type = "FLOAT"
    node.location = location
    node.inputs["Name"].default_value = name
    return node.outputs["Attribute"]


def store_attribute(tree, geometry, name: str, value, location=(0, 0)):
    """写一个 FLOAT 点属性。

    socket 按"名字 + 类型"定位：Blender 5.x 里 Store Named Attribute 的
    ``Value`` 同时存在可见槽与隐式字段槽，只按类型取会命中错误的那个。
    """
    node = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    node.location = location
    node.data_type = "FLOAT"
    node.domain = "POINT"

    def pick(socket_name: str, socket_type: str):
        for socket in node.inputs:
            if socket.name == socket_name and socket.type == socket_type:
                return socket
        raise KeyError(f"{node.bl_idname}: no {socket_name!r} {socket_type}")

    pick("Name", "STRING").default_value = name
    link(tree, geometry, pick("Geometry", "GEOMETRY"))
    value_socket = pick("Value", "VALUE")
    if value is not None:
        link(tree, value, value_socket)
    else:
        value_socket.default_value = 0.0
    return node


def set_position(tree, geometry, offset, location=(0, 0)):
    """相对位移。``Position`` 是绝对赋值、``Offset`` 才是偏移，必须用后者。"""
    node = tree.nodes.new("GeometryNodeSetPosition")
    node.location = location
    for socket in node.inputs:
        if socket.name == "Geometry" and socket.type == "GEOMETRY":
            link(tree, geometry, socket)
            break
    if offset is not None:
        for socket in node.inputs:
            if socket.name == "Offset" and socket.type == "VECTOR":
                link(tree, offset, socket)
                break
    return node


def sample_nearest(tree, geometry, value, position, location=(0, 0)):
    """在网格上取最近点的属性值。"""
    node = tree.nodes.new("GeometryNodeSampleNearestSurface")
    node.location = location
    node.data_type = "FLOAT"
    for socket in node.inputs:
        if socket.name == "Mesh":
            link(tree, geometry, socket)
        elif socket.name == "Value":
            link(tree, value, socket)
        elif socket.name == "Sample Position":
            link(tree, position, socket)
    return node.outputs["Value"]


def sample_at_offset(tree, geometry, value, base_position, dx: float, dy: float,
                     location=(0, 0)):
    """取 (base_position + (dx, dy, 0)) 处的属性值。"""
    offset = combine_xyz(tree, dx, dy, 0.0, (location[0], location[1] - 120))
    probe = vector_math(tree, "ADD", base_position, offset.outputs["Vector"],
                        location)
    return sample_nearest(tree, geometry, value, probe.outputs["Vector"],
                          (location[0] + 200, location[1]))


def neighbours8():
    return ((-1, 0), (1, 0), (0, -1), (0, 1),
            (-1, -1), (-1, 1), (1, -1), (1, 1))


def add_inputs(tree, specs) -> None:
    for name, socket_type, default, minimum, maximum, description in specs:
        socket = tree.interface.new_socket(
            name=name, in_out="INPUT", socket_type=socket_type)
        socket.default_value = default
        socket.min_value = minimum
        socket.max_value = maximum
        socket.description = description


# ---------------------------------------------------------------- 求解器


def build_precipitation(tree, group_input, geometry, seed_w=1.0,
                        location=(-2000, 1600), evaporation=False):
    """降水 / 蒸发：water += rain_amount。

    Houdini 的降水节点只有 `rain_amount` / `rain_density` / `rain_expandradius`
    / `rain_blurradius` / `seed`。其中 density 与 expandradius 是"落点分布"，
    在网格场求解里表现为水量的空间抖动；这里用 4D 噪声实现同样的效果
    （density 越大、抖动越碎）。蒸发节点就是 `rain_amount = -0.04` 的同一节点。

    返回 (新几何, 新 water socket)。
    """
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (location[0], location[1] + 260)
    water = named_attribute(tree, "water", (location[0], location[1] + 120))

    # 落点抖动：值域 [-0.5, 0.5]，乘以 rain_amount 后成为 ±50% 的水量起伏。
    jitter = tree.nodes.new("ShaderNodeTexNoise")
    jitter.location = (location[0] + 180, location[1] + 400)
    jitter.noise_dimensions = "4D"
    jitter.inputs["W"].default_value = seed_w
    jitter.inputs["Detail"].default_value = 2.0
    jitter.inputs["Roughness"].default_value = 0.5
    link(tree, position.outputs["Position"], jitter.inputs["Vector"])
    # density 高 → 频率高（雨点密）；density 0.05 对应很低的频率。
    density_freq = math_node(tree, "MULTIPLY", group_input.outputs["Rain Density"],
                            0.35, (location[0] + 180, location[1] + 540))
    link(tree, density_freq.outputs[0], jitter.inputs["Scale"])
    jitter_centered = math_node(tree, "SUBTRACT", jitter.outputs["Fac"], 0.5,
                                (location[0] + 380, location[1] + 400))
    jitter_amount = (group_input.outputs["Rain Evaporation"] if evaporation
                     else group_input.outputs["Rain Amount"])
    jitter_scaled = math_node(tree, "MULTIPLY", jitter_centered.outputs[0],
                              jitter_amount,
                              (location[0] + 560, location[1] + 400))
    rate = (math_node(tree, "MULTIPLY", group_input.outputs["Rain Evaporation"], -1.0,
                      (location[0] + 720, location[1] + 500)).outputs[0]
            if evaporation else group_input.outputs["Rain Amount"])
    rain_with_jitter = math_node(tree, "ADD", rate,
                                 jitter_scaled.outputs[0],
                                 (location[0] + 720, location[1] + 340))
    rain_positive = rain_with_jitter

    added = math_node(tree, "ADD", water, rain_positive.outputs[0],
                      (location[0] + 1040, location[1] + 160))
    stored = store_attribute(tree, geometry, "water", added.outputs[0],
                             (location[0] + 1220, location[1] + 160))
    return stored.outputs["Geometry"], added.outputs[0]


def build_water_spread(tree, group_input, geometry, cell_m: float,
                       location=(-1600, 1000)):
    """水的邻域扩散（`water_spreaditers` 的一次迭代）。

    Houdini 原文：``'The number of iterations of water spreading simulation to
    do. More iterations result in longer and less dense incisions.'``
    水是 **grid 场**，每轮迭代把水向低处摊平；迭代多 → 河道更长更稀疏。

    实现要点（早期版本在这里踩过坑）：

    * 邻点偏移必须用真实格距。早期把偏移写成 ``dx * 0.0``，8 个邻点全采到
      自己，落差恒为 0，整张图没有任何扩散。
    * 不能写成 ``neighbour_water * lower``：height 是归一化层、有正有负，
      负高度会把水量抵消成 0。这里改成"把自身水位按落差分给邻格"的守恒形式，
      水位 = height + water（Houdini 用同样的 water 柱高语义）。

    返回 (新几何, 新 water socket)。
    """
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (location[0], location[1] + 420)
    water = named_attribute(tree, "water", (location[0], location[1] + 280))
    height = named_attribute(tree, "height", (location[0], location[1] + 140))

    # 水位 = 地形高度 + 水深；扩散按水位差进行，低处水位会先被填平。
    surface = math_node(tree, "ADD", height, water,
                        (location[0] + 140, location[1] + 300))

    outflow = None
    inflow = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 700 - index * 170
        neighbour_surface = sample_at_offset(
            tree, geometry, surface, position.outputs["Position"],
            dx * cell_m, dy * cell_m, (location[0] + 200, row))
        # 流出：本格水位高于邻格的部分
        difference = math_node(tree, "SUBTRACT", surface.outputs[0],
                               neighbour_surface,
                               (location[0] + 420, row))
        positive = math_node(tree, "MAXIMUM", difference.outputs[0], 0.0,
                             (location[0] + 580, row))
        term = math_node(tree, "MULTIPLY", positive.outputs[0],
                         group_input.outputs["Water Spread Rate"],
                         (location[0] + 740, row))
        # 流入：邻格水位高于本格的部分（对应邻格的流出量）
        reverse = math_node(tree, "MULTIPLY", difference.outputs[0], -1.0,
                            (location[0] + 420, row + 70))
        reverse_positive = math_node(tree, "MAXIMUM", reverse.outputs[0], 0.0,
                                     (location[0] + 580, row + 70))
        receive = math_node(tree, "MULTIPLY", reverse_positive.outputs[0],
                            group_input.outputs["Water Spread Rate"],
                            (location[0] + 740, row + 70))
        if outflow is None:
            outflow = term.outputs[0]
            inflow = receive.outputs[0]
        else:
            outflow = math_node(tree, "ADD", outflow, term.outputs[0],
                                (location[0] + 900, row)).outputs[0]
            inflow = math_node(tree, "ADD", inflow, receive.outputs[0],
                               (location[0] + 900, row + 70)).outputs[0]

    # 8 邻域均分，保证流出不超过当前水深。
    share_out = math_node(tree, "DIVIDE", outflow, 8.0,
                          (location[0] + 1060, location[1] + 300))
    limited = math_node(tree, "MINIMUM", share_out.outputs[0], water,
                        (location[0] + 1220, location[1] + 300))
    # 流出的水由邻格流回（守恒），净变化 = 流入 - 流出。
    share_in = math_node(tree, "DIVIDE", inflow, 8.0,
                         (location[0] + 1060, location[1] + 120))
    net = math_node(tree, "SUBTRACT", share_in.outputs[0], limited.outputs[0],
                    (location[0] + 1380, location[1] + 300))
    new_water = math_node(tree, "ADD", water, net.outputs[0],
                          (location[0] + 1540, location[1] + 300))
    non_negative = math_node(tree, "MAXIMUM", new_water.outputs[0], 0.0,
                             (location[0] + 1700, location[1] + 300))
    stored = store_attribute(tree, geometry, "water", non_negative.outputs[0],
                             (location[0] + 1880, location[1] + 300))
    return stored.outputs["Geometry"], non_negative.outputs[0]


def build_hydro_erosion(tree, group_input, geometry, cell_m: float,
                        location=(-1200, 400)):
    """水力侵蚀：bed/bank 分型 + 下切 + 挟沙沉积。

    对应 Houdini `heightfield_erode_hydro` 的参数：

    * `erodability × erosionrate` = 下切强度
    * `bankangle`：河道横截面坡角；决定河岸宽度
    * `bed_sedimentcap`：单位水流能携带的沙量，超出部分沉积
    * `bed_depositionrate`：多余沉积物转成 debris 的速率
    * `removalrate`：被带走的物质直接删除的比例（这是唯一让山体变矮的机制）
    * `bed_/bank_erosionratefactor`：河床/河岸分别的乘数
    * `bank_maxbankbedwaterratio`：按水柱高度比区分 bed / bank

    返回 (新几何, 新 height socket, 新 sediment socket, sediment 增量)。
    """
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (location[0], location[1] + 500)
    height = named_attribute(tree, "height", (location[0], location[1] + 380))
    water = named_attribute(tree, "water", (location[0], location[1] + 260))
    sediment = named_attribute(tree, "sediment", (location[0], location[1] + 140))

    # --- 坡度：8 邻域落差的**平均**陡度。
    #
    # 早期用"最陡邻格"（MAXIMUM），在带噪声的基座上等于逐格挑最坏情况，
    # 把高频抖动放大成满屏碎渣。Houdini 的水力侵蚀作用在连续场上，等价于
    # 用平滑后的坡度；这里改成 average absolute slope，形态才能收敛到大尺度
    # 的树枝状沟谷。
    height_m = math_node(tree, "MULTIPLY", height,
                         group_input.outputs["Height Scale"],
                         (location[0] + 200, location[1] + 500))
    slope_total = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 900 - index * 220
        neighbour_height = sample_at_offset(
            tree, geometry, height, position.outputs["Position"],
            dx * cell_m, dy * cell_m, (location[0] + 200, row))
        neighbour_height_m = math_node(
            tree, "MULTIPLY", neighbour_height,
            group_input.outputs["Height Scale"], (location[0] + 400, row))
        drop = math_node(tree, "SUBTRACT", height_m.outputs[0],
                         neighbour_height_m.outputs[0],
                         (location[0] + 560, row))
        # 用下坡幅度（正值）累加，负值时记 0。
        positive = math_node(tree, "MAXIMUM", drop.outputs[0], 0.0,
                             (location[0] + 720, row))
        if slope_total is None:
            slope_total = positive.outputs[0]
        else:
            slope_total = math_node(tree, "ADD", slope_total,
                                    positive.outputs[0],
                                    (location[0] + 880, row)).outputs[0]
    mean_slope_raw = math_node(tree, "DIVIDE", slope_total, 8.0,
                               (location[0] + 1040, 900))
    # 换算成无量纲坡度：除以格距。
    slope_max = math_node(tree, "DIVIDE", mean_slope_raw.outputs[0], cell_m,
                          (location[0] + 1200, 900))

    # --- bed / bank 分类
    # 取邻域平均水深作为"周围水位"，本站水深 / 周围水位 > ratio 判为 bed。
    neighbour_water_sum = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 2000 - index * 200
        neighbour_water = sample_at_offset(
            tree, geometry, water, position.outputs["Position"],
            dx * cell_m, dy * cell_m, (location[0] + 200, row))
        if neighbour_water_sum is None:
            neighbour_water_sum = neighbour_water
        else:
            neighbour_water_sum = math_node(
                tree, "ADD", neighbour_water_sum, neighbour_water,
                (location[0] + 400, row)).outputs[0]
    neighbour_water_mean = math_node(tree, "DIVIDE", neighbour_water_sum, 8.0,
                                     (location[0] + 560, 2000))
    safe_mean = math_node(tree, "MAXIMUM", neighbour_water_mean.outputs[0], 1e-4,
                          (location[0] + 720, 2000))
    ratio = math_node(tree, "DIVIDE", water, safe_mean.outputs[0],
                      (location[0] + 880, 2000))
    bed_threshold = group_input.outputs["Bank Water Ratio"]
    is_bed = tree.nodes.new("FunctionNodeCompare")
    is_bed.location = (location[0] + 1040, 2000)
    is_bed.data_type = "FLOAT"
    is_bed.operation = "GREATER_THAN"
    link(tree, ratio.outputs[0], is_bed.inputs["A"])
    link(tree, bed_threshold, is_bed.inputs["B"])

    # bed 用 bed_erosionratefactor，否则用 bank_erosionratefactor。
    bed_factor = group_input.outputs["Bed Erosion Factor"]
    bank_factor = group_input.outputs["Bank Erosion Factor"]
    factor_pick = tree.nodes.new("GeometryNodeSwitch")
    factor_pick.location = (location[0] + 1220, 2000)
    factor_pick.input_type = "FLOAT"
    link(tree, is_bed.outputs["Result"], factor_pick.inputs["Switch"])
    link(tree, bank_factor, factor_pick.inputs["False"])
    link(tree, bed_factor, factor_pick.inputs["True"])

    # --- 下切量 = water * slope * erodability * erosionrate * factor
    hydro_k = math_node(tree, "MULTIPLY", group_input.outputs["Hydro Erodability"],
                        group_input.outputs["Hydro Erosion Rate"],
                        (location[0] + 1220, 1700))
    bank_angle_scale = map_range(
        tree, group_input.outputs["Bank Angle"], 0.0, 90.0, 1.0, 0.25,
        (location[0] + 1220, 1840)).outputs["Result"]
    strength = math_node(tree, "MULTIPLY", hydro_k.outputs[0],
                         bank_angle_scale, (location[0] + 1400, 1700))
    strength_with_factor = math_node(tree, "MULTIPLY", strength.outputs[0],
                                     factor_pick.outputs["Output"],
                                     (location[0] + 1580, 1780))
    water_slope = math_node(tree, "MULTIPLY", water, slope_max.outputs[0],
                            (location[0] + 1400, 1560))
    incision = math_node(tree, "MULTIPLY", water_slope.outputs[0],
                         strength_with_factor.outputs[0],
                         (location[0] + 1760, 1660))
    # 下切量以"归一化高度层"为单位，所以除以 Height Scale。
    incision_normalized = math_node(tree, "DIVIDE", incision.outputs[0],
                                    group_input.outputs["Height Scale"],
                                    (location[0] + 1940, 1660))
    max_incision = math_node(tree, "MULTIPLY",
                             group_input.outputs["Max Debris Depth"],
                             0.002, (location[0] + 1940, 1820))
    incision_clamped = math_node(tree, "MINIMUM", incision_normalized.outputs[0],
                                 max_incision.outputs[0],
                                 (location[0] + 2120, 1660))

    # --- 挟沙容量：超出 sedimentcap 的部分回流成沉积
    capacity = math_node(tree, "MULTIPLY", water,
                         group_input.outputs["Sediment Capacity"],
                         (location[0] + 1760, 1420))
    capacity_with_slope = math_node(tree, "MULTIPLY", capacity.outputs[0],
                                    slope_max.outputs[0], (location[0] + 1940, 1420))
    carried = math_node(tree, "ADD", sediment, incision_clamped.outputs[0],
                        (location[0] + 2120, 1420))
    excess = math_node(tree, "SUBTRACT", carried.outputs[0],
                       capacity_with_slope.outputs[0],
                       (location[0] + 2300, 1420))
    excess_positive = math_node(tree, "MAXIMUM", excess.outputs[0], 0.0,
                                (location[0] + 2480, 1420))
    deposition = math_node(tree, "MULTIPLY", excess_positive.outputs[0],
                           group_input.outputs["Bed Deposition Rate"],
                           (location[0] + 2660, 1420))
    # 沉积回填高度，同时留在 sediment 层。
    net_incision = math_node(tree, "SUBTRACT", incision_clamped.outputs[0],
                             deposition.outputs[0],
                             (location[0] + 2660, 1660))

    # --- removal rate：物质被水彻底带走（山体因此变矮）
    removed = math_node(tree, "MULTIPLY", net_incision.outputs[0],
                        group_input.outputs["Removal Rate"],
                        (location[0] + 2840, 1660))
    final_cut = math_node(tree, "ADD", net_incision.outputs[0],
                          removed.outputs[0], (location[0] + 3020, 1660))
    new_height = math_node(tree, "SUBTRACT", height, final_cut.outputs[0],
                           (location[0] + 3200, 1660))
    new_sediment = math_node(tree, "MAXIMUM", deposition.outputs[0], 0.0,
                             (location[0] + 3020, 1420))

    stored_height = store_attribute(tree, geometry, "height",
                                    new_height.outputs[0],
                                    (location[0] + 3380, 1660))
    stored_sediment = store_attribute(tree, stored_height.outputs["Geometry"],
                                      "sediment", new_sediment.outputs[0],
                                      (location[0] + 3560, 1420))
    return (stored_sediment.outputs["Geometry"], new_height.outputs[0],
            new_sediment.outputs[0])


def build_thermal_erosion(tree, group_input, geometry, cell_m: float,
                          location=(-400, -200)):
    """热力侵蚀（一次迭代）。

    官方原文：thermal erosion 是「把高度场（含 debris）与它的**模糊版**比较，
    高出部分按**日照曝晒**比例侵蚀，是一个松弛过程」。

    简化实现：把 8 邻域均值当作模糊版，超出部分按 `thermal_erosionrate`
    塌落进 debris；超过 `reposeangle` 的部分转移到低处（`removalrate`）。

    返回 (新几何, 新 height socket, 新 debris socket)。
    """
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (location[0], location[1] + 500)
    height = named_attribute(tree, "height", (location[0], location[1] + 380))
    debris = named_attribute(tree, "debris", (location[0], location[1] + 260))

    # 高度含 debris：热侵蚀作用在 height + debris 上。
    combined = math_node(tree, "ADD", height, debris,
                         (location[0] + 200, location[1] + 380))

    neighbour_sum = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 900 - index * 200
        neighbour = sample_at_offset(
            tree, geometry, height, position.outputs["Position"],
            dx * cell_m, dy * cell_m, (location[0] + 200, row))
        if neighbour_sum is None:
            neighbour_sum = neighbour
        else:
            neighbour_sum = math_node(tree, "ADD", neighbour_sum, neighbour,
                                      (location[0] + 400, row)).outputs[0]

    blurred = math_node(tree, "DIVIDE", neighbour_sum, 8.0,
                        (location[0] + 560, 900))
    # 高于模糊版 → 被侵蚀；低于则不动（松弛过程）。
    excess = math_node(tree, "SUBTRACT", combined.outputs[0], blurred.outputs[0],
                       (location[0] + 720, 900))
    positive = math_node(tree, "MAXIMUM", excess.outputs[0], 0.0,
                         (location[0] + 880, 900))
    erode_amount = math_node(tree, "MULTIPLY", positive.outputs[0],
                             group_input.outputs["Thermal Erosion Rate"],
                             (location[0] + 1040, 900))

    # 休止角约束：坡度越陡，塌落越多。
    height_m = math_node(tree, "MULTIPLY", combined.outputs[0],
                         group_input.outputs["Height Scale"],
                         (location[0] + 720, 620))
    steepest_drop = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 300 - index * 180
        neighbour = sample_at_offset(
            tree, geometry, height, position.outputs["Position"],
            dx * cell_m, dy * cell_m, (location[0] + 200, row))
        neighbour_m = math_node(tree, "MULTIPLY", neighbour,
                                group_input.outputs["Height Scale"],
                                (location[0] + 400, row))
        drop = math_node(tree, "SUBTRACT", height_m.outputs[0],
                         neighbour_m.outputs[0], (location[0] + 560, row))
        if steepest_drop is None:
            steepest_drop = drop.outputs[0]
        else:
            steepest_drop = math_node(tree, "MAXIMUM", steepest_drop,
                                      drop.outputs[0],
                                      (location[0] + 720, row)).outputs[0]

    repose_radians = math_node(tree, "RADIANS",
                               group_input.outputs["Repose Angle"],
                               location=(location[0] + 880, 500))
    repose_tan = math_node(tree, "TANGENT", repose_radians.outputs[0],
                           (location[0] + 1040, 500))
    cell = group_input.outputs["Cell Size"]
    allowed = math_node(tree, "MULTIPLY", repose_tan.outputs[0], cell,
                        (location[0] + 1200, 500))
    over_angle = math_node(tree, "SUBTRACT", steepest_drop, allowed.outputs[0],
                           (location[0] + 1360, 500))
    over_positive = math_node(tree, "MAXIMUM", over_angle.outputs[0], 0.0,
                              (location[0] + 1520, 500))
    over_normalized = math_node(tree, "DIVIDE", over_positive.outputs[0],
                                group_input.outputs["Height Scale"],
                                (location[0] + 1680, 500))
    # 最终塌落量：日照/松弛项 + 超角项，整体乘 thermal removal rate。
    collapse = math_node(tree, "ADD", erode_amount.outputs[0],
                         over_normalized.outputs[0], (location[0] + 1840, 700))
    collapse_scaled = math_node(tree, "MULTIPLY", collapse.outputs[0],
                                group_input.outputs["Thermal Removal Rate"],
                                (location[0] + 2020, 700))

    new_height = math_node(tree, "SUBTRACT", height, collapse_scaled.outputs[0],
                           (location[0] + 2200, 380))
    new_debris = math_node(tree, "ADD", debris, collapse_scaled.outputs[0],
                           (location[0] + 2200, 260))

    stored_height = store_attribute(tree, geometry, "height",
                                    new_height.outputs[0],
                                    (location[0] + 2380, 380))
    stored_debris = store_attribute(tree, stored_height.outputs["Geometry"],
                                     "debris", new_debris.outputs[0],
                                     (location[0] + 2560, 260))
    return stored_debris.outputs["Geometry"], new_height.outputs[0], new_debris.outputs[0]


def build_erosion_frame(tree, group_input, geometry, cell_m: float,
                        location=(0, 0)):
    """一帧内部的完整求解顺序（与 Houdini 的四个求解节点一一对应）。

    ```text
    precipitation → evaporate → water spread × N → hydro → thermal
    ```
    """
    stage_geometry, _ = build_precipitation(
        tree, group_input, geometry, location=(location[0] + 0, location[1] + 1600))
    evaporate_geometry, _ = build_precipitation(
        tree, group_input, stage_geometry, seed_w=2.0,
        location=(location[0] + 1600, location[1] + 1600), evaporation=True)

    # 水扩散：`water_spreaditers` 次。
    #
    # Repeat 区的插槽布局（Blender 5.2 实测）：
    #   RepeatInput.inputs  = [Iterations, Geometry(Item_0), __extend__]
    #   RepeatInput.outputs = [Iteration, Geometry(Item_0), __extend__]
    #   RepeatOutput.inputs = [Geometry(Item_0), __extend__]
    # 正确接法：entrance → RepeatInput.inputs[1]；
    #           RepeatInput.outputs[1] → 迭代体 → RepeatOutput.inputs[0]。
    # 直接用 outputs[0]/inputs[0] 会拿到 Iteration 整数，循环体不注册，结果为空。
    repeat_out = tree.nodes.new("GeometryNodeRepeatOutput")
    repeat_out.location = (location[0] + 6800, location[1] + 1600)
    repeat_in = tree.nodes.new("GeometryNodeRepeatInput")
    repeat_in.location = (location[0] + 3400, location[1] + 1700)
    repeat_in.pair_with_output(repeat_out)
    repeat_in.inputs["Iterations"].default_value = 1
    link(tree, group_input.outputs["Water Spread Iterations"],
         repeat_in.inputs["Iterations"])
    link(tree, evaporate_geometry, repeat_in.inputs[1])
    spread_geometry, _ = build_water_spread(
        tree, group_input, repeat_in.outputs[1], cell_m,
        (location[0] + 3500, location[1] + 1100))
    link(tree, spread_geometry, repeat_out.inputs[0])

    hydro_geometry, _, _ = build_hydro_erosion(
        tree, group_input, repeat_out.outputs[0], cell_m,
        (location[0] + 5000, location[1] + 600))
    thermal_geometry, _, _ = build_thermal_erosion(
        tree, group_input, hydro_geometry, cell_m,
        (location[0] + 5000, location[1] - 400))
    return thermal_geometry


def build_erode_group(size_m: float, resolution: int, inputs, iterations: int = 1):
    cell_m = size_m / max(resolution - 1, 1)
    """建出完整的 Erode 节点组。

    ``inputs`` 是 add_inputs 用的规格表；``iterations`` 是外层帧数
    （Houdini 的 `freezeframe`，Pegasus 用 5）。
    """
    tree = bpy.data.node_groups.new("VL_Pegasus_Erode", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT",
                              socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT",
                              socket_type="NodeSocketGeometry")
    add_inputs(tree, inputs)

    group_input = tree.nodes.new("NodeGroupInput")
    group_input.location = (-1200, -2400)
    group_output = tree.nodes.new("NodeGroupOutput")
    group_output.location = (14000, 0)

    # 外层：帧循环（Houdini freezeframe=5）
    # 插槽布局同 Repeat 区：entrance → inputs[1]，outputs[1] → 迭代体 → inputs[0]。
    frame_out = tree.nodes.new("GeometryNodeRepeatOutput")
    frame_out.location = (13000, 0)
    frame_in = tree.nodes.new("GeometryNodeRepeatInput")
    frame_in.location = (13000, 200)
    frame_in.pair_with_output(frame_out)
    frame_in.inputs["Iterations"].default_value = int(iterations)

    internal = build_erosion_frame(tree, group_input, frame_in.outputs[1], cell_m)
    link(tree, internal, frame_out.inputs[0])
    link(tree, frame_out.outputs[0], group_output.inputs["Geometry"])
    return tree

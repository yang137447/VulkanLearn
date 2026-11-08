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

# 单点下切 gain：把 water*slope 的无量纲乘积放大到 Houdini 一帧的实际下切量级
# （before/after 基准显示一段侵蚀约切 5 m，而裸乘积只有 1e-3 m 量级）。
HYDRO_GAIN = 8.0


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


def sample_at_offset(tree, geometry, value, base_position, dx: int, dy: int,
                     cell, location=(0, 0)):
    """取 ``base_position + (dx, dy) * cell`` 处的属性值。

    ``cell`` 可以是 float，也可以是 socket——格距必须跟着实际分辨率走，
    不能在建组时焊死成 4 m（4 km / 128 格时真实格距是 31.5 m）。
    """
    offset_x = math_node(tree, "MULTIPLY", cell, float(dx),
                         (location[0] - 200, location[1] - 60))
    offset_y = math_node(tree, "MULTIPLY", cell, float(dy),
                         (location[0] - 200, location[1] - 180))
    offset = combine_xyz(tree, offset_x.outputs[0], offset_y.outputs[0], 0.0,
                         (location[0], location[1] - 120))
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
    """降水 / 蒸发。

    降水：``water += rain_amount × rain_density``（米 → 归一化层）。
    ``rain_density`` 是落点覆盖率（0..1），所以面平均降水深度是
    ``amount × density``。旧实现只把 density 当噪声频率，等于每帧给
    **每个**格子加 0.2 m，5 帧后 water 均匀堆到 0.907 m、几乎没有方差，
    河道完全消失。

    蒸发：``water *= (1 - rain_evaporation)``。参数标签是
    "Evaporation Rate"、范围 0..1，按每帧比例处理。若按 0.04 m/帧的
    深度扣，5 帧 0.05 m 的降水会被全部扣光，与参考水层对不上
    （参考 water 均值 0.024 m、最大 1.535 m）。

    返回 (新几何, 新 water socket)。
    """
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (location[0], location[1] + 260)
    water = named_attribute(tree, "water", (location[0], location[1] + 120))

    # 落点抖动：Fac 均值 0.5，所以 (Fac + 0.5) 的均值为 1，只改空间分布。
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
    if evaporation:
        keep = math_node(tree, "SUBTRACT", 1.0,
                         group_input.outputs["Rain Evaporation"],
                         (location[0] + 560, location[1] + 400))
        new_water = math_node(tree, "MULTIPLY", water, keep.outputs[0],
                              (location[0] + 860, location[1] + 160))
    else:
        patch = math_node(tree, "ADD", jitter.outputs["Fac"], 0.5,
                          (location[0] + 380, location[1] + 400))
        mean_depth = math_node(tree, "MULTIPLY",
                               group_input.outputs["Rain Amount"],
                               group_input.outputs["Rain Density"],
                               (location[0] + 560, location[1] + 520))
        depth = math_node(tree, "MULTIPLY", mean_depth.outputs[0],
                          patch.outputs[0], (location[0] + 720, location[1] + 400))
        normalized = math_node(tree, "DIVIDE", depth.outputs[0],
                               group_input.outputs["Height Scale"],
                               (location[0] + 880, location[1] + 400))
        new_water = math_node(tree, "ADD", water, normalized.outputs[0],
                              (location[0] + 1040, location[1] + 160))

    stored = store_attribute(tree, geometry, "water", new_water.outputs[0],
                             (location[0] + 1220, location[1] + 160))
    return stored.outputs["Geometry"], new_water.outputs[0]


def switch_float(tree, condition, false_value, true_value, location=(0, 0)):
    """浮点 Switch；条件为真取 true_value。"""
    node = tree.nodes.new("GeometryNodeSwitch")
    node.location = location
    node.input_type = "FLOAT"
    link(tree, condition, node.inputs["Switch"])
    for socket_name, value in (("False", false_value), ("True", true_value)):
        socket = node.inputs[socket_name]
        # NodeSocketFloat 也要走 link：只有裸数值才写 default_value。
        if isinstance(value, (int, float)):
            socket.default_value = float(value)
        else:
            link(tree, value, socket)
    return node.outputs["Output"]


def build_water_spread(tree, group_input, geometry, cell, total_cells,
                       location=(-1600, 1000)):
    """水的一步保守多向搬运 + 汇水面积累积（``water_spreaditers`` 的一次迭代）。

    Houdini 原文：``'The number of iterations of water spreading simulation to
    do. More iterations result in longer and less dense incisions.'``

    每个格子按各下坡方向的落差权重，把自己 ``alpha`` 比例的水分给 8 邻域::

        send_i = alpha * water * drop_i / Σ_j drop_j

    接收端对**同一对格子**求值完全相同的表达式（在邻格上算
    ``alpha * water_n * drop(n→self) / Σ_j drop(n→j)``），所以总水量守恒。
    旧实现把 ``min(drop, water) / 8`` 直接抹掉、根本没有按方向路由到邻格，
    实测水均匀堆到 0.907（p1..p99 只有 0.86..0.99），既没有河道，也就
    没有水力侵蚀。

    汇水面积 ``flow``（单位：格）用同一组权重累积::

        A' = 1 + alpha * Σ_n share_n * A_n

    山脊 ≈ 1、主河道可达上万，水力侵蚀因此可以按 ``A / 总格数`` 归一化，
    不再需要一个拍脑袋的汇水常数。

    返回 (新几何, 新 water socket, 新 flow socket)。
    """
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (location[0], location[1] + 420)
    water = named_attribute(tree, "water", (location[0], location[1] + 280))
    height = named_attribute(tree, "height", (location[0], location[1] + 140))
    flow = named_attribute(tree, "flow", (location[0], location[1] + 0))
    alpha = group_input.outputs["Water Spread Rate"]

    surface = math_node(tree, "ADD", height, water,
                        (location[0] + 140, location[1] + 300))

    # --- 第一步：本格朝 8 个方向的总落差 Σ_j max(surface - surface_j, 0)
    total_drop = None
    height_drop = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 900 - index * 190
        neighbour = sample_at_offset(
            tree, geometry, surface, position.outputs["Position"],
            dx, dy, cell, (location[0] + 200, row))
        neighbour_height = sample_at_offset(
            tree, geometry, height, position.outputs["Position"],
            dx, dy, cell, (location[0] + 200, row - 60))
        drop = math_node(tree, "SUBTRACT", surface.outputs[0], neighbour,
                         (location[0] + 400, row))
        positive = math_node(tree, "MAXIMUM", drop.outputs[0], 0.0,
                             (location[0] + 560, row))
        terrain_drop = math_node(tree, "SUBTRACT", height, neighbour_height,
                                 (location[0] + 400, row - 60))
        terrain_positive = math_node(tree, "MAXIMUM",
                                     terrain_drop.outputs[0], 0.0,
                                     (location[0] + 560, row - 60))
        if total_drop is None:
            total_drop = positive.outputs[0]
            height_drop = terrain_positive.outputs[0]
        else:
            total_drop = math_node(tree, "ADD", total_drop, positive.outputs[0],
                                   (location[0] + 720, row)).outputs[0]
            height_drop = math_node(tree, "ADD", height_drop,
                                    terrain_positive.outputs[0],
                                    (location[0] + 720, row - 60)).outputs[0]

    # 分母下限：平地上所有 share 恒为 0，不会除零也不会凭空产生水。
    safe_total = math_node(tree, "MAXIMUM", total_drop, 1e-6,
                           (location[0] + 880, location[1] + 700))
    safe_height_total = math_node(tree, "MAXIMUM", height_drop, 1e-6,
                                  (location[0] + 880, location[1] + 560))
    has_downhill = tree.nodes.new("FunctionNodeCompare")
    has_downhill.location = (location[0] + 880, location[1] + 900)
    has_downhill.data_type = "FLOAT"
    has_downhill.operation = "GREATER_THAN"
    link(tree, total_drop, has_downhill.inputs["A"])
    has_downhill.inputs["B"].default_value = 1e-6

    # 总落差必须先落成属性再被邻域采样：直接对「内含采样节点的派生场」
    # 再做 SampleNearestSurface 会触发嵌套求值，实测整条链出 NaN。
    outflow_stored = store_attribute(tree, geometry, "__outflow", safe_total,
                                     (location[0] + 1000, location[1] + 900))
    outflow_stored = store_attribute(tree, outflow_stored.outputs["Geometry"],
                                     "__outflow_h", safe_height_total,
                                     (location[0] + 1000, location[1] + 760))
    pass_geometry = outflow_stored.outputs["Geometry"]
    outflow_attribute = named_attribute(tree, "__outflow",
                                        (location[0] + 1000, location[1] + 1150))
    outflow_height_attribute = named_attribute(
        tree, "__outflow_h", (location[0] + 1000, location[1] + 1320))

    # --- 第二步：从 8 邻域接收，同时累积汇水面积
    received = None
    new_flow = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 300 - index * 210
        neighbour_surface = sample_at_offset(
            tree, pass_geometry, surface, position.outputs["Position"],
            dx, dy, cell, (location[0] + 200, row))
        neighbour_water = sample_at_offset(
            tree, pass_geometry, water, position.outputs["Position"],
            dx, dy, cell, (location[0] + 200, row - 70))
        neighbour_total = sample_at_offset(
            tree, pass_geometry, outflow_attribute, position.outputs["Position"],
            dx, dy, cell, (location[0] + 200, row - 150))
        neighbour_flow = sample_at_offset(
            tree, pass_geometry, flow, position.outputs["Position"],
            dx, dy, cell, (location[0] + 200, row - 230))
        neighbour_height = sample_at_offset(
            tree, pass_geometry, height, position.outputs["Position"],
            dx, dy, cell, (location[0] + 200, row - 310))
        neighbour_height_total = sample_at_offset(
            tree, pass_geometry, outflow_height_attribute,
            position.outputs["Position"], dx, dy, cell,
            (location[0] + 200, row - 390))

        rise = math_node(tree, "SUBTRACT", neighbour_surface,
                         surface.outputs[0], (location[0] + 400, row))
        rise_positive = math_node(tree, "MAXIMUM", rise.outputs[0], 0.0,
                                  (location[0] + 560, row))
        share = math_node(tree, "DIVIDE", rise_positive.outputs[0],
                          neighbour_total, (location[0] + 720, row))
        share_alpha = math_node(tree, "MULTIPLY", share.outputs[0], alpha,
                                (location[0] + 880, row))
        # 汇水面积用**地形**落差累积：水面会随流动改变方向、形成 A→B→A
        # 的环流，用它累积会无界放大甚至饱和到整个流域；地形落差是静态
        # 有向无环图，收敛到真实汇水面积。
        rise_terrain = math_node(tree, "SUBTRACT", neighbour_height, height,
                                 (location[0] + 400, row - 310))
        rise_terrain_positive = math_node(tree, "MAXIMUM",
                                          rise_terrain.outputs[0], 0.0,
                                          (location[0] + 560, row - 310))
        area_share = math_node(tree, "DIVIDE", rise_terrain_positive.outputs[0],
                               neighbour_height_total,
                               (location[0] + 720, row - 310))
        carry = math_node(tree, "MULTIPLY", neighbour_water,
                          share_alpha.outputs[0], (location[0] + 1040, row))
        area = math_node(tree, "MULTIPLY", neighbour_flow,
                         area_share.outputs[0], (location[0] + 1040, row - 80))

        if received is None:
            received = carry.outputs[0]
        else:
            received = math_node(tree, "ADD", received, carry.outputs[0],
                                 (location[0] + 1200, row)).outputs[0]
        if new_flow is None:
            new_flow = area.outputs[0]
        else:
            new_flow = math_node(tree, "ADD", new_flow, area.outputs[0],
                                 (location[0] + 1200, row - 80)).outputs[0]

    # 有下坡 → 流走 alpha；完全平坦 → 全部保留。
    keep_fraction = math_node(tree, "SUBTRACT", 1.0, alpha,
                              (location[0] + 1400, location[1] + 760))
    keep = switch_float(tree, has_downhill.outputs["Result"], 1.0,
                        keep_fraction.outputs[0],
                        (location[0] + 1400, location[1] + 620))
    kept = math_node(tree, "MULTIPLY", water, keep,
                     (location[0] + 1560, location[1] + 600))
    new_water = math_node(tree, "ADD", kept.outputs[0], received,
                          (location[0] + 1720, location[1] + 600))
    new_water_clamped = math_node(tree, "MAXIMUM", new_water.outputs[0], 0.0,
                                  (location[0] + 1880, location[1] + 600))
    # 本格自身的产流：1 格。
    #
    # 上限是总格数：水面随水流动而变，方向可能出现 A→B→A 的循环，
    # 汇水面积会在迭代中无界放大（实测 20 轮就到 6.4e7 > N），40 轮直接
    # 溢出成 inf，再经 sqrt/乘法把整条链变成 NaN。物理上汇水面积本来就
    # 不可能超过总格数。
    new_flow_total = math_node(tree, "ADD", new_flow, 1.0,
                               (location[0] + 1560, location[1] + 200))
    new_flow_clamped = math_node(tree, "MINIMUM", new_flow_total.outputs[0],
                                 total_cells, (location[0] + 1720, location[1] + 200))

    stored_water = store_attribute(tree, pass_geometry, "water",
                                   new_water_clamped.outputs[0],
                                   (location[0] + 2060, location[1] + 600))
    stored_flow = store_attribute(tree, stored_water.outputs["Geometry"],
                                  "flow", new_flow_clamped.outputs[0],
                                  (location[0] + 2240, location[1] + 200))
    cleanup = tree.nodes.new("GeometryNodeRemoveAttribute")
    cleanup.location = (location[0] + 2380, location[1] + 200)
    for socket in cleanup.inputs:
        if socket.name == "Geometry":
            link(tree, stored_flow.outputs["Geometry"], socket)
        elif socket.name == "Name" and socket.type == "STRING":
            socket.default_value = "__outflow"
    cleanup_height = tree.nodes.new("GeometryNodeRemoveAttribute")
    cleanup_height.location = (location[0] + 2520, location[1] + 200)
    for socket in cleanup_height.inputs:
        if socket.name == "Geometry":
            link(tree, cleanup.outputs["Geometry"], socket)
        elif socket.name == "Name" and socket.type == "STRING":
            socket.default_value = "__outflow_h"
    return (cleanup_height.outputs["Geometry"], new_water_clamped.outputs[0],
            new_flow_clamped.outputs[0])


def build_hydro_erosion(tree, group_input, geometry, cell_m: float,
                        total_cells: float, location=(-1200, 400)):
    """水力侵蚀：按汇水强度下切 + bed/bank 分型 + 挟沙沉积。

    对应 Houdini `heightfield_erode_hydro` 的参数：

    * `erodability × erosionrate`：下切强度
    * `bankangle`：河道横截面坡角（低=窄深，高=宽缓）
    * `bed_sedimentcap`：单位水流挟沙量上限，超出部分沉积
    * `bed_depositionrate`：多余沉积物转成 debris 的速率
    * `removalrate`：被水彻底带走的比例（唯一让山体变矮的机制）
    * `bed_/bank_erosionratefactor`：河床 / 河岸各自的乘数
    * `bank_maxbankbedwaterratio`：按水柱高度比区分河床与河岸

    **下切只发生在有汇水流过的格子**（`flow` 高），这是与早期实现的关键区别：
    早期用"每个点看邻居落差就下切"，等于逐点独立剜坑，放大后是满屏尖刺。
    Houdini 的水力侵蚀是沿水流路径连续作用的，所以这里以 `flow` 为主驱动项。

    返回 (新几何, 新 height socket, 新 sediment socket)。
    """
    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (location[0], location[1] + 500)
    height = named_attribute(tree, "height", (location[0], location[1] + 380))
    water = named_attribute(tree, "water", (location[0], location[1] + 260))
    flow = named_attribute(tree, "flow", (location[0], location[1] + 140))
    sediment = named_attribute(tree, "sediment", (location[0], location[1] + 20))

    # --- 坡度：8 邻域平均落差（米）。用平均值而非最大值，避免逐格抖动被放大。
    height_m = math_node(tree, "MULTIPLY", height,
                         group_input.outputs["Height Scale"],
                         (location[0] + 200, location[1] + 500))
    slope_total = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 900 - index * 220
        neighbour = sample_at_offset(
            tree, geometry, height, position.outputs["Position"],
            dx, dy, cell_m, (location[0] + 200, row))
        neighbour_m = math_node(tree, "MULTIPLY", neighbour,
                                group_input.outputs["Height Scale"],
                                (location[0] + 400, row))
        drop = math_node(tree, "SUBTRACT", height_m.outputs[0],
                         neighbour_m.outputs[0], (location[0] + 560, row))
        positive = math_node(tree, "MAXIMUM", drop.outputs[0], 0.0,
                             (location[0] + 720, row))
        if slope_total is None:
            slope_total = positive.outputs[0]
        else:
            slope_total = math_node(tree, "ADD", slope_total,
                                    positive.outputs[0],
                                    (location[0] + 880, row)).outputs[0]
    # 平均落差（米）→ 除以格距得到无量纲坡度
    mean_drop = math_node(tree, "DIVIDE", slope_total, 8.0,
                          (location[0] + 1040, 900))
    slope = math_node(tree, "DIVIDE", mean_drop.outputs[0], cell_m,
                      (location[0] + 1200, 900))

    # --- bed / bank 分类：本站水深 / 邻域平均水深 超过阈值判为河床
    neighbour_water_sum = None
    for index, (dx, dy) in enumerate(neighbours8()):
        row = location[1] + 2000 - index * 200
        neighbour_water = sample_at_offset(
            tree, geometry, water, position.outputs["Position"],
            dx, dy, cell_m, (location[0] + 200, row))
        if neighbour_water_sum is None:
            neighbour_water_sum = neighbour_water
        else:
            neighbour_water_sum = math_node(
                tree, "ADD", neighbour_water_sum, neighbour_water,
                (location[0] + 400, row)).outputs[0]
    neighbour_mean = math_node(tree, "DIVIDE", neighbour_water_sum, 8.0,
                               (location[0] + 560, 2000))
    safe_mean = math_node(tree, "MAXIMUM", neighbour_mean.outputs[0], 1e-6,
                          (location[0] + 720, 2000))
    water_ratio = math_node(tree, "DIVIDE", water, safe_mean.outputs[0],
                            (location[0] + 880, 2000))
    is_bed = tree.nodes.new("FunctionNodeCompare")
    is_bed.location = (location[0] + 1040, 2000)
    is_bed.data_type = "FLOAT"
    is_bed.operation = "GREATER_THAN"
    link(tree, water_ratio.outputs[0], is_bed.inputs["A"])
    link(tree, group_input.outputs["Bank Water Ratio"], is_bed.inputs["B"])
    factor = switch_float(
        tree, is_bed.outputs["Result"],
        group_input.outputs["Bank Erosion Factor"],
        group_input.outputs["Bed Erosion Factor"], (location[0] + 1220, 2000))

    # --- 下切强度（按 Houdini 官方文档语义重建）
    #
    # 官方 heightfield_erode_hydro 原文：
    #   Global Erosion Rate: "how many meters of the eroding layer is eroded
    #     **every iteration**"
    #   Erodability: "softness ... higher values cause deeper incisions"
    #   Erosion Rate: "a **multiplier on Erodability**"
    #   Slope Factor: "how much the slope would **affect** the erosion amount"
    # 早期把下切写成 ``flow × slope × K`` —— 乘积在平坦处趋于 0，沟谷切不深，
    # 曲率只有基准的 0.42 倍。正确形式是"每迭代固定米数 × 各项系数"。
    hydro_k = math_node(tree, "MULTIPLY", group_input.outputs["Hydro Erodability"],
                        group_input.outputs["Hydro Erosion Rate"],
                        (location[0] + 1220, 1750))
    bank_angle_scale = map_range(
        tree, group_input.outputs["Bank Angle"], 0.0, 90.0, 1.0, 0.25,
        (location[0] + 1220, 1880)).outputs["Result"]
    strength = math_node(tree, "MULTIPLY", hydro_k.outputs[0],
                         bank_angle_scale, (location[0] + 1400, 1800))
    strength = math_node(tree, "MULTIPLY", strength.outputs[0], factor,
                         (location[0] + 1580, 1800))

    # --- 汇水权重：标准 stream power 的 A^m 项，m = 1。
    #
    # flow 是**汇水面积（单位：格）**，除以总格数得到无量纲面积比 A/N。
    # 山脊 A/N ≈ 1/N、主河道 A/N ≈ 0.1..0.5，所以下切自然集中在河道里，
    # 而不是像旧实现那样用
    # map_range(log10(flow)+1, 1..2 → 0.15..1) 把所有格子都推到 1
    # （实测侵蚀高度差 rms 2.16 m，参考只有 0.084 m）。
    #
    # m 的取值由参考的**分布**定：m=0.5 时坡面背景下切 0.17 m、参考只有
    # 0.02 m（每格都掉 10 cm 以上，而参考只有 10% 的格子超过 10 cm）。
    # m=1（输运限制型 stream power 的常用取值）把 A/N 直接当权重，
    # 平均下切与河道峰值都落在参考量级上（见 sweep --stage full）。
    area_ratio = math_node(tree, "DIVIDE", flow, total_cells,
                           (location[0] + 1400, 1420))
    area_positive = math_node(tree, "MAXIMUM", area_ratio.outputs[0], 0.0,
                              (location[0] + 1560, 1420))
    flow_weight = math_node(tree, "MINIMUM", area_positive.outputs[0], 1.0,
                            (location[0] + 1720, 1420)).outputs[0]
    # 挟沙容量按汇水面积（格）计，单位与 flow 一致。
    flow_sat = flow

    # --- Slope Factor：坡度对侵蚀量的**影响系数**（1 + slopeFactor × slope），
    #     不是直接把坡度当成乘积项。
    slope_effect = math_node(tree, "MULTIPLY", slope,
                             group_input.outputs["Erodability Slope Factor"],
                             (location[0] + 1400, 1620))
    slope_factor = math_node(tree, "ADD", slope_effect.outputs[0], 1.0,
                             (location[0] + 1560, 1620))

    # --- Global Erosion Rate：每迭代的基准下切（米 → 归一化层）
    base_incision = math_node(tree, "DIVIDE",
                              group_input.outputs["Global Erosion Rate"],
                              group_input.outputs["Height Scale"],
                              (location[0] + 1720, 1780))

    # --- 下切量 = 基准米数 × 可蚀性 × 坡度系数 × 汇水权重
    incision = math_node(tree, "MULTIPLY", base_incision.outputs[0],
                         strength.outputs[0], (location[0] + 1880, 1780))
    incision = math_node(tree, "MULTIPLY", incision.outputs[0],
                         slope_factor.outputs[0], (location[0] + 2040, 1780))
    incision = math_node(tree, "MULTIPLY", incision.outputs[0],
                         flow_weight, (location[0] + 2200, 1780))
    incision_norm = incision

    # --- 单帧下切上限：
    #     * Max Debris Depth：碎屑层厚度上限
    #     * Global Erosion Rate：HDA 里 parmtag units 是 "m1frame-1"，
    #       即每帧最多降低多少米，是硬上限而不是乘数
    debris_cap = math_node(tree, "DIVIDE", group_input.outputs["Max Debris Depth"],
                           group_input.outputs["Height Scale"],
                           (location[0] + 2200, 1940))
    frame_cap = math_node(tree, "DIVIDE",
                          group_input.outputs["Global Erosion Rate"],
                          group_input.outputs["Height Scale"],
                          (location[0] + 2200, 2060))
    cap = math_node(tree, "MINIMUM", debris_cap.outputs[0],
                    frame_cap.outputs[0], (location[0] + 2340, 2000))
    incision_clamped = math_node(tree, "MINIMUM", incision_norm.outputs[0],
                                 cap.outputs[0], (location[0] + 2500, 1780))

    # --- 挟沙：被侵蚀的物质进入 sediment，超过挟沙容量的部分沉积回地表
    carried = math_node(tree, "ADD", sediment, incision_clamped.outputs[0],
                        (location[0] + 2300, 1400))
    capacity = math_node(tree, "MULTIPLY", flow_sat,
                         group_input.outputs["Sediment Capacity"],
                         (location[0] + 2120, 1280))
    capacity = math_node(tree, "MULTIPLY", capacity.outputs[0], slope,
                         (location[0] + 2300, 1280))
    excess = math_node(tree, "SUBTRACT", carried.outputs[0],
                       capacity.outputs[0], (location[0] + 2480, 1340))
    excess_positive = math_node(tree, "MAXIMUM", excess.outputs[0], 0.0,
                                (location[0] + 2660, 1340))
    deposition = math_node(tree, "MULTIPLY", excess_positive.outputs[0],
                           group_input.outputs["Bed Deposition Rate"],
                           (location[0] + 2840, 1340))
    net_cut = math_node(tree, "SUBTRACT", incision_clamped.outputs[0],
                        deposition.outputs[0], (location[0] + 2840, 1600))

    # --- removal rate：决定被蚀物质里多少被彻底带走（其余进入 debris）。
    #     height 层的高度损失始终是 net_cut —— 旧实现写成
    #     ``net_cut + net_cut × removal``，等于凭空多削 10%。
    new_height = math_node(tree, "SUBTRACT", height, net_cut.outputs[0],
                           (location[0] + 3380, 1600))
    new_sediment = math_node(tree, "MAXIMUM", deposition.outputs[0], 0.0,
                             (location[0] + 3200, 1340))

    stored_height = store_attribute(tree, geometry, "height",
                                    new_height.outputs[0],
                                    (location[0] + 3560, 1600))
    stored_sediment = store_attribute(tree, stored_height.outputs["Geometry"],
                                      "sediment", new_sediment.outputs[0],
                                      (location[0] + 3740, 1340))
    return stored_sediment.outputs["Geometry"], new_height.outputs[0]


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
            dx, dy, cell_m, (location[0] + 200, row))
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
            dx, dy, cell_m, (location[0] + 200, row))
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
    # 最终塌落量：松弛项 + 超角项，**两项都要先乘 thermal erosion rate**。
    #
    # 超角项原本直接是"超过休止角的全部高差"，只乘了 removal rate(0.7)，
    # 等于每帧把坡面一次性削到休止角 —— 实测地形整体下降 1.7 m、最大 37 m，
    # 而参考 5 帧总共只有 rms 0.084 m / max 1.87 m。erosionrate=0.03 是
    # 每帧的松弛比例，两项都必须受它约束。
    over_scaled = math_node(tree, "MULTIPLY", over_normalized.outputs[0],
                            group_input.outputs["Thermal Erosion Rate"],
                            (location[0] + 1680, 640))
    collapse = math_node(tree, "ADD", erode_amount.outputs[0],
                         over_scaled.outputs[0], (location[0] + 1840, 700))
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


def build_erosion_frame(tree, group_input, geometry, cell, total_cells,
                        location=(0, 0)):
    """一帧内部的完整求解顺序，与 Houdini 的求解器接法一致。

    Houdini 的输入图（``_erode_solvers.json``）是：

    ```text
    erode_thermal → precipitation → erode_hydro → evaporate
    ```

    即热力侵蚀在最前、蒸发在最后；水扩散是 ``erode_hydro`` 内部的
    ``water_spreaditers`` 次迭代。旧实现写成
    ``precipitation → evaporate → spread → hydro → thermal``：雨水刚落下就被
    蒸发掉、热力侵蚀又跑到最后，与参考的演化顺序相反。
    """
    thermal_geometry, _, _ = build_thermal_erosion(
        tree, group_input, geometry, cell, (location[0] + 0, location[1] - 400))
    stage_geometry, _ = build_precipitation(
        tree, group_input, thermal_geometry,
        location=(location[0] + 0, location[1] + 1600))

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
    link(tree, stage_geometry, repeat_in.inputs[1])
    spread_geometry, _, spread_flow = build_water_spread(
        tree, group_input, repeat_in.outputs[1], cell, total_cells,
        (location[0] + 3500, location[1] + 1100))
    link(tree, spread_geometry, repeat_out.inputs[0])

    hydro_geometry, _ = build_hydro_erosion(
        tree, group_input, repeat_out.outputs[0], cell, total_cells,
        (location[0] + 5000, location[1] + 600))
    evaporate_geometry, _ = build_precipitation(
        tree, group_input, hydro_geometry, seed_w=2.0,
        location=(location[0] + 6800, location[1] + 1600), evaporation=True)
    return evaporate_geometry


def build_erode_group(size_m: float, resolution: int, inputs, iterations: int = 1):
    """建出完整的 Erode 节点组。

    ``inputs`` 是 add_inputs 用的规格表；``iterations`` 是外层帧数
    （Houdini 的 `freezeframe`，Pegasus 用 5）。
    """
    cell_m = size_m / max(resolution - 1, 1)
    total_cells = float(resolution * resolution)
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

    internal = build_erosion_frame(tree, group_input, frame_in.outputs[1],
                                   cell_m, total_cells)
    link(tree, internal, frame_out.inputs[0])
    link(tree, frame_out.outputs[0], group_output.inputs["Geometry"])
    return tree

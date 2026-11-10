"""阶段 01 的几何节点实现：Pegasus 高度场基座 + 一轮侵蚀。

这一层对应 Houdini ``sop_pe_landscape.1.0.hda`` 的前半段：

```text
heightfield 网格
  -> Noise 1   (Perlin / hmfT, amp 200, element 900)
  -> Coast     (高度向海岸的衰减偏移; Pegasus 原工程由 heightfield 底图提供)
  -> Noise 2   (sparse, amp 90, 280)
  -> Distort   (curl, amp 140, element 800)
  -> Remap     (把噪声海拔重映射到 [0,1] 高度层)
  -> Erode     (一轮: 流向下坡 + 流量累积 + 河流下切 + 热力塌落)
```

与原工程的差别（都在节点注释里说明）：

* Houdini 的 ``heightfield_erode`` 是 GPU 体积算例，带 ``freezeframe`` 迭代
  与降水/水体求解；这里的侵蚀是**近似**：用 ``SampleNearestSurface`` 采样
  下坡邻点的坡度、用 ``SampleIndex`` 累加汇水，再按 stream-power 的
  ``K * A^m * S^n`` 下切，最后做一次热力角塌落。它保留 Pegasus 地形的
  “树枝状沟谷 + 崩积坡”特征，但不是逐位复现。
* 所有数值参数都暴露成节点组输入，可以在 Blender 里直接拖。

本模块只被 Blender 内的 ``build_pegasus_stage01.py`` 调用，不依赖 bpy 之外的包。
"""

from __future__ import annotations

import math

import bpy


# ---------------------------------------------------------------- 基础工具


def link(tree, source, target) -> None:
    """连线；``source``/``target`` 可以传节点，会自动取第一个输出/输入。

    移植代码里大量 ``link(tree, some_math_node, inputs["Value"])`` 的写法，
    直接把节点对象交给 ``links.new`` 会抛 TypeError，而 Blender 又是静默
    失败/部分成功，所以这里统一解包。
    """
    if hasattr(source, "outputs") and not hasattr(source, "is_output"):
        source = source.outputs[0]
    if hasattr(target, "inputs") and not hasattr(target, "is_output"):
        target = target.inputs[0]
    if source is None or target is None:
        raise TypeError("link() got None socket")
    tree.links.new(source, target)


def io(tree, node, identifier: str):
    """按 ``identifier`` 取节点 socket。

    不能只按名字取：``StoreNamedAttribute`` 在 FLOAT 模式下同时暴露
    ``Geometry``（无标识字段槽）和 ``Value``（字段槽），``Value`` 上方还有
    一个隐式几何 socket，按名字 ``Value`` 拿到的是错误槽位——侵蚀段因此把
    高度写进了几何输入，属性全丢。标识如 ``"Socket_2"`` / ``"Socket_3"``。
    """
    for socket in node.inputs:
        if socket.identifier == identifier:
            return socket
    for socket in node.outputs:
        if socket.identifier == identifier:
            return socket
    raise KeyError(f"{node.bl_idname} has no socket {identifier!r}: "
                   f"{[(s.name, s.identifier) for s in node.inputs]}")


def input_socket(node, socket_name: str, socket_type: str):
    """按"名字 + 类型"定位输入槽。

    Blender 5.x 的若干节点会在 ``inputs`` 里同时出现同名但类型不同的隐式槽
    （例如 Set Position 的 ``Position`` 既有字段版也有几何版），只按名字或
    只按类型都可能命中错误的那个，导致整张图静默算错。
    """
    for socket in node.inputs:
        if socket.name == socket_name and socket.type == socket_type:
            return socket
    raise KeyError(
        f"{node.bl_idname}: no input {socket_name!r} of type {socket_type}; "
        f"have {[(s.name, s.type) for s in node.inputs]}")


def position_of_position(tree, location=(0, 0)):
    """当前位置字段。

    同一张图里不只有字段节点（``GeometryNodeInputPosition``）会叫
    "Position"；``SetPosition`` 自己的绝对坐标槽、组输入/输出槽也可能
    叫这个名字。本函数专门用于"读取当前点坐标"的场景，返回字段 socket。
    """
    node = tree.nodes.new("GeometryNodeInputPosition")
    node.location = location
    return node.outputs["Position"]


def set_position(tree, geometry, offset, location=(0, 0)):
    """相对位移节点。

    ``Position`` 是**绝对**坐标赋值、``Offset`` 是**相对**偏移；两者都是
    VECTOR，按类型取 socket 会取到 ``Position``，结果把整张网格（连同
    属性）覆盖成偏移量本身。所以这里按标识精确取 ``Offset``。
    """
    node = tree.nodes.new("GeometryNodeSetPosition")
    node.location = location
    link(tree, geometry, input_socket(node, "Geometry", "GEOMETRY"))
    if offset is not None:
        offset_socket = next(
            (s for s in node.inputs if s.name == "Offset" and s.type == "VECTOR"),
            None)
        if offset_socket is None:
            raise KeyError("Set Position has no Offset vector socket")
        link(tree, offset, offset_socket)
    return node


def store_attribute(tree, geometry, name: str, value, location=(0, 0)):
    """写一个 FLOAT 点属性，返回节点（取几何用 ``.outputs[0]``）。

    Blender 5.x 的 Store Named Attribute 在 ``inputs`` 末尾除了可见的
    ``Geometry/Selection/Name/Value`` 之外，还可能有同名的隐式字段槽；
    只按类型找 ``VALUE`` 会命中错的那个。因此按"名字 + 类型"双条件定位，
    找不到才退回到类型匹配。
    """
    node = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    node.location = location
    node.data_type = "FLOAT"
    node.domain = "POINT"

    def pick(socket_name: str, socket_type: str):
        for socket in node.inputs:
            if socket.name == socket_name and socket.type == socket_type:
                return socket
        for socket in node.inputs:
            if socket.type == socket_type:
                return socket
        raise KeyError(f"{node.bl_idname}: no {socket_name!r} {socket_type} socket, "
                       f"have {[(s.name, s.type) for s in node.inputs]}")

    pick("Name", "STRING").default_value = name
    link(tree, geometry, pick("Geometry", "GEOMETRY"))
    value_socket = pick("Value", "VALUE")
    if value is not None:
        link(tree, value, value_socket)
    else:
        value_socket.default_value = 0.0
    return node


def math_node(tree, operation: str, a=None, b=None, location=(0, 0)):
    """标量数学节点；``a``/``b`` 可以是数值或 socket。"""
    node = tree.nodes.new("ShaderNodeMath")
    node.operation = operation
    node.location = location
    for index, value in enumerate((a, b)):
        if value is None:
            continue
        if isinstance(value, (int, float)):
            node.inputs[index].default_value = float(value)
        else:
            link(tree, value, node.inputs[index])
    return node


def vector_math(tree, operation: str, a=None, b=None, location=(0, 0)):
    node = tree.nodes.new("ShaderNodeVectorMath")
    node.operation = operation
    node.location = location
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
              location=(0, 0), clamp: bool = True, interpolation: str = "LINEAR"):
    node = tree.nodes.new("ShaderNodeMapRange")
    node.location = location
    node.clamp = clamp
    node.interpolation_type = interpolation
    node.inputs["From Min"].default_value = float(from_min)
    node.inputs["From Max"].default_value = float(from_max)
    node.inputs["To Min"].default_value = float(to_min)
    node.inputs["To Max"].default_value = float(to_max)
    link(tree, value, node.inputs["Value"])
    return node


def smoothstep(tree, value, edge0: float, edge1: float, location=(0, 0)):
    return map_range(tree, value, edge0, edge1, 0.0, 1.0, location,
                     interpolation="SMOOTHSTEP").outputs["Result"]


def noise(tree, vector_socket, w: float, scale: float, detail: float,
          roughness: float, location=(0, 0), distortion: float = 0.0):
    node = tree.nodes.new("ShaderNodeTexNoise")
    node.location = location
    node.noise_dimensions = "4D"
    node.inputs["W"].default_value = float(w)
    node.inputs["Scale"].default_value = float(scale)
    node.inputs["Detail"].default_value = float(detail)
    node.inputs["Roughness"].default_value = float(roughness)
    node.inputs["Distortion"].default_value = float(distortion)
    link(tree, vector_socket, node.inputs["Vector"])
    return node


def named_attribute(tree, name: str, location=(0, 0)):
    node = tree.nodes.new("GeometryNodeInputNamedAttribute")
    node.data_type = "FLOAT"
    node.location = location
    node.inputs["Name"].default_value = name
    return node.outputs["Attribute"]


def socket_id(tree, name: str) -> str:
    for item in tree.interface.items_tree:
        if getattr(item, "in_out", None) == "INPUT" and item.name == name:
            return item.identifier
    raise KeyError(f"no input socket named {name!r}")


def add_inputs(tree, specs) -> None:
    """``specs`` = [(名字, 类型, 默认值, 最小值, 最大值, 描述)]。"""
    for name, socket_type, default, minimum, maximum, description in specs:
        socket = tree.interface.new_socket(
            name=name, in_out="INPUT", socket_type=socket_type)
        socket.default_value = default
        socket.min_value = minimum
        socket.max_value = maximum
        socket.description = description


def scaled(tree, value, group_input, name: str, location=(0, 0)):
    """``value * 组输入[name]``；组输入是 socket，不能直接做 Python 运算。"""
    return math_node(tree, "MULTIPLY", value, group_input.outputs[name],
                     location=location)


def ui_value(group_input, name: str):
    return group_input.outputs[name]


# ---------------------------------------------------------------- 高度场基座


def build_base_heightfield(tree, ui, group_input, size_m: float, resolution: int):
    """建网格 + 两层噪声 + 海岸 + 扭曲，返回 (网格 socket, 归一化高度 socket)。

    ``ui`` 是 {名字: 组输入 socket} 的字典，``group_input`` 是 NodeGroupInput 节点。
    """
    grid = tree.nodes.new("GeometryNodeMeshGrid")
    grid.location = (-2400, 600)
    grid.inputs["Size X"].default_value = float(size_m)
    grid.inputs["Size Y"].default_value = float(size_m)
    grid.inputs["Vertices X"].default_value = int(resolution)
    grid.inputs["Vertices Y"].default_value = int(resolution)

    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (-2400, 380)
    # Mesh Grid 的坐标范围本来就是 [-size/2, +size/2]，除以 size/2 直接得到
    # [0,1] 的 UV；不能像以前那样再 +0.5，否则 UV 落在 [0.5,1.5]，径向距离
    # 恒大于海岸阈值，整张图都会被判成海。
    uv = vector_math(tree, "DIVIDE", position.outputs["Position"],
                     (size_m * 0.5, size_m * 0.5, 1.0), (-2200, 380))
    uv_centered = vector_math(tree, "ADD", uv.outputs["Vector"], (0.5, 0.5, 0.0),
                              (-2020, 380))
    uv_flat = vector_math(tree, "MULTIPLY", uv_centered.outputs["Vector"],
                          (1.0, 1.0, 0.0), (-1840, 380))

    # --- 噪声 1：大尺度山体轮廓（Houdini: heightfield_noise1）
    # element 900 m 的半波长 ≈ 900 / 8000 的 UV 频率；Blender 的 Scale 是
    # "每单位 UV 的重复次数"，所以 1 / element_uv。amp 按高度层的比例缩放。
    large = noise(tree, uv_flat.outputs["Vector"], w=1.7,
                  scale=1.0 / (8000.0 * 0.001 / 8000.0),
                  detail=6.0, roughness=0.5, location=(-1600, 700))
    # 频率来自组输入：Scale = 1 / (Element / Size)。这里用 Math 节点现场算，
    # 保证在 Blender 里拖动 Element 时频率跟着变。
    large_freq = math_node(tree, "DIVIDE", 1.0, math_node(
        tree, "DIVIDE", group_input.outputs["Noise 1 Element"], size_m,
        location=(-1800, 780)).outputs[0], location=(-1700, 760))
    link(tree, large_freq.outputs[0], large.inputs["Scale"])
    large_centered = math_node(tree, "SUBTRACT", large.outputs["Fac"], 0.5,
                               location=(-1420, 700))
    large_amp = scaled(tree, large_centered.outputs[0], group_input,
                       "Noise 1 Amplitude", (-1240, 700))

    # --- 噪声 2：中尺度山脊（Houdini: heightfield_noise2, sparse）
    medium = noise(tree, uv_flat.outputs["Vector"], w=11.3,
                   scale=1.0 / 0.035,
                   detail=5.0, roughness=0.55, location=(-1600, 420))
    medium_freq = math_node(tree, "DIVIDE", 1.0, math_node(
        tree, "DIVIDE", group_input.outputs["Noise 2 Element"], size_m,
        location=(-1800, 500)).outputs[0], location=(-1700, 480))
    link(tree, medium_freq.outputs[0], medium.inputs["Scale"])
    medium_centered = math_node(tree, "SUBTRACT", medium.outputs["Fac"], 0.5,
                                location=(-1420, 420))
    medium_amp = scaled(tree, medium_centered.outputs[0], group_input,
                        "Noise 2 Amplitude", (-1240, 420))

    stacked = math_node(tree, "ADD", large_amp.outputs[0], medium_amp.outputs[0],
                        location=(-1060, 560))

    # --- 海岸：Pegasus 原工程由 heightfield 底图给出“海平面”，这里用径向
    # 衰减 + 低频噪声扰动，保证岛屿边缘落回 0。
    # 注意 UV 范围：uv_centered 落在 [0,1]，所以要减 0.5 才是中心到边缘；
    # 归一化后距离最大约 0.707，阈值必须按这个尺度给。
    radial = vector_math(tree, "SUBTRACT", uv_centered.outputs["Vector"],
                         (0.5, 0.5, 0.0), (-1840, 160))
    radial_len = vector_math(tree, "LENGTH", radial.outputs["Vector"],
                             location=(-1660, 160))
    coast_perturb = noise(tree, uv_flat.outputs["Vector"], w=31.7, scale=2.4,
                          detail=3.0, roughness=0.5, location=(-1660, -60))
    coast_noise = math_node(tree, "SUBTRACT", coast_perturb.outputs["Fac"], 0.5,
                            location=(-1480, -60))
    coast_warp = scaled(tree, coast_noise.outputs[0], group_input,
                        "Coast Wobble", (-1300, -60))
    radial_wobbled = math_node(tree, "ADD", radial_len.outputs["Value"],
                               coast_warp.outputs[0], location=(-1120, 60))
    # 0.30 以内是纯陆地，0.62 以外完全落海；中间是过渡滩涂。
    coast = map_range(tree, radial_wobbled.outputs[0],
                      0.30, 0.62, 1.0, 0.0, (-940, 60),
                      interpolation="SMOOTHSTEP").outputs["Result"]

    # --- 扭曲（Houdini: heightfield_distort1, curl）
    # Blender 的 Noise 4D 没有 curl；用两个去相关的噪声当偏移场，得到同样的
    # “抹开边界、拉出坡度”效果。强度按 Distort Amplitude 缩放 UV 偏移。
    distort_freq = math_node(tree, "DIVIDE", 1.0, math_node(
        tree, "DIVIDE", group_input.outputs["Distort Element"], size_m,
        location=(-2000, -680)).outputs[0], location=(-1900, -700))
    warp_x = noise(tree, uv_flat.outputs["Vector"], w=51.1, scale=1.0 / 0.1,
                   detail=2.0, roughness=0.5, location=(-1840, -320))
    warp_y = noise(tree, uv_flat.outputs["Vector"], w=77.3, scale=1.0 / 0.1,
                   detail=2.0, roughness=0.5, location=(-1840, -540))
    link(tree, distort_freq.outputs[0], warp_x.inputs["Scale"])
    link(tree, distort_freq.outputs[0], warp_y.inputs["Scale"])
    warp_x_c = math_node(tree, "SUBTRACT", warp_x.outputs["Fac"], 0.5,
                         location=(-1660, -320))
    warp_y_c = math_node(tree, "SUBTRACT", warp_y.outputs["Fac"], 0.5,
                         location=(-1660, -540))
    warp_x_s = math_node(tree, "MULTIPLY", warp_x_c.outputs[0], 0.035,
                         location=(-1480, -320))
    warp_y_s = math_node(tree, "MULTIPLY", warp_y_c.outputs[0], 0.035,
                         location=(-1480, -540))
    warp_vec = tree.nodes.new("ShaderNodeCombineXYZ")
    warp_vec.location = (-1300, -420)
    link(tree, warp_x_s.outputs[0], warp_vec.inputs["X"])
    link(tree, warp_y_s.outputs[0], warp_vec.inputs["Y"])
    warped_uv = vector_math(tree, "ADD", uv_flat.outputs["Vector"],
                            warp_vec.outputs["Vector"], (-1120, -420))

    # --- 重映射（Houdini: heightfield_remap1）：把噪声海拔压进 [0,1]
    #
    # 关键：coast 与 stacked 都是 Position 的**字段**。一旦网格被 SetPosition
    # 抬高，这两种字段在位移之后会重新求值成完全不同的值，导致海面覆盖失效、
    # 整个岛外变成高频噪声。所以这里用 Capture Attribute 在位移之前把它们
    # **固定成数据**，下游只读这两条固定值，不再依赖 Position。
    captured = tree.nodes.new("GeometryNodeCaptureAttribute")
    captured.location = (-880, -700)
    captured.domain = "POINT"
    captured.capture_items.new("FLOAT", "height_field")
    captured.capture_items.new("FLOAT", "coast_field")
    link(tree, grid.outputs["Mesh"], captured.inputs["Geometry"])
    link(tree, stacked.outputs[0], captured.inputs["height_field"])
    link(tree, coast, captured.inputs["coast_field"])
    captured_geometry = captured.outputs["Geometry"]
    height_field = captured.outputs["height_field"]
    coast_field = captured.outputs["coast_field"]

    remapped = map_range(tree, height_field, -0.35, 0.35, 0.0, 1.0,
                         (-600, 560), clamp=True).outputs["Result"]
    # 海岸掩膜：coast=1 取 remapped 原值，coast=0 取常数 SEA_FLOOR（略低于
    # 海平面）。用线性混合而不是相乘——相乘会留下 -0.18 × 噪声的起伏，
    # 岛外海面因此在 hillshade 里放大成整片噪声。
    SEA_FLOOR = -0.18
    blend = map_range(tree, coast_field, 0.0, 1.0, 0.0, 1.0,
                      (-600, 200)).outputs["Result"]
    height_centered = math_node(tree, "SUBTRACT", remapped, SEA_FLOOR,
                                location=(-600, 480))
    height_scaled = math_node(tree, "MULTIPLY", height_centered.outputs[0], blend,
                              location=(-420, 480))
    masked = math_node(tree, "ADD", height_scaled.outputs[0], SEA_FLOOR,
                       location=(-240, 480))
    raised = math_node(tree, "ADD", masked.outputs[0],
                       group_input.outputs["Sea Level"], location=(-60, 480))

    # 最终高度也必须在位移前固定成数据：否则 SetPosition 之后 "height"
    # 字段会用新坐标重新求值，岛外又会冒出噪声。
    final_capture = tree.nodes.new("GeometryNodeCaptureAttribute")
    final_capture.location = (-60, 400)
    final_capture.domain = "POINT"
    final_capture.capture_items.new("FLOAT", "final_height")
    link(tree, captured_geometry, final_capture.inputs["Geometry"])
    link(tree, raised.outputs[0], final_capture.inputs["final_height"])
    return (final_capture.outputs["Geometry"], final_capture.outputs["final_height"],
            warped_uv.outputs["Vector"], coast_field)


# ---------------------------------------------------------------- 侵蚀


def _descent_neighbour(tree, size_m: float, cell_m: float, x: int, y: int,
                       location, source_index_socket=None):
    """返回 (目标偏移向量, 相对位置系数)。

    ``x``/``y`` 是邻点方向的格数；返回的偏移让 ``SampleNearestSurface``
    落在该邻点位置。
    """
    dx = x * cell_m
    dy = y * cell_m
    return (dx, dy, 0.0), math.hypot(dx, dy)


def build_one_erosion_pass(tree, ui, group_input, geometry, height_socket,
                           size_m: float, resolution: int):
    """一轮 stream-power 侵蚀 + 热力塌落。

    步骤（都在几何节点里求值，没有 Python 回调）：

    1. 8 邻域采样高度，算最大下坡落差 ``S`` 与下坡方向。
    2. 按下坡方向把汇水从上游往下游传播：用 ``Repeat`` 区域把
       ``flow`` 逐级搬到下坡邻居，迭代次数即河谷长度。
    3. 下切量 ``K * flow^m * S^n``，从当前高度里减去。
    4. 热力塌落：与邻点高差超过休止角的部分向邻居转移一定比例。

    返回 (新几何, 新高度 socket)。
    """
    node_groups = (
        ("SampleNearestSurface", "GeometryNodeSampleNearestSurface"),
    )
    del node_groups  # 采样节点直接用

    position = tree.nodes.new("GeometryNodeInputPosition")
    position.location = (180, 1800)

    neighbours = [(1, 0), (-1, 0), (0, 1), (0, -1),
                  (1, 1), (1, -1), (-1, 1), (-1, -1)]

    def sample_offset(dx: float, dy: float, location):
        offset = tree.nodes.new("ShaderNodeCombineXYZ")
        offset.location = location
        offset.inputs["X"].default_value = dx
        offset.inputs["Y"].default_value = dy
        probe = vector_math(tree, "ADD", position.outputs["Position"],
                            offset.outputs["Vector"],
                            (location[0] + 180, location[1]))
        sampler = tree.nodes.new("GeometryNodeSampleNearestSurface")
        sampler.location = (location[0] + 360, location[1])
        sampler.data_type = "FLOAT"
        link(tree, geometry, sampler.inputs["Mesh"])
        link(tree, height_socket, sampler.inputs["Value"])
        link(tree, probe.outputs["Vector"], sampler.inputs["Sample Position"])
        return sampler.outputs["Value"], sampler.outputs["Is Valid"]

    # --- 1. 最大下坡落差与方向
    #
    # 单位注意：height 属性是**归一化高度层**（0..1 ≈ 0..HeightScale 米），
    # 而格子间距是米。如果直接用"归一化高度 / 米"当坡度，数值会小到 1e-4，
    # 后面 flow^m * S^n 的下切量就恒为 0（实测 erode_flow 与 height 都不变）。
    # 这里把落差换算成米再除间距，得到真正的无量纲坡度。
    cell = size_m / max(resolution - 1, 1)
    height_m = math_node(tree, "MULTIPLY", height_socket,
                         group_input.outputs["Height Scale"],
                         location=(180, 1650))
    drop_max = None
    dir_x = None
    dir_y = None
    valid_any = None
    for index, (nx, ny) in enumerate(neighbours):
        length = math.hypot(nx, ny) * cell
        sampled, valid = sample_offset(nx * cell, ny * cell,
                                       (400, 1500 - index * 240))
        # 邻点高度同样先换算成米，再算落差与坡度。
        sampled_m = math_node(tree, "MULTIPLY", sampled,
                              group_input.outputs["Height Scale"],
                              location=(580, 1440 - index * 240))
        drop = math_node(tree, "SUBTRACT", height_m.outputs[0],
                         sampled_m.outputs[0],
                         location=(700, 1500 - index * 240))
        slope = math_node(tree, "DIVIDE", drop.outputs[0], length,
                          location=(880, 1500 - index * 240))
        normalised_x = math_node(tree, "DIVIDE", nx, math.hypot(nx, ny),
                                 location=(880, 1380 - index * 240))
        normalised_y = math_node(tree, "DIVIDE", ny, math.hypot(nx, ny),
                                 location=(880, 1260 - index * 240))
        if drop_max is None:
            drop_max = slope.outputs[0]
            dir_x = normalised_x.outputs[0]
            dir_y = normalised_y.outputs[0]
            valid_any = valid
            continue
        better = tree.nodes.new("FunctionNodeCompare")
        better.location = (1060, 1500 - index * 240)
        better.data_type = "FLOAT"
        better.operation = "GREATER_THAN"
        link(tree, slope.outputs[0], better.inputs["A"])
        link(tree, drop_max, better.inputs["B"])
        pick_x = tree.nodes.new("GeometryNodeSwitch")
        pick_x.location = (1240, 1440 - index * 240)
        pick_x.input_type = "FLOAT"
        link(tree, better.outputs["Result"], pick_x.inputs["Switch"])
        link(tree, dir_x, pick_x.inputs["False"])
        link(tree, normalised_x.outputs[0], pick_x.inputs["True"])
        pick_y = tree.nodes.new("GeometryNodeSwitch")
        pick_y.location = (1240, 1320 - index * 240)
        pick_y.input_type = "FLOAT"
        link(tree, better.outputs["Result"], pick_y.inputs["Switch"])
        link(tree, dir_y, pick_y.inputs["False"])
        link(tree, normalised_y.outputs[0], pick_y.inputs["True"])
        pick_s = tree.nodes.new("GeometryNodeSwitch")
        pick_s.location = (1240, 1500 - index * 240)
        pick_s.input_type = "FLOAT"
        link(tree, better.outputs["Result"], pick_s.inputs["Switch"])
        link(tree, drop_max, pick_s.inputs["False"])
        link(tree, slope.outputs[0], pick_s.inputs["True"])
        drop_max = pick_s.outputs["Output"]
        dir_x = pick_x.outputs["Output"]
        dir_y = pick_y.outputs["Output"]

    max_slope = math_node(tree, "MAXIMUM", drop_max, 0.0, location=(1420, 1500))

    # --- 2. 汇水面积：按下坡方向做一轮流量扩散。
    #
    # 几何节点没有"按下坡方向写邻居"的能力，所以这里用的是近似：
    # 每个点取**下坡邻居**已经累积的流量，按 Flow Merge 比例并进自己的流量。
    # 一轮只能传递一格，所以 flow 的最大值不会超过 1/(1-Merge)；真正的
    # 长程汇水要靠 Repeat 区域多轮迭代（下一步再做），否则沟谷偏碎。
    flow = named_attribute(tree, "erode_flow", (1240, 900))
    flow_sampler_position = vector_math(tree, "ADD", position.outputs["Position"],
                                        (0.0, 0.0, 0.0), (1600, 1020))
    direction_vec = tree.nodes.new("ShaderNodeCombineXYZ")
    direction_vec.location = (1600, 900)
    link(tree, dir_x, direction_vec.inputs["X"])
    link(tree, dir_y, direction_vec.inputs["Y"])
    downhill_probe = vector_math(
        tree, "ADD", flow_sampler_position.outputs["Vector"],
        vector_math(tree, "MULTIPLY", direction_vec.outputs["Vector"],
                    (cell, cell, 0.0), (1780, 900)).outputs["Vector"],
        (1960, 900))
    flow_sampler = tree.nodes.new("GeometryNodeSampleNearestSurface")
    flow_sampler.location = (2140, 900)
    flow_sampler.data_type = "FLOAT"
    link(tree, geometry, flow_sampler.inputs["Mesh"])
    link(tree, flow, flow_sampler.inputs["Value"])
    link(tree, downhill_probe.outputs["Vector"], flow_sampler.inputs["Sample Position"])

    has_downhill = tree.nodes.new("FunctionNodeCompare")
    has_downhill.location = (2140, 1100)
    has_downhill.data_type = "FLOAT"
    has_downhill.operation = "GREATER_THAN"
    link(tree, max_slope.outputs[0], has_downhill.inputs["A"])
    has_downhill.inputs["B"].default_value = 0.0

    picked_flow = tree.nodes.new("GeometryNodeSwitch")
    picked_flow.location = (2320, 900)
    picked_flow.input_type = "FLOAT"
    link(tree, has_downhill.outputs["Result"], picked_flow.inputs["Switch"])
    picked_flow.inputs["False"].default_value = 0.0
    link(tree, flow_sampler.outputs["Value"], picked_flow.inputs["True"])

    merge_rate = math_node(tree, "MULTIPLY", picked_flow.outputs["Output"],
                           math_node(tree, "MAXIMUM",
                                     group_input.outputs["Flow Merge"], 0.0,
                                     location=(2320, 1020)).outputs[0],
                           location=(2500, 960))
    # 自身降雨 1.0 加上下坡邻居并入的流量；flow 的上限约 1/(1-Merge)，
    # 所以汇水是"局部累积"，多轮会话才需要 Repeat 区域。
    rainfall = math_node(tree, "MULTIPLY", 1.0, 1.0, location=(2500, 800))
    rainfall.outputs[0].default_value = 1.0
    grown = math_node(tree, "ADD", flow,
                      math_node(tree, "ADD", merge_rate.outputs[0],
                                rainfall.outputs[0],
                                location=(2680, 860)).outputs[0],
                      location=(2860, 960))

    # --- 3. 下切：K * flow^m * S^n
    flow_pow = math_node(tree, "POWER", grown.outputs[0],
                         group_input.outputs["Flow Exponent"], location=(3040, 960))
    slope_pow = math_node(tree, "POWER", max_slope.outputs[0],
                          group_input.outputs["Slope Exponent"], location=(3040, 1120))
    stream_power = math_node(tree, "MULTIPLY", flow_pow.outputs[0],
                             slope_pow.outputs[0], location=(3220, 1040))
    incision = math_node(tree, "MULTIPLY", stream_power.outputs[0],
                         group_input.outputs["Erosion Strength"],
                         location=(3400, 1040))
    incision_clamped = math_node(tree, "MINIMUM", incision.outputs[0],
                                 group_input.outputs["Max Incision"],
                                 location=(3580, 1040))
    cut = math_node(tree, "SUBTRACT", height_socket, incision_clamped.outputs[0],
                    location=(3760, 1040))

    store_cut = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store_cut.location = (3940, 1040)
    store_cut.data_type = "FLOAT"
    store_cut.domain = "POINT"
    store_cut.inputs["Name"].default_value = "height"
    link(tree, geometry, store_cut.inputs["Geometry"])
    link(tree, cut.outputs[0], store_cut.inputs["Value"])

    # 把这一轮累积出来的 flow 写回属性，否则下一次采样 erode_flow 全是 0。
    store_flow_out = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store_flow_out.location = (4120, 1240)
    store_flow_out.data_type = "FLOAT"
    store_flow_out.domain = "POINT"
    store_flow_out.inputs["Name"].default_value = "erode_flow"
    link(tree, store_cut.outputs["Geometry"], store_flow_out.inputs["Geometry"])
    link(tree, grown.outputs[0], store_flow_out.inputs["Value"])

    # --- 4. 热力塌落：超过休止角的高差向 8 邻域平均分摊。
    talus_tan = math_node(tree, "TANGENT",
                          math_radians(tree, group_input.outputs["Talus Angle"],
                                       (3940, 800)).outputs[0],
                          location=(4120, 800))
    excess_total = None
    for index, (nx, ny) in enumerate(neighbours):
        length = math.hypot(nx, ny) * cell
        sampled, _ = sample_offset(nx * cell, ny * cell,
                                   (3940, 600 - index * 200))
        diff = math_node(tree, "SUBTRACT", cut.outputs[0], sampled,
                         location=(4120, 600 - index * 200))
        allowed = math_node(tree, "MULTIPLY", talus_tan.outputs[0], length,
                            location=(4300, 600 - index * 200))
        over = math_node(tree, "SUBTRACT", diff.outputs[0], allowed.outputs[0],
                         location=(4480, 600 - index * 200))
        positive = math_node(tree, "MAXIMUM", over.outputs[0], 0.0,
                             location=(4660, 600 - index * 200))
        if excess_total is None:
            excess_total = positive.outputs[0]
        else:
            excess_total = math_node(tree, "MAXIMUM", excess_total,
                                     positive.outputs[0],
                                     location=(4840, 600 - index * 200)).outputs[0]
    collapse = math_node(tree, "MULTIPLY", excess_total,
                         group_input.outputs["Thermal Factor"], location=(5020, 900))
    collapsed = math_node(tree, "SUBTRACT", cut.outputs[0],
                          collapse.outputs[0], location=(5200, 1040))

    store_final = tree.nodes.new("GeometryNodeStoreNamedAttribute")
    store_final.location = (5380, 1040)
    store_final.data_type = "FLOAT"
    store_final.domain = "POINT"
    store_final.inputs["Name"].default_value = "height"
    link(tree, store_flow_out.outputs["Geometry"], store_final.inputs["Geometry"])
    link(tree, collapsed.outputs[0], store_final.inputs["Value"])

    final_height = named_attribute(tree, "height", (5560, 1040))
    return store_final.outputs["Geometry"], final_height, grown.outputs[0]


def math_radians(tree, degrees_socket, location):
    return math_node(tree, "RADIANS", degrees_socket, location=location)

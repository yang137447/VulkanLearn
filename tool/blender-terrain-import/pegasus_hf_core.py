"""VulkanLearn 地形节点组库 —— 基础框架。

## 为什么要有这一层

Houdini 的地形不是一个大节点，而是一组**单算子树（heightfield_noise /
_distort / _remap / _erode ...）**。Pegasus 的整条地形链就是把它们串起来，
所以复刻时也必须按同样的粒度拆分，否则每个新场景都要重写一大坨节点。

本模块提供建组的统一框架：

* :func:`new_group` —— 建一个带标准输入输出的 Geometry Node Group
* :func:`add_params` —— 声明参数，同时生成 ``NodeGroupInput`` 的连接
* :func:`finish_group` —— 把几何接到输出，并校验没有悬空输入
* :func:`heightfield_io` —— 统一的"读本地形层 / 写回本地形层"封装

## 命名与接口规范（强制）

1. 组名一律 ``VL_HF_<算子>``，与 Houdini 节点名一一对应：
   ``VL_HF_Noise`` ↔ ``heightfield_noise``。
2. 每个组**只有一对几何端口**：输入 ``Geometry``、输出 ``Geometry``；
   地形数据一律放在几何的点属性里，属性名固定为
   ``height`` / ``water`` / ``debris`` / ``sediment`` / ``bedrock`` / ``flow``。
3. 参数以 Houdini 参数名为准，用 CamelCase 转写并保留语义：
   ``elementsize`` → ``Element Size``，``bankangle`` → ``Bank Angle``。
4. 组内不允许引用组外节点；子组只能通过嵌套 ``Group`` 节点使用。
5. 每个组的 ``description`` 必须写清楚它对应哪个 Houdini 节点。
"""

from __future__ import annotations

import bpy

# 地形数据层：与 Houdini heightfield 的 layer 绑定一致
TERRAIN_LAYERS = ("height", "water", "debris", "sediment", "bedrock", "flow")

# 组名前缀
GROUP_PREFIX = "VL_HF_"


def group_name(operator: str) -> str:
    """按规范生成组名：``Operator`` → ``VL_HF_Operator``。"""
    return f"{GROUP_PREFIX}{operator}"


def link(tree, source, target) -> None:
    """连线；参数可传节点（自动取首个输出/输入）。"""
    if hasattr(source, "outputs") and not hasattr(source, "is_output"):
        source = source.outputs[0]
    if hasattr(target, "inputs") and not hasattr(target, "is_output"):
        target = target.inputs[0]
    if source is None or target is None:
        raise TypeError("link() got None socket")
    tree.links.new(source, target)


def math_node(tree, operation: str, a=None, b=None, c=None, location=(0, 0)):
    """标量数学节点。

    移植代码里常见 ``math_node(tree, "ADD", x, y, (100, 200))`` 的写法；
    只有一个操作数时位置元组还可能落进 ``b`` 槽。两种形态都在这里归一化。
    """
    if c is not None:
        location = _as_location(c)
    elif _is_location(b):
        location, b = _as_location(b), None
    elif _is_location(a):
        location, a = _as_location(a), None
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


def vector_math(tree, operation: str, a=None, b=None, c=None, location=(0, 0)):
    if c is not None:
        location = _as_location(c)
    elif _is_location(b):
        location, b = _as_location(b), None
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
              location=(0, 0), clamp: bool = True,
              interpolation: str = "LINEAR"):
    node = tree.nodes.new("ShaderNodeMapRange")
    node.location = _as_location(location)
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
    node.location = _as_location(location)
    for index, value in enumerate((x, y, z)):
        if value is None:
            continue
        if isinstance(value, (int, float)):
            node.inputs[index].default_value = float(value)
        else:
            link(tree, value, node.inputs[index])
    return node


def switch_float(tree, condition, false_value, true_value, location=(0, 0)):
    node = tree.nodes.new("GeometryNodeSwitch")
    node.location = _as_location(location)
    node.input_type = "FLOAT"
    link(tree, condition, node.inputs["Switch"])
    for socket_name, value in (("False", false_value), ("True", true_value)):
        socket = node.inputs[socket_name]
        if isinstance(value, (int, float)):
            socket.default_value = float(value)
        else:
            link(tree, value, socket)
    return node.outputs["Output"]


def _is_location(value) -> bool:
    return (isinstance(value, tuple) and len(value) == 2
            and all(isinstance(item, (int, float)) for item in value))


def _as_location(value):
    if _is_location(value):
        return (int(value[0]), int(value[1]))
    return value


# ---------------------------------------------------------------- 层读写


def read_layer(tree, name: str, location=(0, 0)):
    """读地形层属性。"""
    node = tree.nodes.new("GeometryNodeInputNamedAttribute")
    node.data_type = "FLOAT"
    node.location = location
    node.inputs["Name"].default_value = name
    return node.outputs["Attribute"]


def write_layer(tree, geometry, name: str, value, location=(0, 0)):
    """写地形层属性（FLOAT / POINT）。

    socket 按"名字 + 类型"定位：Blender 5.x 的 Store Named Attribute 同时
    暴露可见槽与隐式字段槽，只按类型取会命中错误的那一个。
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
    target = pick("Value", "VALUE")
    if value is not None:
        link(tree, value, target)
    else:
        target.default_value = 0.0
    return node


def set_position(tree, geometry, offset, location=(0, 0)):
    """相对位移。``Position`` 是绝对赋值、``Offset`` 才是偏移。"""
    node = tree.nodes.new("GeometryNodeSetPosition")
    node.location = _as_location(location)
    link(tree, geometry, node.inputs["Geometry"])
    if offset is not None:
        for socket in node.inputs:
            if socket.name == "Offset" and socket.type == "VECTOR":
                link(tree, offset, socket)
                break
    return node


# ---------------------------------------------------------------- 建组框架


def new_group(operator: str, description: str):
    """新建一个符合规范的地形算子组。"""
    name = group_name(operator)
    if name in bpy.data.node_groups:
        bpy.data.node_groups.remove(bpy.data.node_groups[name])
    tree = bpy.data.node_groups.new(name, "GeometryNodeTree")
    tree.description = description
    tree.interface.new_socket(name="Geometry", in_out="INPUT",
                              socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT",
                              socket_type="NodeSocketGeometry")
    tree.use_fake_user = True
    return tree


def add_params(tree, specs):
    """声明参数并返回 ``NodeGroupInput``。

    ``specs`` = [(名字, 类型, 默认值, 最小值, 最大值, 说明)]。
    """
    for name, socket_type, default, minimum, maximum, description in specs:
        socket = tree.interface.new_socket(
            name=name, in_out="INPUT", socket_type=socket_type)
        socket.default_value = default
        if minimum is not None:
            socket.min_value = minimum
        if maximum is not None:
            socket.max_value = maximum
        socket.description = description
    group_input = tree.nodes.new("NodeGroupInput")
    group_input.location = (-2400, -1200)
    return group_input


def params(group_input) -> dict:
    """参数 socket 的便捷字典（跳过 Geometry 端口）。"""
    return {socket.name: socket for socket in group_input.outputs
            if socket.type != "GEOMETRY"}


def finish_group(tree, group_input, geometry, location=(2400, 0)):
    """把几何接到组输出，并做基本校验。"""
    group_output = tree.nodes.new("NodeGroupOutput")
    group_output.location = _as_location(location)
    link(tree, geometry, group_output.inputs["Geometry"])
    return group_output

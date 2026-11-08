"""验证地形节点组库：把组串起来跑一遍，确认能真正求值。

这个脚本是节点库的**回归测试**：每次改库都跑它，任何组接错都会在这里暴露。
"""

from __future__ import annotations

import sys
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

for _stale in [m for m in list(sys.modules)
               if m.startswith(("pegasus_hf_core", "pegasus_hf_nodes"))]:
    del sys.modules[_stale]

import pegasus_hf_erode as erode_nodes  # noqa: E402
import pegasus_hf_layers as layer_nodes  # noqa: E402
import pegasus_hf_nodes as nodes  # noqa: E402
import pegasus_hf_presets as presets  # noqa: E402


def instance(tree, group, location=(0, 0), params=None):
    """在 tree 里放一个 group 实例，并设置参数。"""
    node = tree.nodes.new("GeometryNodeGroup")
    node.node_tree = group
    node.location = location
    if params:
        for socket in node.inputs:
            if socket.name in params:
                socket.default_value = params[socket.name]
    return node


def attribute(data, name: str) -> np.ndarray | None:
    ref = data.attributes.get(name)
    if ref is None:
        return None
    values = np.zeros(len(data.vertices), dtype=np.float32)
    ref.data.foreach_get("value", values)
    return values


def evaluate(tree, label: str):
    mesh = bpy.data.meshes.new(f"test_{label}")
    obj = bpy.data.objects.new(f"obj_{label}", mesh)
    bpy.context.collection.objects.link(obj)
    modifier = obj.modifiers.new("gn", "NODES")
    modifier.node_group = tree
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    data = evaluated.data
    count = len(data.vertices)
    print(f"TEST {label}: verts={count}")
    if count == 0:
        raise AssertionError(f"{label}: empty geometry")
    for layer in ("height", "water", "flow"):
        values = attribute(data, layer)
        if values is None:
            raise AssertionError(f"{label}.{layer}: missing attribute")
        if not np.isfinite(values).all():
            raise AssertionError(f"{label}.{layer}: non-finite values")
        print(f"TEST {label}.{layer}: "
              f"min={values.min():.5f} max={values.max():.5f} "
              f"mean={values.mean():.5f}")
    return data


def evaluate_layers(tree, label: str, layers) -> None:
    """求值并检查指定层的范围。"""
    mesh = bpy.data.meshes.new(f"t_{label}")
    obj = bpy.data.objects.new(f"o_{label}", mesh)
    bpy.context.collection.objects.link(obj)
    modifier = obj.modifiers.new("gn", "NODES")
    modifier.node_group = tree
    bpy.context.view_layer.update()
    data = obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
    print(f"TEST {label}: verts={len(data.vertices)}")
    for layer in layers:
        values = attribute(data, layer)
        if values is None:
            print(f"TEST {label}.{layer}: MISSING")
            continue
        print(f"TEST {label}.{layer}: min={values.min():.5f} "
              f"max={values.max():.5f} nonzero={int((values != 0).sum())}")


def main() -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    base = (list(nodes.build_all()) + list(erode_nodes.build_all())
            + list(layer_nodes.build_all()))
    groups = {group.name: group for group in base}
    for group in presets.build_all():
        groups[group.name] = group

    tree = bpy.data.node_groups.new("VL_HF_TestChain", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT",
                              socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT",
                              socket_type="NodeSocketGeometry")
    output = tree.nodes.new("NodeGroupOutput")
    output.location = (2000, 0)

    heightfield = instance(tree, groups["VL_HF_Heightfield"], (-1800, 0),
                           {"Size": 1000.0, "Samples": 64})
    noise = instance(tree, groups["VL_HF_Noise"], (-1300, 0),
                     {"Size": 1000.0, "Amplitude": 500.0,
                      "Element Size": 500.0, "Octaves": 8.0,
                      "Lacunarity": 1.841, "Height Scale": 500.0})
    distort = instance(tree, groups["VL_HF_Distort"], (-800, 0),
                       {"Size": 1000.0, "Amplitude": 140.0,
                        "Element Size": 800.0})
    # remap 的 From 区间要包住噪声的实际输出范围（height 层是 ±1 归一化）
    remap = instance(tree, groups["VL_HF_Remap"], (-300, 0),
                     {"Layer": "height", "From Min": -1.0, "From Max": 1.0,
                      "To Min": -1.0, "To Max": 1.0})
    resample = instance(tree, groups["VL_HF_Resample"], (200, 0),
                        {"Size": 1000.0, "Samples": 128})
    blur = instance(tree, groups["VL_HF_Blur"], (700, 0),
                    {"Layer": "height", "Cell Size": 7.87, "Radius": 2.0,
                     "Strength": 0.5})

    chain = (heightfield, noise, distort, remap, resample, blur)
    for index in range(len(chain) - 1):
        tree.links.new(chain[index].outputs[0], chain[index + 1].inputs[0])
    tree.links.new(chain[-1].outputs[0], output.inputs["Geometry"])

    evaluate(tree, "chain")

    # 单独检查 Noise 组：不经过 remap
    solo = bpy.data.node_groups.new("VL_HF_TestNoise", "GeometryNodeTree")
    solo.interface.new_socket(name="Geometry", in_out="INPUT",
                              socket_type="NodeSocketGeometry")
    solo.interface.new_socket(name="Geometry", in_out="OUTPUT",
                              socket_type="NodeSocketGeometry")
    solo_out = solo.nodes.new("NodeGroupOutput")
    hf2 = instance(solo, groups["VL_HF_Heightfield"], (-1400, 0),
                   {"Size": 1000.0, "Samples": 64})
    nz2 = instance(solo, groups["VL_HF_Noise"], (-900, 0),
                   {"Size": 1000.0, "Amplitude": 500.0,
                    "Element Size": 500.0, "Octaves": 8.0,
                    "Lacunarity": 1.841, "Height Scale": 500.0})
    solo.links.new(hf2.outputs[0], nz2.inputs[0])
    solo.links.new(nz2.outputs[0], solo_out.inputs["Geometry"])
    evaluate(solo, "noise_only")

    # erosion 组：heightfield → noise → Erode
    er = bpy.data.node_groups.new("VL_HF_TestErode", "GeometryNodeTree")
    er.interface.new_socket(name="Geometry", in_out="INPUT",
                            socket_type="NodeSocketGeometry")
    er.interface.new_socket(name="Geometry", in_out="OUTPUT",
                            socket_type="NodeSocketGeometry")
    er_out = er.nodes.new("NodeGroupOutput")
    hf3 = instance(er, groups["VL_HF_Heightfield"], (-1800, 0),
                   {"Size": 1000.0, "Samples": 64})
    nz3 = instance(er, groups["VL_HF_Noise"], (-1300, 0),
                   {"Size": 1000.0, "Amplitude": 500.0,
                    "Element Size": 500.0, "Octaves": 8.0,
                    "Lacunarity": 1.841, "Height Scale": 500.0})
    erode_group = groups["VL_HF_Erode"]
    ed = er.nodes.new("GeometryNodeGroup")
    ed.node_tree = erode_group
    ed.location = (-800, 0)
    for socket in ed.inputs:
        if socket.name == "Cell Size":
            socket.default_value = 15.87
        elif socket.name == "Water Spread Iterations":
            socket.default_value = 40
    er.links.new(hf3.outputs[0], nz3.inputs[0])
    er.links.new(nz3.outputs[0], ed.inputs[0])
    er.links.new(ed.outputs[0], er_out.inputs["Geometry"])
    evaluate(er, "erode_group")

    # preset：一行调取完整 Pegasus 地形链
    pre = bpy.data.node_groups.new("VL_HF_TestPreset", "GeometryNodeTree")
    pre.interface.new_socket(name="Geometry", in_out="INPUT",
                             socket_type="NodeSocketGeometry")
    pre.interface.new_socket(name="Geometry", in_out="OUTPUT",
                             socket_type="NodeSocketGeometry")
    pre_out = pre.nodes.new("NodeGroupOutput")
    preset_node = pre.nodes.new("GeometryNodeGroup")
    preset_node.node_tree = groups["VL_HF_PegasusTerrain"]
    preset_node.location = (0, 0)
    for socket in preset_node.inputs:
        if socket.name == "Samples":
            socket.default_value = 64
        elif socket.name == "Work Samples":
            # 测试场景只有 64 格；Work Samples 必须跟着走，否则会先升采样到
            # 512 再降回来，既慢又让 Grid Cells 与真实网格不符。
            socket.default_value = 64
    pre.links.new(preset_node.outputs[0], pre_out.inputs["Geometry"])
    preset_data = evaluate(pre, "preset")
    # 汇水面积是"上游格数"，物理上不可能超过总格数；超过说明累积出现了
    # 环流/未归一化（曾经溢出成 inf 并把整条链变成 NaN）。
    preset_flow = attribute(preset_data, "flow")
    if float(preset_flow.max()) > len(preset_data.vertices) + 1e-3:
        raise AssertionError(
            f"preset.flow exceeds cell count: {preset_flow.max()} > "
            f"{len(preset_data.vertices)}")

    # 层操作与掩膜组：逐个接在 noise 之后验证
    for name, params, layers in (
        ("VL_HF_LayerCopy", {"Source Layer": "height",
                              "Destination Layer": "bedrock"}, ("bedrock",)),
        ("VL_HF_Layer", {"Layer": "water", "Value": 0.25}, ("water",)),
        ("VL_HF_LayerClear", {"Layer": "water", "Value": 0.0}, ("water",)),
        ("VL_HF_Crop", {"Layer": "height", "Threshold": 0.0,
                         "Keep Below": False}, ("height",)),
        ("VL_HF_MaskByFeature", {"Output Layer": "mask", "Use Slope": True,
                                  "Min Slope": 5.0, "Max Slope": 80.0,
                                  "Cell Size": 15.87}, ("mask",)),
        ("VL_HF_Blur", {"Layer": "height", "Cell Size": 15.87,
                         "Radius": 2.0, "Strength": 0.5,
                         "Method": "shrink"}, ("height",)),
    ):
        single = bpy.data.node_groups.new(f"VL_HF_Test_{name}", "GeometryNodeTree")
        single.interface.new_socket(name="Geometry", in_out="INPUT",
                                    socket_type="NodeSocketGeometry")
        single.interface.new_socket(name="Geometry", in_out="OUTPUT",
                                    socket_type="NodeSocketGeometry")
        single_out = single.nodes.new("NodeGroupOutput")
        hf4 = instance(single, groups["VL_HF_Heightfield"], (-1800, 0),
                       {"Size": 1000.0, "Samples": 32})
        nz4 = instance(single, groups["VL_HF_Noise"], (-1300, 0),
                       {"Size": 1000.0, "Amplitude": 500.0,
                        "Element Size": 500.0, "Octaves": 8.0,
                        "Lacunarity": 1.841, "Height Scale": 500.0})
        op = instance(single, groups[name], (-800, 0), params)
        single.links.new(hf4.outputs[0], nz4.inputs[0])
        single.links.new(nz4.outputs[0], op.inputs[0])
        single.links.new(op.outputs[0], single_out.inputs["Geometry"])
        evaluate_layers(single, name, layers)


main()

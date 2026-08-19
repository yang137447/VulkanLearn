import json
import sys
from pathlib import Path

import bpy


def socket_value(socket):
    if socket.is_linked:
        link = socket.links[0]
        return {
            "linked": True,
            "fromNode": link.from_node.name,
            "fromSocket": link.from_socket.name,
        }

    if not hasattr(socket, "default_value"):
        return {"linked": False, "value": None}
    value = socket.default_value
    if hasattr(value, "to_list"):
        value = value.to_list()
    elif isinstance(value, (list, tuple)):
        value = list(value)
    elif not isinstance(value, (str, bytes, int, float, bool)):
        try:
            value = [float(item) for item in value]
        except (TypeError, ValueError):
            value = str(value)
    elif hasattr(value, "__float__") and not isinstance(value, bool):
        value = float(value)
    return {"linked": False, "value": value}


def describe_node(node):
    result = {
        "name": node.name,
        "type": node.bl_idname,
        "label": node.label,
        "operation": getattr(node, "operation", None),
        "inputs": {},
        "outputs": {},
    }
    for socket in node.inputs:
        if socket.is_linked or hasattr(socket, "default_value"):
            result["inputs"][socket.name] = socket_value(socket)
    for socket in node.outputs:
        if hasattr(socket, "default_value"):
            result["outputs"][socket.name] = socket_value(socket)

    color_ramp = getattr(node, "color_ramp", None)
    if color_ramp is not None:
        result["colorRamp"] = {
            "interpolation": color_ramp.interpolation,
            "elements": [
                {
                    "position": element.position,
                    "color": list(element.color),
                }
                for element in color_ramp.elements
            ],
        }

    image = getattr(node, "image", None)
    if image is not None:
        result["image"] = {
            "name": image.name,
            "filepath": image.filepath,
            "filepathResolved": image.filepath_from_user(),
        }
    return result


def describe_material(name):
    material = bpy.data.materials.get(name)
    if material is None:
        return {"name": name, "missing": True}

    result = {
        "name": material.name,
        "useNodes": material.use_nodes,
        "surfaceRenderMethod": getattr(material, "surface_render_method", None),
        "nodes": [],
        "links": [],
    }
    if not material.use_nodes:
        return result

    result["nodes"] = [describe_node(node) for node in material.node_tree.nodes]
    result["links"] = [
        {
            "fromNode": link.from_node.name,
            "fromSocket": link.from_socket.name,
            "toNode": link.to_node.name,
            "toSocket": link.to_socket.name,
        }
        for link in material.node_tree.links
    ]
    return result


def describe_scene():
    scene = bpy.context.scene
    view = scene.view_settings
    world = scene.world
    result = {
        "sourceBlend": bpy.data.filepath,
        "scene": scene.name,
        "render": {
            "engine": scene.render.engine,
            "resolution": [scene.render.resolution_x, scene.render.resolution_y],
            "percentage": scene.render.resolution_percentage,
            "filmTransparent": scene.render.film_transparent,
            "viewTransform": view.view_transform,
            "look": view.look,
            "exposure": view.exposure,
            "gamma": view.gamma,
        },
        "world": {
            "name": world.name if world else None,
            "useNodes": world.use_nodes if world else False,
        },
        "cameras": [],
        "lights": [],
    }
    if world and world.use_nodes:
        result["world"]["nodes"] = [describe_node(node) for node in world.node_tree.nodes]

    for obj in scene.objects:
        if obj.type == "CAMERA":
            result["cameras"].append(
                {
                    "name": obj.name,
                    "location": list(obj.location),
                    "rotation": list(obj.rotation_euler),
                    "lens": obj.data.lens,
                    "sensorWidth": obj.data.sensor_width,
                }
            )
        elif obj.type == "LIGHT":
            result["lights"].append(
                {
                    "name": obj.name,
                    "type": obj.data.type,
                    "location": list(obj.location),
                    "rotation": list(obj.rotation_euler),
                    "energy": obj.data.energy,
                    "color": list(obj.data.color),
                    "size": getattr(obj.data, "size", None),
                }
            )
    return result


def main():
    arguments = sys.argv[sys.argv.index("--") + 1 :]
    if len(arguments) != 1:
        raise RuntimeError(
            "Usage: blender --background file.blend --python "
            "inspect_car_material_look.py -- output.json"
        )

    output_path = Path(arguments[0])
    report = describe_scene()
    report["materials"] = [
        describe_material("M_Carpaint"),
        describe_material("M_Inner"),
        describe_material("M_Light"),
        describe_material("M_Grid"),
        describe_material("M_Out"),
        describe_material("M_Logo"),
        describe_material("M_LicensePlate"),
        describe_material("M_Glass_Light"),
    ]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2, default=str),
        encoding="utf-8",
    )
    print(
        "CAR_LOOK_REPORT "
        f"materials={len(report['materials'])} "
        f"cameras={len(report['cameras'])} "
        f"lights={len(report['lights'])} "
        f"output={output_path}"
    )


if __name__ == "__main__":
    main()

import argparse
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--wheel", default="FL")
    parser.add_argument(
        "--flip-v",
        action="store_true",
    )
    parser.add_argument(
        "--base-color",
        action="store_true",
    )
    script_arguments = []
    if "--" in sys.argv:
        script_arguments = sys.argv[sys.argv.index("--") + 1 :]
    return parser.parse_args(script_arguments)


def point_camera_at(camera, target):
    direction = target - camera.location
    camera.rotation_euler = direction.to_track_quat(
        "-Z",
        "Y",
    ).to_euler()


def replace_material_with_base_color(material):
    if material is None or not material.use_nodes:
        return material
    principled = next(
        (
            node
            for node in material.node_tree.nodes
            if node.type == "BSDF_PRINCIPLED"
        ),
        None,
    )
    if principled is None:
        return material
    base_color = principled.inputs.get("Base Color")
    if base_color is None or not base_color.is_linked:
        return material
    image_node = base_color.links[0].from_node
    if image_node.type != "TEX_IMAGE":
        return material

    result = material.copy()
    result.use_nodes = True
    nodes = result.node_tree.nodes
    links = result.node_tree.links
    nodes.clear()
    texture = nodes.new("ShaderNodeTexImage")
    texture.image = image_node.image
    emission = nodes.new("ShaderNodeEmission")
    output = nodes.new("ShaderNodeOutputMaterial")
    links.new(texture.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    return result


def calculate_world_bounds(objects):
    minimum = Vector(
        (
            float("inf"),
            float("inf"),
            float("inf"),
        )
    )
    maximum = Vector(
        (
            float("-inf"),
            float("-inf"),
            float("-inf"),
        )
    )
    for source_object in objects:
        for corner in source_object.bound_box:
            world_corner = (
                source_object.matrix_world
                @ Vector(corner)
            )
            for axis in range(3):
                minimum[axis] = min(
                    minimum[axis],
                    world_corner[axis],
                )
                maximum[axis] = max(
                    maximum[axis],
                    world_corner[axis],
                )
    return minimum, maximum


def main():
    arguments = parse_arguments()
    output_path = Path(arguments.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    wheel_object = bpy.data.objects.get(arguments.wheel)
    if wheel_object is None or wheel_object.type != "MESH":
        raise RuntimeError(
            "Missing Blender wheel mesh: " + arguments.wheel
        )
    wheel_material_names = {
        slot.material.name
        for slot in wheel_object.material_slots
        if slot.material is not None
    }
    expected_material_names = {
        "M_Wheel_Brake",
        "M_Wheel_Hub",
        "M_Wheel_Tread",
    }
    missing_material_names = (
        expected_material_names - wheel_material_names
    )
    if missing_material_names:
        raise RuntimeError(
            f"Wheel '{arguments.wheel}' is missing materials: "
            f"{sorted(missing_material_names)}"
        )
    brake_objects = [wheel_object]

    visible_names = {
        source_object.name
        for source_object in brake_objects
    }
    for source_object in bpy.context.scene.objects:
        if source_object.type == "MESH":
            source_object.hide_render = (
                source_object.name not in visible_names
            )

    if arguments.flip_v:
        for source_object in brake_objects:
            uv_layer = source_object.data.uv_layers.active
            if uv_layer is None:
                continue
            for loop in uv_layer.data:
                loop.uv.y = 1.0 - loop.uv.y

    if arguments.base_color:
        for source_object in brake_objects:
            for material_slot in source_object.material_slots:
                source_material = material_slot.material
                if (
                    source_material is not None
                    and source_material.name
                    in expected_material_names
                ):
                    material_slot.material = (
                        replace_material_with_base_color(
                            source_material
                        )
                    )

    minimum, maximum = calculate_world_bounds(brake_objects)
    center = (minimum + maximum) * 0.5
    size = maximum - minimum
    radius = max(size.y, size.z) * 0.7

    camera_data = bpy.data.cameras.new("BrakeReferenceCamera")
    camera = bpy.data.objects.new(
        "BrakeReferenceCamera",
        camera_data,
    )
    bpy.context.scene.collection.objects.link(camera)
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = max(radius * 2.4, 0.5)
    outside_sign = 1.0 if center.x >= 0.0 else -1.0
    camera.location = center + Vector(
        (outside_sign * 3.0, 0.0, 0.0)
    )
    point_camera_at(camera, center)
    bpy.context.scene.camera = camera

    key_data = bpy.data.lights.new(
        "BrakeReferenceKey",
        "AREA",
    )
    key_data.energy = 900.0
    key_data.shape = "DISK"
    key_data.size = 4.0
    key = bpy.data.objects.new(
        "BrakeReferenceKey",
        key_data,
    )
    bpy.context.scene.collection.objects.link(key)
    key.location = center + Vector(
        (outside_sign * 2.0, -2.0, 2.5)
    )
    point_camera_at(key, center)

    fill_data = bpy.data.lights.new(
        "BrakeReferenceFill",
        "AREA",
    )
    fill_data.energy = 500.0
    fill_data.size = 3.0
    fill = bpy.data.objects.new(
        "BrakeReferenceFill",
        fill_data,
    )
    bpy.context.scene.collection.objects.link(fill)
    fill.location = center + Vector(
        (outside_sign * 1.5, 2.0, 0.5)
    )
    point_camera_at(fill, center)

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 640
    scene.render.resolution_y = 640
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.render.filepath = str(output_path)
    scene.world.use_nodes = True
    background = scene.world.node_tree.nodes.get(
        "Background"
    )
    if background is not None:
        background.inputs["Color"].default_value = (
            0.08,
            0.08,
            0.08,
            1.0,
        )
        background.inputs["Strength"].default_value = 0.2
    scene.view_settings.look = "Medium High Contrast"
    bpy.ops.render.render(write_still=True)
    print("BRAKE_REFERENCE_RENDER=" + str(output_path))


if __name__ == "__main__":
    main()

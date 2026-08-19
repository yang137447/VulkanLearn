import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def point_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def remove_scene_object_types(*object_types):
    for obj in list(bpy.context.scene.objects):
        if obj.type in object_types:
            bpy.data.objects.remove(obj, do_unlink=True)


def create_camera():
    camera_data = bpy.data.cameras.new("CarReferenceCamera")
    camera = bpy.data.objects.new("CarReferenceCamera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = Vector((7.4, -8.584, 4.884))
    camera_data.angle = math.radians(24.0)
    point_at(camera, (0.0, 0.0, 0.58))
    bpy.context.scene.camera = camera


def create_area_light(name, location, energy, size, color):
    light_data = bpy.data.lights.new(name, type="AREA")
    light_data.energy = energy
    light_data.shape = "DISK"
    light_data.size = size
    light_data.color = color
    light = bpy.data.objects.new(name, light_data)
    bpy.context.scene.collection.objects.link(light)
    light.location = Vector(location)
    point_at(light, (0.0, 0.0, 0.55))
    return light


def configure_world():
    world = bpy.context.scene.world
    if world is None:
        world = bpy.data.worlds.new("CarReferenceWorld")
        bpy.context.scene.world = world
    world.use_nodes = True
    nodes = world.node_tree.nodes
    nodes.clear()
    output = nodes.new("ShaderNodeOutputWorld")
    background = nodes.new("ShaderNodeBackground")
    background.inputs["Color"].default_value = (0.32, 0.36, 0.44, 1.0)
    background.inputs["Strength"].default_value = 0.42
    world.node_tree.links.new(background.outputs["Background"], output.inputs["Surface"])


def main():
    arguments = sys.argv[sys.argv.index("--") + 1 :]
    if len(arguments) != 1:
        raise RuntimeError(
            "Usage: blender --background Car_02.blend --python "
            "render_car_blender_reference.py -- output.png"
        )

    output_path = Path(arguments[0])
    output_path.parent.mkdir(parents=True, exist_ok=True)

    remove_scene_object_types("CAMERA", "LIGHT")
    create_camera()
    create_area_light(
        "CarReferenceKey",
        (4.5, -4.0, 8.0),
        1350.0,
        4.0,
        (1.0, 0.92, 0.82),
    )
    create_area_light(
        "CarReferenceFill",
        (-4.0, -2.5, 4.5),
        700.0,
        5.0,
        (0.55, 0.72, 1.0),
    )
    create_area_light(
        "CarReferenceRim",
        (-2.0, 4.0, 5.5),
        900.0,
        3.0,
        (0.85, 0.92, 1.0),
    )
    configure_world()

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 960
    scene.render.resolution_y = 540
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.render.filepath = str(output_path)
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "Medium High Contrast"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    bpy.ops.render.render(write_still=True)
    print(f"CAR_BLENDER_REFERENCE output={output_path}")


if __name__ == "__main__":
    main()

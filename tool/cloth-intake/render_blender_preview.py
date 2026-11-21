import sys
from pathlib import Path

import bpy
from mathutils import Vector


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    if len(argv) != 1:
        raise SystemExit(
            "usage: blender --background <blend> --python "
            "render_blender_preview.py -- <output.png>"
        )
    return Path(argv[0])


def main():
    output = parse_args()
    points = []
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH" or obj.hide_render:
            continue
        points.extend(obj.matrix_world @ Vector(corner) for corner in obj.bound_box)
    if not points:
        raise SystemExit("no visible mesh objects")
    minimum = Vector(
        (
            min(point.x for point in points),
            min(point.y for point in points),
            min(point.z for point in points),
        )
    )
    maximum = Vector(
        (
            max(point.x for point in points),
            max(point.y for point in points),
            max(point.z for point in points),
        )
    )
    center = (minimum + maximum) * 0.5
    width = maximum.x - minimum.x
    height = maximum.z - minimum.z

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
    scene.display.shading.show_shadows = True
    scene.display.shading.show_cavity = True
    scene.render.resolution_x = 1600
    scene.render.resolution_y = 1000
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = str(output)

    camera_data = bpy.data.cameras.new("PreviewCamera")
    camera_data.type = "ORTHO"
    aspect = scene.render.resolution_x / scene.render.resolution_y
    camera_data.ortho_scale = max(width / aspect, height) * 1.35
    camera = bpy.data.objects.new("PreviewCamera", camera_data)
    scene.collection.objects.link(camera)
    camera.location = (center.x, minimum.y - 10.0, center.z)
    camera.rotation_euler = (1.57079632679, 0.0, 0.0)
    scene.camera = camera
    bpy.ops.render.render(write_still=True)
    print(f"rendered {output}")


if __name__ == "__main__":
    main()

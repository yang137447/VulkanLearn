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
            "render_outfit_thumbnails.py -- <output-dir>"
        )
    return Path(argv[0])


def mesh_bounds(objects):
    points = []
    for obj in objects:
        if obj.type != "MESH":
            continue
        points.extend(obj.matrix_world @ Vector(corner) for corner in obj.bound_box)
    if not points:
        return None
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
    return minimum, maximum


def main():
    output_dir = parse_args()
    output_dir.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    root = bpy.data.collections.get("3d66 Cloth Showcase")
    if root is None:
        raise SystemExit("missing '3d66 Cloth Showcase' collection")

    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "MATERIAL"
    scene.display.shading.show_shadows = True
    scene.display.shading.show_cavity = True
    scene.render.resolution_x = 512
    scene.render.resolution_y = 512
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"

    camera_data = bpy.data.cameras.new("ThumbnailCamera")
    camera_data.type = "ORTHO"
    camera = bpy.data.objects.new("ThumbnailCamera", camera_data)
    scene.collection.objects.link(camera)
    camera.rotation_euler = (1.57079632679, 0.0, 0.0)
    scene.camera = camera

    for collection in sorted(root.children, key=lambda item: item.name):
        meshes = [obj for obj in collection.objects if obj.type == "MESH"]
        bounds = mesh_bounds(meshes)
        if bounds is None:
            continue
        minimum, maximum = bounds
        center = (minimum + maximum) * 0.5
        width = maximum.x - minimum.x
        height = maximum.z - minimum.z
        camera.location = (center.x, minimum.y - 10.0, center.z)
        camera_data.ortho_scale = max(width, height) * 1.25

        for obj in scene.objects:
            if obj.type in {"MESH", "EMPTY"}:
                obj.hide_render = True
        for obj in meshes:
            obj.hide_render = False

        scene.render.filepath = str(output_dir / f"{collection.name}.png")
        bpy.ops.render.render(write_still=True)
        print(f"rendered {collection.name}")


if __name__ == "__main__":
    main()

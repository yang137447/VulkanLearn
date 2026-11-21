import re
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
            "organize_blender_collections.py -- <output.blend>"
        )
    return Path(argv[0])


def group_number(name):
    match = re.search(r"(\d+)$", name)
    return int(match.group(1)) if match else 1 << 30


def move_to_collection(objects, collection):
    for obj in objects:
        for source in list(obj.users_collection):
            source.objects.unlink(obj)
        collection.objects.link(obj)


def main():
    output_path = parse_args()
    scene = bpy.context.scene
    root = bpy.data.collections.new("3d66 Cloth Showcase")
    scene.collection.children.link(root)

    top_groups = sorted(
        (
            obj
            for obj in scene.objects
            if obj.type == "EMPTY"
            and obj.name.startswith("3d66Group")
            and obj.parent is None
        ),
        key=lambda obj: group_number(obj.name),
    )

    collected = set()
    for index, group in enumerate(top_groups, start=1):
        collection = bpy.data.collections.new(
            f"Cloth_{index:03d}_{group.name}"
        )
        collection["source_group"] = group.name
        root.children.link(collection)
        objects = [group, *group.children_recursive]
        move_to_collection(objects, collection)
        collected.update(objects)

    ungrouped_meshes = [
        obj
        for obj in scene.objects
        if obj.type == "MESH" and obj not in collected
    ]
    if ungrouped_meshes:
        standalone = bpy.data.collections.new("Cloth_000_Standalone")
        standalone["source_group"] = "ungrouped mesh objects"
        root.children.link(standalone)
        move_to_collection(ungrouped_meshes, standalone)
        collected.update(ungrouped_meshes)

    helpers = bpy.data.collections.new("Scene_Helpers")
    scene.collection.children.link(helpers)
    leftover = [
        obj
        for obj in scene.objects
        if obj not in collected
    ]
    move_to_collection(leftover, helpers)

    points = []
    for obj in scene.objects:
        if obj.type != "MESH":
            continue
        points.extend(obj.matrix_world @ Vector(corner) for corner in obj.bound_box)
    if points:
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
        extent = max(maximum - minimum)
    else:
        center = Vector((0.0, 0.0, 0.0))
        extent = 100.0

    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type != "VIEW_3D":
                continue
            for space in area.spaces:
                if space.type == "VIEW_3D":
                    space.clip_start = 0.01
                    space.clip_end = 10000000.0
                    space.region_3d.view_location = center
                    space.region_3d.view_distance = extent * 1.8

    bpy.context.view_layer.update()
    bpy.ops.wm.save_as_mainfile(filepath=str(output_path))
    print(
        f"collections={len(root.children)} outfits={len(top_groups)} "
        f"helpers={len(leftover)} objects={len(scene.objects)}"
    )
    print(f"saved {output_path}")


if __name__ == "__main__":
    main()

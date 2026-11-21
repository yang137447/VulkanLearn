import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    if len(argv) not in (1, 2):
        raise SystemExit(
            "usage: blender --background <blend> --python "
            "scale_blender_outfits.py -- <output.blend> [factor]"
        )
    return Path(argv[0]), float(argv[1]) if len(argv) == 2 else 2.54


def collection_bounds(collection):
    minimum = None
    maximum = None
    for obj in collection.objects:
        if obj.type != "MESH" or len(obj.data.vertices) == 0:
            continue
        coordinates = np.empty(len(obj.data.vertices) * 3, dtype=np.float64)
        obj.data.vertices.foreach_get("co", coordinates)
        coordinates = coordinates.reshape(-1, 3)
        matrix = np.array(obj.matrix_world, dtype=np.float64)
        points = coordinates @ matrix[:3, :3].T + matrix[:3, 3]
        object_minimum = points.min(axis=0)
        object_maximum = points.max(axis=0)
        if minimum is None:
            minimum = object_minimum
            maximum = object_maximum
        else:
            minimum = np.minimum(minimum, object_minimum)
            maximum = np.maximum(maximum, object_maximum)
    return minimum, maximum


def main():
    output_path, factor = parse_args()
    scene = bpy.context.scene
    root_collection = bpy.data.collections.get("3d66 Cloth Showcase")
    if root_collection is None:
        raise SystemExit("missing '3d66 Cloth Showcase' collection")

    global_minimum = None
    global_maximum = None
    for collection in root_collection.children:
        minimum, maximum = collection_bounds(collection)
        if minimum is None:
            continue
        if global_minimum is None:
            global_minimum = minimum
            global_maximum = maximum
        else:
            global_minimum = np.minimum(global_minimum, minimum)
            global_maximum = np.maximum(global_maximum, maximum)
    if global_minimum is None:
        raise SystemExit("no mesh data found")
    pivot = Vector(
        (
            float((global_minimum[0] + global_maximum[0]) * 0.5),
            float((global_minimum[1] + global_maximum[1]) * 0.5),
            float((global_minimum[2] + global_maximum[2]) * 0.5),
        )
    )

    meshes = []
    for collection in root_collection.children:
        root = bpy.data.objects.get(collection.get("root_object", ""))
        if root is None:
            continue
        root.location = pivot + (root.location - pivot) * factor
        for obj in collection.objects:
            if obj.type != "MESH":
                continue
            if obj.data.users > 1:
                obj.data = obj.data.copy()
            obj.location *= factor
            obj.scale *= factor
            meshes.append(obj)

    bpy.ops.object.select_all(action="DESELECT")
    for obj in meshes:
        obj.select_set(True)
    if meshes:
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bpy.context.view_layer.update()

    maximum_error = 0.0
    worst_outfit = ""
    heights = []
    for collection in sorted(root_collection.children, key=lambda item: item.name):
        minimum, maximum = collection_bounds(collection)
        if minimum is None:
            continue
        root = bpy.data.objects.get(collection.get("root_object", ""))
        if root is None:
            continue
        expected = Vector(
            (
                float((minimum[0] + maximum[0]) * 0.5),
                float((minimum[1] + maximum[1]) * 0.5),
                float(minimum[2]),
            )
        )
        actual = root.matrix_world.translation
        error = max(abs(actual.x - expected.x), abs(actual.y - expected.y), abs(actual.z - expected.z))
        if error > maximum_error:
            maximum_error = error
            worst_outfit = collection.name
        root["ground_error"] = error
        heights.append(float(maximum[2] - minimum[2]))

    points = []
    for collection in root_collection.children:
        minimum, maximum = collection_bounds(collection)
        if minimum is not None:
            points.append(minimum)
            points.append(maximum)
    if points:
        minimum = Vector(
            (
                min(point[0] for point in points),
                min(point[1] for point in points),
                min(point[2] for point in points),
            )
        )
        maximum = Vector(
            (
                max(point[0] for point in points),
                max(point[1] for point in points),
                max(point[2] for point in points),
            )
        )
        center = (minimum + maximum) * 0.5
        extent = max(maximum - minimum)
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

    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0
    scene.unit_settings.length_unit = "CENTIMETERS"
    bpy.ops.wm.save_as_mainfile(filepath=str(output_path))
    print(
        f"factor={factor} meshes={len(meshes)} "
        f"height_min={min(heights):.4f} height_max={max(heights):.4f} "
        f"max_root_error={maximum_error:.9f} worst={worst_outfit}"
    )
    print(f"saved {output_path}")


if __name__ == "__main__":
    main()

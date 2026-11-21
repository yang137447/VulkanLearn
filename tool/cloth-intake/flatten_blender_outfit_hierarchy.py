import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    if len(argv) != 1:
        raise SystemExit(
            "usage: blender --background <blend> --python "
            "flatten_blender_outfit_hierarchy.py -- <output.blend>"
        )
    return Path(argv[0])


def mesh_bounds(objects):
    minimum = None
    maximum = None
    for obj in objects:
        if obj.type != "MESH":
            continue
        count = len(obj.data.vertices)
        if count == 0:
            continue
        coordinates = np.empty(count * 3, dtype=np.float64)
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
    if minimum is None:
        return None
    return Vector(minimum), Vector(maximum)


def main():
    output_path = parse_args()
    scene = bpy.context.scene
    root_collection = bpy.data.collections.get("3d66 Cloth Showcase")
    if root_collection is None:
        raise SystemExit("missing '3d66 Cloth Showcase' collection")

    for outfit in sorted(root_collection.children, key=lambda collection: collection.name):
        meshes = [obj for obj in outfit.objects if obj.type == "MESH"]
        if not meshes:
            continue
        bounds = mesh_bounds(meshes)
        if bounds is None:
            continue
        minimum, maximum = bounds
        center = (minimum + maximum) * 0.5
        extent = max(maximum - minimum)
        root_origin = Vector((center.x, center.y, minimum.z))

        root_name = f"{outfit.name}_ROOT"
        root = bpy.data.objects.get(root_name)
        if root is None:
            root = bpy.data.objects.new(root_name, None)
            root.empty_display_type = "PLAIN_AXES"
            root.empty_display_size = 0.2
            root.show_in_front = True
            outfit.objects.link(root)
        root.matrix_world = Matrix.Translation(root_origin)
        root["source_collection"] = outfit.name
        root["source_group"] = outfit.get("source_group", "")
        root["mesh_count"] = len(meshes)

        for obj in meshes:
            world = obj.matrix_world.copy()
            obj.parent = None
            obj.matrix_parent_inverse = Matrix.Identity(4)
            obj.parent = root
            obj.matrix_parent_inverse = Matrix.Identity(4)
            obj.matrix_world = world

        stale_empties = [
            obj
            for obj in list(outfit.objects)
            if obj.type == "EMPTY" and obj != root
        ]
        for obj in stale_empties:
            bpy.data.objects.remove(obj, do_unlink=True)

        outfit["root_object"] = root_name
        outfit["mesh_count"] = len(meshes)

    bpy.context.view_layer.update()
    maximum_error = 0.0
    worst_outfit = ""
    verification_failures = []
    for outfit in sorted(root_collection.children, key=lambda collection: collection.name):
        meshes = [obj for obj in outfit.objects if obj.type == "MESH"]
        root = bpy.data.objects.get(outfit.get("root_object", ""))
        bounds = mesh_bounds(meshes)
        if bounds is None or root is None:
            continue
        minimum, maximum = bounds
        expected = Vector(
            (
                (minimum.x + maximum.x) * 0.5,
                (minimum.y + maximum.y) * 0.5,
                minimum.z,
            )
        )
        actual = root.matrix_world.translation
        error = max(abs(actual.x - expected.x), abs(actual.y - expected.y), abs(actual.z - expected.z))
        if error > maximum_error:
            maximum_error = error
            worst_outfit = outfit.name
        if error > 1.0e-5:
            verification_failures.append((outfit.name, error))
        root["ground_error"] = error

    bpy.ops.wm.save_as_mainfile(filepath=str(output_path))
    print(
        f"outfits={len(root_collection.children)} "
        f"objects={len(scene.objects)} "
        f"meshes={sum(1 for obj in scene.objects if obj.type == 'MESH')}"
    )
    print(f"max_root_error={maximum_error:.9f} worst={worst_outfit}")
    if verification_failures:
        print("root verification failures:")
        for name, error in verification_failures:
            print(f"  {name}: {error:.9f}")
    print(f"saved {output_path}")


if __name__ == "__main__":
    main()

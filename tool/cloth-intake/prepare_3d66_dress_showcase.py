import csv
import math
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
    if len(argv) != 2:
        raise SystemExit(
            "usage: blender --background <blend> --python "
            "prepare_3d66_dress_showcase.py -- <source.blend> <export.fbx>"
        )
    return Path(argv[0]), Path(argv[1])


def mesh_bounds(objects):
    minimum = None
    maximum = None
    for obj in objects:
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
    source_blend, export_fbx = parse_args()
    source_blend = source_blend.resolve()
    export_fbx = export_fbx.resolve()
    collection = bpy.data.collections.get("Cloth_012_3d66Group017")
    if collection is None:
        raise SystemExit("missing Cloth_012_3d66Group017")

    keep_objects = {obj for obj in collection.objects}
    for obj in list(bpy.context.scene.objects):
        if obj not in keep_objects:
            bpy.data.objects.remove(obj, do_unlink=True)
    for other in list(bpy.data.collections):
        if other != collection:
            bpy.data.collections.remove(other)
    if collection.name not in bpy.context.scene.collection.children:
        bpy.context.scene.collection.children.link(collection)

    root = bpy.data.objects.get("Cloth_012_3d66Group017_ROOT")
    if root is not None:
        root.name = "Cloth_3d66_Dress_ROOT"
        root.location = (0.0, 0.0, 0.0)
        root["source_collection"] = "Cloth_012_3d66Group017"
    collection.name = "Cloth_3d66_Dress"
    collection["source_group"] = "3d66Group017"

    bpy.context.view_layer.update()
    source_blend.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(source_blend))

    meshes = [obj for obj in collection.objects if obj.type == "MESH"]
    minimum, maximum = mesh_bounds(meshes)
    offset = Matrix.Translation(
        (
            -float((minimum[0] + maximum[0]) * 0.5),
            -float((minimum[1] + maximum[1]) * 0.5),
            -float(minimum[2]),
        )
    )
    axis = Matrix.Rotation(math.radians(-90.0), 4, "X")

    for obj in meshes:
        if obj.data.users > 1:
            obj.data = obj.data.copy()
        world = obj.matrix_world.copy()
        obj.parent = None
        obj.matrix_parent_inverse = Matrix.Identity(4)
        obj.matrix_world = axis @ offset @ world
    bpy.context.view_layer.update()
    bpy.ops.object.select_all(action="DESELECT")
    for obj in meshes:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for obj in meshes:
        print(
            "OBJECT_MATRIX",
            obj.name,
            tuple(round(v, 4) for v in obj.location),
            tuple(round(v, 4) for v in obj.scale),
        )
        local_points = [vertex.co for vertex in obj.data.vertices]
        if local_points:
            print(
                "LOCAL_BOUNDS",
                obj.name,
                f"y={min(point.y for point in local_points):.4f}",
                f"..{max(point.y for point in local_points):.4f}",
                f"z={min(point.z for point in local_points):.4f}",
                f"..{max(point.z for point in local_points):.4f}",
            )
    for obj in meshes:
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.normals_make_consistent(inside=False)
        bpy.ops.object.mode_set(mode="OBJECT")

    bpy.ops.object.select_all(action="DESELECT")
    for obj in meshes:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]

    for obj in list(bpy.context.scene.objects):
        if obj.type != "MESH":
            bpy.data.objects.remove(obj, do_unlink=True)

    export_fbx.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.fbx(
        filepath=str(export_fbx),
        use_selection=True,
        object_types={"MESH"},
        use_mesh_modifiers=False,
        mesh_smooth_type="OFF",
        use_tspace=True,
        add_leaf_bones=False,
        bake_anim=False,
        path_mode="STRIP",
        embed_textures=False,
        apply_unit_scale=True,
        apply_scale_options="FBX_SCALE_ALL",
        axis_forward="-Z",
        axis_up="Y",
        global_scale=1.0,
    )

    print(f"saved source {source_blend}")
    print(f"exported {export_fbx}")


if __name__ == "__main__":
    main()

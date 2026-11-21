import csv
import sys
from pathlib import Path

import bpy


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    if len(argv) != 2:
        raise SystemExit("usage: blender --background --python inspect_blender_fbx.py -- <fbx> <out-dir>")
    return Path(argv[0]), Path(argv[1])


def world_bounds(obj):
    if obj.type != "MESH":
        return None
    points = [obj.matrix_world @ vertex.co for vertex in obj.data.vertices]
    if not points:
        return None
    minimum = [min(point[index] for point in points) for index in range(3)]
    maximum = [max(point[index] for point in points) for index in range(3)]
    return minimum, maximum


def union_bounds(objects):
    bounds = [world_bounds(obj) for obj in objects]
    bounds = [value for value in bounds if value is not None]
    if not bounds:
        return None
    minimum = [min(value[0][index] for value in bounds) for index in range(3)]
    maximum = [max(value[1][index] for value in bounds) for index in range(3)]
    return minimum, maximum


def mesh_descendants(obj):
    result = []
    for child in obj.children:
        if child.type == "MESH":
            result.append(child)
        result.extend(mesh_descendants(child))
    return result


def nearest_group_parent(obj):
    parent = obj.parent
    while parent is not None:
        if parent.name.startswith("3d66Group") and parent.type == "EMPTY":
            return parent
        parent = parent.parent
    return None


def main():
    fbx_path, output_dir = parse_args()
    output_dir.mkdir(parents=True, exist_ok=True)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(
        filepath=str(fbx_path),
        use_custom_normals=True,
        use_image_search=True,
    )

    objects = list(bpy.context.scene.objects)
    with (output_dir / "objects.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "name",
                "type",
                "parent",
                "children",
                "collections",
                "mesh_vertices",
                "materials",
                "bbox_min",
                "bbox_max",
                "location",
            ]
        )
        for obj in sorted(objects, key=lambda item: item.name):
            bounds = world_bounds(obj)
            writer.writerow(
                [
                    obj.name,
                    obj.type,
                    obj.parent.name if obj.parent else "",
                    "|".join(child.name for child in obj.children),
                    "|".join(collection.name for collection in obj.users_collection),
                    len(obj.data.vertices) if obj.type == "MESH" else 0,
                    "|".join(
                        slot.material.name
                        for slot in obj.material_slots
                        if slot.material is not None
                    ),
                    "|".join(f"{value:.6f}" for value in bounds[0]) if bounds else "",
                    "|".join(f"{value:.6f}" for value in bounds[1]) if bounds else "",
                    "|".join(f"{value:.6f}" for value in obj.location),
                ]
            )

    with (output_dir / "collections.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["collection", "objects", "children"])
        for collection in sorted(bpy.data.collections, key=lambda item: item.name):
            writer.writerow(
                [
                    collection.name,
                    len(collection.objects),
                    "|".join(child.name for child in collection.children),
                ]
            )

    group_objects = [
        obj
        for obj in objects
        if obj.type == "EMPTY" and obj.name.startswith("3d66Group")
    ]
    with (output_dir / "groups.csv").open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "group",
                "parent_group",
                "direct_meshes",
                "direct_vertices",
                "all_meshes",
                "all_vertices",
                "child_groups",
                "materials",
                "direct_bbox_min",
                "direct_bbox_max",
                "all_bbox_min",
                "all_bbox_max",
            ]
        )
        for group in sorted(group_objects, key=lambda item: item.name):
            direct_meshes = [child for child in group.children if child.type == "MESH"]
            all_meshes = mesh_descendants(group)
            child_groups = [
                child.name
                for child in group.children
                if child.type == "EMPTY" and child.name.startswith("3d66Group")
            ]
            materials = sorted(
                {
                    slot.material.name
                    for mesh in all_meshes
                    for slot in mesh.material_slots
                    if slot.material is not None
                }
            )
            direct_bounds = union_bounds(direct_meshes)
            all_bounds = union_bounds(all_meshes)
            parent_group = nearest_group_parent(group)
            writer.writerow(
                [
                    group.name,
                    parent_group.name if parent_group else "",
                    len(direct_meshes),
                    sum(len(mesh.data.vertices) for mesh in direct_meshes),
                    len(all_meshes),
                    sum(len(mesh.data.vertices) for mesh in all_meshes),
                    "|".join(child_groups),
                    "|".join(materials),
                    "|".join(f"{value:.3f}" for value in direct_bounds[0])
                    if direct_bounds
                    else "",
                    "|".join(f"{value:.3f}" for value in direct_bounds[1])
                    if direct_bounds
                    else "",
                    "|".join(f"{value:.3f}" for value in all_bounds[0])
                    if all_bounds
                    else "",
                    "|".join(f"{value:.3f}" for value in all_bounds[1])
                    if all_bounds
                    else "",
                ]
            )

    with (output_dir / "summary.txt").open("w", encoding="utf-8") as handle:
        handle.write(f"objects={len(objects)}\n")
        handle.write(f"meshes={sum(1 for obj in objects if obj.type == 'MESH')}\n")
        handle.write(f"empties={sum(1 for obj in objects if obj.type == 'EMPTY')}\n")
        handle.write(f"materials={len(bpy.data.materials)}\n")
        handle.write(f"images={len(bpy.data.images)}\n")
        handle.write(f"collections={len(bpy.data.collections)}\n")

    print(f"imported {len(objects)} objects")


if __name__ == "__main__":
    main()

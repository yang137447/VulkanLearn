import csv
import math
import re
import sys
from collections import defaultdict
from pathlib import Path

import bpy
from mathutils import Matrix, Vector


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    if len(argv) != 3:
        raise SystemExit(
            "usage: blender --background <blend> --python "
            "restore_blender_fbx_instances.py -- <instances.csv> <mesh_bounds.csv> <out.blend>"
        )
    return Path(argv[0]), Path(argv[1]), Path(argv[2])


def read_rows(path):
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def object_bounds(obj):
    if obj.type != "MESH":
        return None
    points = [obj.matrix_world @ vertex.co for vertex in obj.data.vertices]
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


def matrix_from_row(row):
    values = [float(row[f"m{row_index}{column_index}"]) for row_index in range(4) for column_index in range(4)]
    return Matrix(
        (
            values[0:4],
            values[4:8],
            values[8:12],
            values[12:16],
        )
    )


def converted_bounds(row):
    minimum = Vector(
        (
            float(row["world_min_x"]),
            -float(row["world_max_z"]),
            float(row["world_min_y"]),
        )
    )
    maximum = Vector(
        (
            float(row["world_max_x"]),
            -float(row["world_min_z"]),
            float(row["world_max_y"]),
        )
    )
    return minimum, maximum


def bounds_score(bounds, reference):
    if bounds is None or reference is None:
        return 1.0e30
    center = (bounds[0] + bounds[1]) * 0.5
    reference_center = (reference[0] + reference[1]) * 0.5
    size = bounds[1] - bounds[0]
    reference_size = reference[1] - reference[0]
    return (center - reference_center).length + (size - reference_size).length


def nearest_group(node_path):
    matches = re.findall(r"3d66Group\d+", node_path)
    if not matches:
        return None
    return bpy.data.objects.get(matches[-1])


def main():
    instances_path, bounds_path, output_path = parse_args()
    instances = read_rows(instances_path)
    bounds_by_mesh = {
        row["mesh_index"]: row for row in read_rows(bounds_path)
    }
    grouped = defaultdict(list)
    for row in instances:
        grouped[row["mesh_name"]].append(row)

    axis_to_blender = Matrix.Rotation(math.radians(90.0), 4, "X")
    axis_to_assimp = axis_to_blender.inverted()
    added = 0
    missing_objects = []
    duplicate_names = 0

    for mesh_name, rows in sorted(grouped.items()):
        if len(rows) <= 1:
            continue
        duplicate_names += 1
        base_object = bpy.data.objects.get(mesh_name)
        if base_object is None or base_object.type != "MESH":
            missing_objects.append(mesh_name)
            continue

        reference_bounds = object_bounds(base_object)
        scored = []
        for row in rows:
            bounds_row = bounds_by_mesh.get(row["mesh_index"])
            bounds = converted_bounds(bounds_row) if bounds_row else None
            scored.append((bounds_score(bounds, reference_bounds), row, bounds))
        scored.sort(key=lambda item: item[0])
        base_row = scored[0][1]
        base_matrix = matrix_from_row(base_row)

        for score, row, _ in scored[1:]:
            relative_assimp = matrix_from_row(row) @ base_matrix.inverted()
            relative_blender = axis_to_blender @ relative_assimp @ axis_to_assimp
            duplicate = base_object.copy()
            duplicate.data = base_object.data
            duplicate.name = f"{base_object.name}.inst{int(row['instance_index']):03d}"
            for collection in base_object.users_collection:
                collection.objects.link(duplicate)
            group = nearest_group(row["node_path"])
            if group is not None:
                duplicate.parent = group
                duplicate.matrix_parent_inverse = Matrix.Identity(4)
            duplicate.matrix_world = relative_blender @ base_object.matrix_world
            duplicate["source_instance_index"] = int(row["instance_index"])
            added += 1

    bpy.context.view_layer.update()
    bpy.ops.wm.save_as_mainfile(filepath=str(output_path))
    print(
        f"duplicate names={duplicate_names} added instances={added} "
        f"objects={len(bpy.context.scene.objects)}"
    )
    if missing_objects:
        print("missing base objects:")
        for name in missing_objects:
            print(f"  {name}")


if __name__ == "__main__":
    main()

import csv
import math
import re
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    if len(argv) < 4:
        raise SystemExit(
            "usage: blender --background --python split_3d66_blender.py -- "
            "<fbx> <nodes-csv> <models-dir> <groups-csv> [--only GROUP]"
        )
    only = None
    if "--only" in argv:
        index = argv.index("--only")
        only = argv[index + 1]
        argv = argv[:index] + argv[index + 2 :]
    return Path(argv[0]), Path(argv[1]), Path(argv[2]), Path(argv[3]), only


def group_number(name):
    match = re.search(r"(\d+)$", name)
    return int(match.group(1)) if match else 1 << 30


def mesh_bounds(objects):
    points = []
    for obj in objects:
        if obj.type != "MESH":
            continue
        points.extend(obj.matrix_world @ vertex.co for vertex in obj.data.vertices)
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


def subtree(group):
    return [group, *group.children_recursive]


def load_material_map(nodes_csv):
    result = {}
    with nodes_csv.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            mesh_names = [value for value in row["mesh_names"].split("|") if value]
            material_names = [value for value in row["material_names"].split("|") if value]
            for index, mesh_name in enumerate(mesh_names):
                if material_names:
                    result[mesh_name] = material_names[min(index, len(material_names) - 1)]
    return result


def clear_and_assign_materials(objects, material_map):
    for material in list(bpy.data.materials):
        bpy.data.materials.remove(material, do_unlink=True)
    cache = {}
    for obj in objects:
        if obj.type != "MESH":
            continue
        if obj.data.users > 1:
            obj.data = obj.data.copy()
        obj.data.materials.clear()
        material_name = material_map.get(obj.name, "Default")
        material = cache.get(material_name)
        if material is None:
            material = bpy.data.materials.new(name=material_name)
            material.use_nodes = False
            cache[material_name] = material
        obj.data.materials.append(material)
    return cache


def bake_world_transforms(objects):
    bpy.ops.object.select_all(action="DESELECT")
    active = None
    axis_fix = Matrix.Rotation(math.radians(-90.0), 4, "X")
    for obj in objects:
        if obj.type != "MESH":
            continue
        if obj.data.users > 1:
            obj.data = obj.data.copy()
        world = axis_fix @ obj.matrix_world
        obj.parent = None
        obj.matrix_world = world
        obj.select_set(True)
        active = obj
    if active is not None:
        bpy.context.view_layer.objects.active = active
        bpy.ops.object.transform_apply(
            location=True,
            rotation=True,
            scale=True,
        )


def export_group(group, output_path, material_map):
    descendants = subtree(group)
    meshes = [obj for obj in descendants if obj.type == "MESH"]
    bounds = mesh_bounds(meshes)
    if bounds is None:
        return None, 0

    minimum, maximum = bounds
    offset = Vector(
        (
            -(minimum.x + maximum.x) * 0.5,
            -(minimum.y + maximum.y) * 0.5,
            -minimum.z,
        )
    )
    original = group.matrix_world.copy()
    group.matrix_world = Matrix.Translation(offset) @ group.matrix_world
    bpy.context.view_layer.update()
    materials = clear_and_assign_materials(meshes, material_map)
    bake_world_transforms(meshes)

    bpy.ops.object.select_all(action="DESELECT")
    for obj in descendants:
        obj.hide_set(False)
        obj.hide_viewport = False
        obj.select_set(True)
    bpy.context.view_layer.objects.active = group

    try:
        bpy.ops.export_scene.fbx(
            filepath=str(output_path),
            use_selection=True,
            object_types={"EMPTY", "MESH"},
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
    finally:
        group.matrix_world = original
        bpy.context.view_layer.update()

    return {
        "minimum": minimum,
        "maximum": maximum,
        "meshes": len(meshes),
        "vertices": sum(len(obj.data.vertices) for obj in meshes),
        "materials": len(materials),
    }, len(meshes)


def main():
    fbx_path, nodes_csv, models_dir, groups_csv, only = parse_args()
    material_map = load_material_map(nodes_csv)
    models_dir.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(
        filepath=str(fbx_path),
        use_custom_normals=True,
        use_image_search=True,
    )

    groups = sorted(
        (
            obj
            for obj in bpy.context.scene.objects
            if obj.type == "EMPTY"
            and obj.name.startswith("3d66Group")
            and obj.parent is None
        ),
        key=lambda item: group_number(item.name),
    )
    if only is not None:
        groups = [group for group in groups if group.name == only]
    if not groups:
        raise SystemExit("no top-level 3d66 group objects found")

    rows = []
    for sequential, group in enumerate(groups, start=1):
        asset_id = f"{sequential:03d}"
        output_path = models_dir / f"SM_cloth_{asset_id}.fbx"
        info, object_count = export_group(group, output_path, material_map)
        if info is None:
            print(f"skip empty group {group.name}")
            continue
        rows.append(
            {
                "id": asset_id,
                "source_group": group.name,
                "object_count": object_count,
                "material_count": info["materials"],
                "vertices": info["vertices"],
                "bbox_min": "|".join(f"{value:.6f}" for value in info["minimum"]),
                "bbox_max": "|".join(f"{value:.6f}" for value in info["maximum"]),
                "fbx": f"Maps/SC_cloth_showcase/Source/Models/SM_cloth_{asset_id}.fbx",
            }
        )
        print(f"exported {group.name} -> {output_path.name}")

    groups_csv.parent.mkdir(parents=True, exist_ok=True)
    with groups_csv.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"exported {len(rows)} top-level clothing groups")


if __name__ == "__main__":
    main()

import json
import os
import shutil
import sys
import time
from pathlib import Path

import bpy
from mathutils import Matrix


PARTS = {
    "main": "main",
    "BL": "wheel_bl",
    "FR": "wheel_fr",
    "FL": "wheel_fl",
    "BR": "wheel_br",
    "Plane.001": "floor",
}

MATERIAL_INSTANCES = {
    "M_Carpaint": "materials/car/MI_car_carpaint.json",
    "M_Inner": "materials/car/MI_car_inner.json",
    "M_Light": "materials/car/MI_car_light.json",
    "M_Grid": "materials/car/MI_car_grille.json",
    "M_Out": "materials/car/MI_car_exterior_trim.json",
    "M_Logo": "materials/car/MI_car_logo.json",
    "M_LicensePlate": "materials/car/MI_car_license_plate.json",
    "M_Glass_Light": "materials/car/MI_car_light_glass.json",
    "M_Wheel_Tread": "materials/car/MI_car_wheel_tread.json",
    "M_Wheel_Brake": "materials/car/MI_car_wheel_brake.json",
    "M_Wheel_Hub": "materials/car/MI_car_wheel_hub.json",
    "M_Car_Showcase_Floor": "materials/car/MI_car_showcase_floor.json",
}

MATERIAL_EXPORT_NAMES = {
    "Material.001": "M_Car_Showcase_Floor",
}


def atomic_write_json(path, data):
    temporary_path = path.with_suffix(path.suffix + ".tmp")
    temporary_path.write_text(
        json.dumps(data, ensure_ascii=False, indent=4) + "\n",
        encoding="utf-8",
    )

    for attempt in range(10):
        try:
            os.replace(temporary_path, path)
            return
        except PermissionError:
            if attempt == 9:
                raise
            time.sleep(0.1 * (attempt + 1))


def remove_stale_assets(model_directory, model_data_directory):
    expected_descriptors = {
        f"SM_car_{asset_id}.json"
        for asset_id in PARTS.values()
    }
    expected_data_files = {"car_asset_manifest.json"}
    for asset_id in PARTS.values():
        expected_data_files.add(f"car_{asset_id}.obj")
        expected_data_files.add(f"car_{asset_id}.mtl")

    resolved_model_directory = model_directory.resolve()
    resolved_model_data_directory = model_data_directory.resolve()
    stale_files = []

    for path in model_directory.glob("SM_car_*.json"):
        if path.name not in expected_descriptors:
            stale_files.append((path, resolved_model_directory))

    for path in model_data_directory.glob("car_*"):
        if path.is_file() and path.name not in expected_data_files:
            stale_files.append((path, resolved_model_data_directory))

    for path, expected_parent in stale_files:
        resolved_path = path.resolve()
        if resolved_path.parent != expected_parent:
            raise RuntimeError(f"Refusing to remove asset outside export root: {resolved_path}")
        resolved_path.unlink()

    return len(stale_files)


def remove_legacy_export_directory(model_data_directory):
    model_data_root = model_data_directory.parent.resolve()
    legacy_directory = model_data_root / "car_showcase.fbm"
    if not legacy_directory.exists():
        return 0

    resolved_legacy_directory = legacy_directory.resolve()
    if (
        resolved_legacy_directory.parent != model_data_root
        or resolved_legacy_directory.name != "car_showcase.fbm"
    ):
        raise RuntimeError(
            "Refusing to remove unexpected legacy export directory: "
            f"{resolved_legacy_directory}"
        )

    removed_file_count = sum(
        1
        for path in resolved_legacy_directory.rglob("*")
        if path.is_file()
    )
    shutil.rmtree(resolved_legacy_directory)
    return removed_file_count


def exported_material_name(material):
    return MATERIAL_EXPORT_NAMES.get(material.name, material.name)


def engine_position(obj):
    location = obj.location
    return [
        float(location.x),
        float(location.z),
        float(-location.y),
    ]


def engine_scale(obj):
    scale = obj.scale
    return [
        float(scale.x),
        float(scale.z),
        float(scale.y),
    ]


def engine_rotation(obj):
    epsilon = 1.0e-5
    if any(abs(value) > epsilon for value in obj.rotation_euler):
        raise RuntimeError(
            f"Object '{obj.name}' has a non-zero rotation. "
            "Add an explicit Blender-to-engine rotation conversion before export."
        )
    return [0.0, 0.0, 0.0]


def validate_source_objects():
    mesh_objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    actual_names = {obj.name for obj in mesh_objects}
    expected_names = set(PARTS)

    if actual_names != expected_names:
        missing = sorted(expected_names - actual_names)
        unexpected = sorted(actual_names - expected_names)
        raise RuntimeError(
            "Car_02.blend scene mesh set changed. "
            f"missing={missing} unexpected={unexpected}"
        )

    for obj in mesh_objects:
        if not obj.data.uv_layers:
            raise RuntimeError(f"Object '{obj.name}' has no UV layer")
        if not obj.material_slots:
            raise RuntimeError(f"Object '{obj.name}' has no material slots")

        for slot in obj.material_slots:
            if slot.material is None:
                raise RuntimeError(f"Object '{obj.name}' has an empty material slot")
            material_name = exported_material_name(slot.material)
            if material_name not in MATERIAL_INSTANCES:
                raise RuntimeError(
                    f"Object '{obj.name}' uses unmapped material '{material_name}'"
                )


def export_object(obj, asset_id, model_data_directory):
    logical_name = f"SM_car_{asset_id}"
    obj_path = model_data_directory / f"car_{asset_id}.obj"

    original_matrix = obj.matrix_world.copy()
    original_name = obj.name
    renamed_materials = []

    try:
        obj.matrix_world = Matrix.Identity(4)
        obj.name = logical_name

        for slot in obj.material_slots:
            material = slot.material
            export_name = exported_material_name(material)
            if export_name != material.name:
                renamed_materials.append((material, material.name))
                material.name = export_name

        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.wm.obj_export(
            filepath=str(obj_path),
            export_selected_objects=True,
            apply_modifiers=True,
            export_uv=True,
            export_normals=True,
            export_materials=True,
            export_colors=False,
            export_triangulated_mesh=False,
            forward_axis="NEGATIVE_Z",
            up_axis="Y",
        )
    finally:
        for material, original_material_name in renamed_materials:
            material.name = original_material_name
        obj.name = original_name
        obj.matrix_world = original_matrix

    return logical_name, obj_path


def create_model_descriptor(obj, asset_id):
    logical_name = f"SM_car_{asset_id}"
    material_slots = []

    for slot in obj.material_slots:
        material_name = exported_material_name(slot.material)
        material_slots.append(
            {
                "name": material_name,
                "materialInstancePath": MATERIAL_INSTANCES[material_name],
            }
        )

    return {
        "name": logical_name,
        "type": "mesh",
        "modelDataPath": f"models/datas/car/car_{asset_id}.obj",
        "materialSlots": material_slots,
    }


def create_scene_mesh_object(obj, asset_id):
    logical_name = f"SM_car_{asset_id}"
    return {
        "name": logical_name,
        "type": "mesh",
        "modelPath": f"models/{logical_name}.json",
        "position": engine_position(obj),
        "scale": engine_scale(obj),
        "rotation": engine_rotation(obj),
    }


def main():
    arguments = sys.argv[sys.argv.index("--") + 1 :]
    if len(arguments) != 2:
        raise RuntimeError(
            "Usage: blender --background Car_02.blend --python export_car_showcase.py "
            "-- resource_root scene_path"
        )

    resource_root = Path(arguments[0])
    scene_path = Path(arguments[1])
    model_directory = resource_root / "models"
    model_data_directory = model_directory / "datas" / "car"
    manifest_path = model_data_directory / "car_asset_manifest.json"

    model_directory.mkdir(parents=True, exist_ok=True)
    model_data_directory.mkdir(parents=True, exist_ok=True)

    validate_source_objects()

    scene = json.loads(scene_path.read_text(encoding="utf-8-sig"))
    non_mesh_objects = [
        scene_object
        for scene_object in scene["objects"]
        if scene_object.get("type") != "mesh"
    ]

    scene_mesh_objects = []
    manifest_parts = []

    for blender_name, asset_id in PARTS.items():
        obj = bpy.context.scene.objects[blender_name]
        logical_name, obj_path = export_object(obj, asset_id, model_data_directory)

        model_descriptor_path = model_directory / f"{logical_name}.json"
        atomic_write_json(
            model_descriptor_path,
            create_model_descriptor(obj, asset_id),
        )

        scene_mesh_object = create_scene_mesh_object(obj, asset_id)
        scene_mesh_objects.append(scene_mesh_object)
        manifest_parts.append(
            {
                "blenderObject": blender_name,
                "parent": obj.parent.name if obj.parent else None,
                "assetId": asset_id,
                "modelPath": scene_mesh_object["modelPath"],
                "materials": [
                    exported_material_name(slot.material)
                    for slot in obj.material_slots
                ],
                "position": scene_mesh_object["position"],
                "rotation": scene_mesh_object["rotation"],
                "scale": scene_mesh_object["scale"],
                "vertexCount": len(obj.data.vertices),
                "polygonCount": len(obj.data.polygons),
                "objPath": str(obj_path.relative_to(resource_root)).replace("\\", "/"),
            }
        )

    scene["objects"] = scene_mesh_objects + non_mesh_objects
    atomic_write_json(scene_path, scene)

    manifest = {
        "sourceBlend": bpy.data.filepath,
        "partCount": len(manifest_parts),
        "parts": manifest_parts,
    }
    atomic_write_json(manifest_path, manifest)
    removed_stale_asset_count = remove_stale_assets(
        model_directory,
        model_data_directory,
    )
    removed_legacy_file_count = remove_legacy_export_directory(
        model_data_directory,
    )

    print(
        "CAR_SHOWCASE_EXPORT "
        f"parts={len(manifest_parts)} "
        f"removedStaleAssets={removed_stale_asset_count + removed_legacy_file_count} "
        f"scene={scene_path} "
        f"manifest={manifest_path}"
    )


main()

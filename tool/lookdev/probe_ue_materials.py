"""Inspect UE material parents and authored texture/scalar parameters."""

from __future__ import annotations

import json
import os
from pathlib import Path

import unreal


ASSET_ROOT = "/Game/TemplateLevels/Assets"
MATERIALS = (
    "Base_M",
    "M_ColorGrid",
    "M_SkinBall",
    "Gray_MI",
    "Gray_NoSpec",
    "Gradient_M",
    "ArrowColor_M",
    "BackDrop_M",
)


def safe_name(value) -> str | None:
    if value is None:
        return None
    try:
        return value.get_path_name()
    except AttributeError:
        return str(value)


def parameter_name(value) -> str:
    try:
        return str(value.parameter_info.name)
    except AttributeError:
        return str(getattr(value, "parameter_name", ""))


def material_record(asset) -> dict:
    record = {
        "name": asset.get_name(),
        "path": asset.get_path_name(),
        "class": asset.get_class().get_name(),
    }
    if isinstance(asset, unreal.MaterialInstance):
        parent = asset.get_editor_property("parent")
        record["parent"] = safe_name(parent)
        record["scalarParameters"] = {
            parameter_name(item): float(item.parameter_value)
            for item in asset.get_editor_property("scalar_parameter_values")
        }
        record["vectorParameters"] = {
            parameter_name(item): [
                float(item.parameter_value.r),
                float(item.parameter_value.g),
                float(item.parameter_value.b),
                float(item.parameter_value.a),
            ]
            for item in asset.get_editor_property("vector_parameter_values")
        }
        record["textureParameters"] = {
            parameter_name(item): safe_name(item.parameter_value)
            for item in asset.get_editor_property("texture_parameter_values")
        }
    elif isinstance(asset, unreal.Material):
        try:
            record["usedTextures"] = [
                safe_name(texture)
                for texture in unreal.MaterialEditingLibrary.get_used_textures(asset)
            ]
        except Exception as error:
            record["usedTexturesError"] = str(error)
    return record


def main() -> None:
    output = Path(os.environ["VL_UE_PROBE_OUTPUT"])
    records = []
    for name in MATERIALS:
        path = f"{ASSET_ROOT}/{name}"
        asset = unreal.load_asset(path)
        if asset is None:
            records.append({"name": name, "path": path, "loaded": False})
            continue
        records.append({"loaded": True, **material_record(asset)})

    mesh = unreal.load_asset(f"{ASSET_ROOT}/SM_ColorCalibrator")
    mesh_materials = []
    if mesh is not None:
        for index in range(len(mesh.get_editor_property("static_materials"))):
            material = mesh.get_material(index)
            mesh_materials.append({
                "slot": index,
                "material": safe_name(material),
                "name": material.get_name() if material else None,
            })

    output.write_text(
        json.dumps(
            {"materials": records, "colorCalibratorSlots": mesh_materials},
            ensure_ascii=False,
            indent=2,
        ) + "\n",
        encoding="utf-8",
    )
    unreal.SystemLibrary.quit_editor()


main()

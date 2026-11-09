"""Headless export of the UE Niagara reference meshes and textures.

Run with UnrealEditor-Cmd:
  UnrealEditor-Cmd.exe <effect.uproject> -run=pythonscript -script=<this file>
    -EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities
    -unattended -nosound -nop4 -nosplash -nullrhi
"""

from __future__ import annotations

import json
from pathlib import Path

import unreal


RESOURCE_ROOT = Path(
    r"D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/niagara-particle-system/"
    r"VukanLearnResources"
)
OUTPUT_ROOT = RESOURCE_ROOT / "Common/Source/UE/NiagaraExamples"
MANIFEST_DIR = RESOURCE_ROOT / "Generated/Import/ue_niagara_assets"

ASSETS = (
    {
        "id": "DinoDragon",
        "path": "/Game/NiagaraExamples/Gallery/SkeletalMesh/DinoDragon/SK_DinoDragon",
        "kind": "skeletal",
        "output": "SkeletalMesh/DinoDragon/SK_DinoDragon.fbx",
    },
    {
        "id": "Manny",
        "path": "/Game/NiagaraExamples/Gallery/SkeletalMesh/Mannequins/Meshes/SKM_Manny_Simple",
        "kind": "skeletal",
        "output": "SkeletalMesh/Mannequins/SKM_Manny_Simple.fbx",
    },
    {
        "id": "Quinn",
        "path": "/Game/NiagaraExamples/Gallery/SkeletalMesh/Mannequins/Meshes/SKM_Quinn_Simple",
        "kind": "skeletal",
        "output": "SkeletalMesh/Mannequins/SKM_Quinn_Simple.fbx",
    },
    {
        "id": "ToyCow",
        "path": "/Game/NiagaraExamples/Gallery/StaticMesh/ToyCow/SM_ToyCow",
        "kind": "static",
        "output": "StaticMesh/ToyCow/SM_ToyCow.fbx",
    },
    {
        "id": "Rifle",
        "path": "/Game/NiagaraExamples/Gallery/Weapons/Rifle/Mesh/SM_Rifle",
        "kind": "static",
        "output": "StaticMesh/Rifle/SM_Rifle.fbx",
    },
)

MATERIAL_ASSETS = (
    "/Game/NiagaraExamples/Gallery/SkeletalMesh/DinoDragon/Material/DINO",
    "/Game/NiagaraExamples/Gallery/SkeletalMesh/Mannequins/Materials/Manny/MI_Manny_01_New",
    "/Game/NiagaraExamples/Gallery/SkeletalMesh/Mannequins/Materials/Quinn/MI_Quinn_01",
    "/Game/NiagaraExamples/Gallery/StaticMesh/ToyCow/Materials/M_Toy_Cow",
    "/Game/NiagaraExamples/Gallery/Weapons/Rifle/Materials/MI_Weapon_Rifle",
)

EXTRA_TEXTURE_ASSETS = (
    "/Game/NiagaraExamples/Gallery/Weapons/Rifle/Textures/T_Rifle_D",
    "/Game/NiagaraExamples/Gallery/Weapons/Rifle/Textures/T_Rifle_Combined_N",
    "/Game/NiagaraExamples/Gallery/Weapons/Rifle/Textures/T_Rifle_AORM",
    "/Game/NiagaraExamples/Gallery/Weapons/Rifle/Textures/T_Rifle_Masks",
)


def log(message: str) -> None:
    unreal.log("[VL Niagara Export] " + message)


def export_asset(asset, output: Path, kind: str) -> dict:
    output.parent.mkdir(parents=True, exist_ok=True)
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = str(output)
    task.automated = True
    task.replace_identical = True
    task.prompt = False
    task.selected = False
    if kind == "skeletal":
        task.exporter = unreal.SkeletalMeshExporterFBX()
    elif kind == "static":
        task.exporter = unreal.StaticMeshExporterFBX()
    elif kind == "texture":
        task.exporter = unreal.TextureExporterPNG()
    else:
        raise ValueError("unknown export kind: " + kind)

    if kind in ("skeletal", "static"):
        try:
            options = unreal.FbxExportOption()
            for name, value in (
                ("ascii", False),
                ("force_front_x_axis", False),
                ("collision", False),
                ("level_of_detail", False),
                ("vertex_color", True),
                ("export_morph_targets", False),
                ("export_preview_mesh", False),
                ("map_skeletal_motion_to_root", False),
            ):
                try:
                    options.set_editor_property(name, value)
                except Exception:
                    pass
            try:
                options.set_editor_property(
                    "bake_material_inputs", unreal.FbxMaterialBakeMode.DISABLED)
            except Exception:
                pass
            task.options = options
        except Exception:
            pass

    ok = unreal.Exporter.run_asset_export_task(task)
    exists = output.is_file() and output.stat().st_size > 0
    return {
        "path": str(output.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
        "exported": bool(ok and exists),
        "size": output.stat().st_size if exists else 0,
        "errors": list(getattr(task, "errors", [])),
    }


def class_name(asset) -> str:
    try:
        return asset.get_class().get_name()
    except Exception:
        return ""


def asset_record(asset) -> dict | None:
    if asset is None:
        return None
    try:
        return {
            "name": asset.get_name(),
            "path": asset.get_path_name(),
            "class": class_name(asset),
        }
    except Exception:
        return None


def material_slots(asset) -> list[dict]:
    slots = []
    for property_name in ("materials", "static_materials"):
        try:
            values = asset.get_editor_property(property_name)
        except Exception:
            continue
        for index, entry in enumerate(values):
            material = None
            slot_name = ""
            for key in ("material_interface", "material"):
                try:
                    material = entry.get_editor_property(key)
                    if material is not None:
                        break
                except Exception:
                    continue
            try:
                slot_name = str(entry.get_editor_property("material_slot_name"))
            except Exception:
                pass
            slots.append({
                "index": index,
                "slotName": slot_name,
                "material": asset_record(material),
            })
        if slots:
            break
    return slots


def bone_hierarchy(asset) -> dict:
    result = {"skeletonAsset": None, "bones": [], "error": None}
    try:
        skeleton = asset.get_editor_property("skeleton")
        result["skeletonAsset"] = asset_record(skeleton)
        if skeleton is None:
            return result
        tree = skeleton.get_editor_property("bone_tree")
        for index, node in enumerate(tree):
            name = node.get_editor_property("name")
            parent = node.get_editor_property("parent_index")
            result["bones"].append({
                "index": index,
                "name": str(name),
                "parentIndex": int(parent),
            })
    except Exception as error:
        result["error"] = str(error)
    return result


def texture_from_expression(expression) -> object | None:
    for key in ("texture", "texture_object"):
        try:
            value = expression.get_editor_property(key)
            if isinstance(value, unreal.Texture2D):
                return value
        except Exception:
            continue
    return None


def collect_material_textures(material) -> list[dict]:
    textures: dict[str, dict] = {}
    if material is None:
        return []
    try:
        expressions = list(unreal.MaterialEditingLibrary.get_material_expressions(material))
    except Exception:
        return []
    for expression in expressions:
        texture = texture_from_expression(expression)
        record = asset_record(texture)
        if record and record["path"] not in textures:
            textures[record["path"]] = record
    return list(textures.values())


def material_record(path: str) -> dict:
    asset = unreal.load_asset(path)
    if asset is None:
        return {"path": path, "loaded": False}
    result = {
        "path": path,
        "loaded": True,
        "name": asset.get_name(),
        "class": class_name(asset),
        "textures": [],
        "scalars": [],
        "vectors": [],
    }
    try:
        parent = asset.get_editor_property("parent")
    except Exception:
        parent = asset
    result["sourceMaterial"] = asset_record(parent)
    result["textures"] = collect_material_textures(parent)
    for parameter in ("get_material_parameter_names", "get_material_instance_parameter_names"):
        function = getattr(unreal.MaterialEditingLibrary, parameter, None)
        if function is None:
            continue
        try:
            names = list(function(asset))
        except Exception:
            continue
        for name in names:
            name = str(name)
            try:
                value = unreal.MaterialEditingLibrary.get_material_instance_scalar_parameter_value(
                    asset, name)
                if abs(float(value)) > 1e-9:
                    result["scalars"].append({"name": name, "value": float(value)})
            except Exception:
                pass
            try:
                value = unreal.MaterialEditingLibrary.get_material_instance_vector_parameter_value(
                    asset, name)
                if any(abs(float(component)) > 1e-9 for component in value):
                    result["vectors"].append({
                        "name": name,
                        "value": [float(component) for component in value],
                    })
            except Exception:
                pass
    return result


def main() -> None:
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    MANIFEST_DIR.mkdir(parents=True, exist_ok=True)
    manifest = {
        "engineVersion": unreal.SystemLibrary.get_engine_version(),
        "project": str(Path(unreal.Paths.project_dir()).resolve()).replace("\\", "/"),
        "assets": [],
        "materials": [],
        "textures": [],
        "failures": [],
    }
    texture_assets: dict[str, object] = {}

    for spec in ASSETS:
        asset = unreal.load_asset(spec["path"])
        if asset is None:
            manifest["failures"].append({"asset": spec["path"], "error": "load failed"})
            continue
        record = {
            "id": spec["id"],
            "uePath": spec["path"],
            "kind": spec["kind"],
            "class": class_name(asset),
            "materials": material_slots(asset),
        }
        if spec["kind"] == "skeletal":
            record["skeleton"] = bone_hierarchy(asset)
        record["export"] = export_asset(asset, OUTPUT_ROOT / spec["output"], spec["kind"])
        manifest["assets"].append(record)
        for slot in record["materials"]:
            material = slot.get("material")
            if material is None:
                continue
            material_asset = unreal.load_asset(material["path"].split(".")[0])
            if material_asset is None:
                continue
            try:
                parent = material_asset.get_editor_property("parent")
            except Exception:
                parent = material_asset
            for texture in collect_material_textures(parent):
                texture_assets[texture["path"]] = texture

    for path in MATERIAL_ASSETS:
        manifest["materials"].append(material_record(path))

    for path in EXTRA_TEXTURE_ASSETS:
        asset = unreal.load_asset(path)
        if asset is None:
            manifest["failures"].append({"asset": path, "error": "texture load failed"})
            continue
        texture_assets[path] = asset_record(asset)

    for path in texture_assets:
        asset = unreal.load_asset(path.split(".")[0])
        if asset is None:
            manifest["failures"].append({"asset": path, "error": "texture load failed"})
            continue
        output = OUTPUT_ROOT / "Textures" / (asset.get_name() + ".png")
        export = export_asset(asset, output, "texture")
        manifest["textures"].append({
            "uePath": path,
            "name": asset.get_name(),
            "export": export,
        })
        if not export["exported"]:
            manifest["failures"].append({
                "asset": path,
                "error": "texture export failed",
            })

    manifest_path = MANIFEST_DIR / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, default=str) + "\n",
                             encoding="utf-8")
    log("wrote " + str(manifest_path))
    log("assets=%d textures=%d failures=%d" % (
        len(manifest["assets"]), len(manifest["textures"]), len(manifest["failures"])))
    unreal.SystemLibrary.quit_editor()


main()

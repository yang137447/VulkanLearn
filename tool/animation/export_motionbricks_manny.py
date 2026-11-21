import json
import os
from pathlib import Path

import unreal


def main():
    output = Path(os.environ["VL_MANNY_EXPORT_DIR"])
    output.mkdir(parents=True, exist_ok=True)
    mesh = unreal.load_asset("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple")
    if not isinstance(mesh, unreal.SkeletalMesh):
        raise RuntimeError("UE5 Manny skeletal mesh was not found")
    options = unreal.FbxExportOption()
    options.set_editor_property("ascii", False)
    options.set_editor_property("level_of_detail", False)
    options.set_editor_property("collision", False)
    task = unreal.AssetExportTask()
    task.object = mesh
    task.filename = str(output / "SKM_Manny_Simple.fbx")
    task.automated = True
    task.replace_identical = True
    task.prompt = False
    task.options = options
    if not unreal.Exporter.run_asset_export_task(task):
        raise RuntimeError("Manny FBX export failed")
    materials = []
    for material in mesh.get_editor_property("materials"):
        materials.append(str(material.get_editor_property("material_slot_name")))
    report = {
        "engine": unreal.SystemLibrary.get_engine_version(),
        "source": mesh.get_path_name(),
        "skeleton": mesh.get_editor_property("skeleton").get_path_name(),
        "materialSlots": materials,
        "purpose": "Internal MotionBricks retarget evaluation",
    }
    (output / "manny-export.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


main()

from __future__ import annotations

from pathlib import Path

import unreal


OUTPUT = Path(
    r"D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/niagara-particle-system/"
    r"VukanLearnResources/Common/Source/UE/NiagaraExamples/"
    r"SkeletalMesh/Mannequins/SKM_Manny_Simple_LOD1.fbx"
)


def main() -> None:
    asset = unreal.load_asset(
        "/Game/NiagaraExamples/Gallery/SkeletalMesh/Mannequins/Meshes/SKM_Manny_Simple")
    if asset is None:
        raise RuntimeError("Manny skeletal mesh missing")
    subsystem = unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem)
    before = subsystem.get_lod_count(asset)
    generated = subsystem.regenerate_lod(asset, 3, True, False)
    after = subsystem.get_lod_count(asset)
    removed = subsystem.remove_lods(asset, [0])
    final = subsystem.get_lod_count(asset)
    unreal.log("[VL Manny LOD1] before=%d generated=%r after=%d removed=%r final=%d" % (
        before, generated, after, removed, final))

    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = str(OUTPUT)
    task.automated = True
    task.replace_identical = True
    task.prompt = False
    task.selected = False
    task.exporter = unreal.SkeletalMeshExporterFBX()
    options = unreal.FbxExportOption()
    for name, value in {
        "ascii": False,
        "force_front_x_axis": False,
        "vertex_color": True,
        "level_of_detail": False,
        "export_morph_targets": False,
        "export_preview_mesh": False,
        "map_skeletal_motion_to_root": False,
    }.items():
        try:
            options.set_editor_property(name, value)
        except Exception:
            pass
    try:
        options.set_editor_property("bake_material_inputs",
                                    unreal.FbxMaterialBakeMode.DISABLED)
    except Exception:
        pass
    task.options = options
    ok = unreal.Exporter.run_asset_export_task(task)
    unreal.log("[VL Manny LOD1] export ok=%r" % ok)
    unreal.SystemLibrary.quit_editor()


main()

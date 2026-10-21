"""Audit (and optionally export) GASP traversal AnimSequences in an isolated UE copy.

Run with UE 5.8's PythonScript commandlet. The audit is read-only; FBX export uses
the same no-bake, no-preview-mesh policy as audit_gasp.py and writes only to a new
directory outside the isolated project.

Environment:
  VL_GASP_TRAVERSAL_OUTPUT  new output directory
  VL_GASP_TRAVERSAL_CLIPS   optional comma-separated AnimSequence names to export
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import unreal

ROOT = "/Game/Characters/UEFN_Mannequin/Animations/Traversal"


def fbx_options():
    options = unreal.FbxExportOption()
    for name, value in {
        "ascii": False, "force_front_x_axis": False, "vertex_color": True,
        "level_of_detail": False, "export_morph_targets": False,
        "export_preview_mesh": False, "map_skeletal_motion_to_root": False,
    }.items():
        options.set_editor_property(name, value)
    options.set_editor_property("bake_material_inputs", unreal.FbxMaterialBakeMode.DISABLED)
    return options


def export_file(asset, path, exporter, options=None):
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = str(path)
    task.automated = True
    task.replace_identical = False
    task.prompt = False
    task.exporter = exporter
    if options is not None:
        task.options = options
    result = unreal.Exporter.run_asset_export_task(task)
    return {"file": path.name, "success": bool(result),
            "errors": [str(error) for error in task.errors], "bytes": path.stat().st_size if path.exists() else 0}


def main():
    output = Path(os.environ["VL_GASP_TRAVERSAL_OUTPUT"]).resolve()
    project = Path(unreal.Paths.project_dir()).resolve()
    if output.is_relative_to(project):
        raise RuntimeError("Export output must be outside the isolated UE project")
    if output.exists():
        raise RuntimeError("Choose a new output directory")
    output.mkdir(parents=True)
    selected = {name.strip() for name in os.environ.get("VL_GASP_TRAVERSAL_CLIPS", "").split(",") if name.strip()}
    report = {"root": ROOT, "engine": unreal.SystemLibrary.get_engine_version(),
              "project": str(project), "clips": [], "errors": []}
    try:
        assets = sorted(unreal.EditorAssetLibrary.list_assets(ROOT, recursive=True, include_folder=False))
        for path in assets:
            asset = unreal.load_asset(path)
            if not isinstance(asset, unreal.AnimSequence):
                continue
            name = asset.get_name()
            library = unreal.AnimationLibrary
            record = {
                "name": name, "package": asset.get_path_name(),
                "duration": float(library.get_sequence_length(asset)),
                "numFrames": int(library.get_num_frames(asset)),
                "numKeys": int(library.get_num_keys(asset)),
                "rateScale": float(library.get_rate_scale(asset)),
                "enableRootMotion": bool(asset.get_editor_property("enable_root_motion")),
                "forceRootLock": bool(asset.get_editor_property("force_root_lock")),
                "rootMotionRootLock": str(asset.get_editor_property("root_motion_root_lock")),
                "curves": [str(value) for value in
                           library.get_animation_curve_names(asset, unreal.RawCurveTrackTypes.RCT_FLOAT)],
                "notifyTracks": [str(value) for value in
                                 library.get_animation_notify_track_names(asset)],
            }
            if name in selected:
                record["fbx"] = export_file(asset, output / (name + ".fbx"),
                                             unreal.AnimSequenceExporterFBX(), fbx_options())
            report["clips"].append(record)
            unreal.log("VL_TRAVERSAL " + json.dumps(record, ensure_ascii=False))
        report["status"] = "complete"
    except Exception as error:
        report["status"] = "failed"
        report["errors"].append(str(error))
        raise
    finally:
        (output / "traversal-audit.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


main()

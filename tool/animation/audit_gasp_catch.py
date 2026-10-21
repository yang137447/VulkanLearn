"""Read-only UE 5.8 enumeration of the GASP Catch traversal AnimMontages.

Run inside the *artifact copy* of the UE project with UE's PythonScript
commandlet; never point this at the source project.  The script only loads
assets and reads properties -- it never saves a package, upgrades the project or
mutates an asset.  The T3D text export and the FBX exports are written to new
paths outside the UE project.

Environment:
  VL_GASP_CATCH_OUTPUT    new JSON report path outside the UE project
  VL_GASP_CATCH_T3D_DIR   directory for the AM_*.copy text exports
  VL_GASP_CATCH_FBX_DIR   directory for the referenced AnimSequence FBX exports

The montage -> AnimSequence reference is recovered from the T3D text that UE
itself produced (``AnimReference=`` / ``LinkedSequence=``), so a sequence name is
never guessed from the montage name.  Every referenced sequence is then loaded
and summarised; a missing or non-AnimSequence asset is recorded as a failure
instead of being skipped, which is what decides whether a montage is usable.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import traceback

import unreal

ANIMS = "/Game/Characters/UEFN_Mannequin/Animations"
PREFIX = "AM_M_Neutral_Traversal_Catch_"
SEQUENCE_PATTERN = re.compile(r"/Script/Engine\.AnimSequence'([^']+)'")


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def serialize(value):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, unreal.Object):
        return {"path": value.get_path_name(), "class": value.get_class().get_name()}
    if isinstance(value, unreal.StructBase):
        return {"ueStructText": value.export_text()}
    if isinstance(value, (unreal.Array, list, tuple)):
        return [serialize(item) for item in value]
    return str(value)


def properties(obj, names):
    result = {}
    for name in names:
        try:
            result[name] = serialize(obj.get_editor_property(name))
        except Exception as error:
            result[name] = {"unavailable": str(error)}
    return result


def fbx_options():
    """Same no-bake, no-preview-mesh policy as the existing GASP FBX exports."""
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
            "errors": [str(error) for error in task.errors],
            "bytes": path.stat().st_size if path.exists() else 0}


def montage_assets():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.search_all_assets(True)
    found = []
    for asset in registry.get_assets_by_path(ANIMS, recursive=True):
        if str(asset.asset_class_path.asset_name) != "AnimMontage":
            continue
        name = str(asset.asset_name)
        if not name.startswith(PREFIX):
            continue
        found.append((name, str(asset.package_name) + "." + name))
    return sorted(found)


def sequence_record(package_path, fbx_dir):
    """Load one referenced AnimSequence and return its summary."""
    asset = unreal.load_asset(package_path)
    if asset is None:
        raise RuntimeError("AnimSequence asset is missing: " + package_path)
    if not isinstance(asset, unreal.AnimSequence):
        raise RuntimeError("Referenced asset is not an AnimSequence: " + package_path)
    library = unreal.AnimationLibrary
    name = asset.get_name()
    record = {
        "name": str(name),
        "package": asset.get_path_name(),
        "duration": float(library.get_sequence_length(asset)),
        "numFrames": int(library.get_num_frames(asset)),
        "numKeys": int(library.get_num_keys(asset)),
        "rateScale": float(library.get_rate_scale(asset)),
        "skeleton": serialize(asset.get_editor_property("skeleton")),
        "curves": [str(value) for value in
                   library.get_animation_curve_names(asset, unreal.RawCurveTrackTypes.RCT_FLOAT)],
        "notifyTracks": [str(value) for value in
                         library.get_animation_notify_track_names(asset)],
    }
    if fbx_dir is not None:
        target = fbx_dir / (str(name) + ".fbx")
        if target.exists():
            raise RuntimeError("Refusing to overwrite existing FBX: " + str(target))
        record["fbx"] = export_file(asset, target, unreal.AnimSequenceExporterFBX(), fbx_options())
    return record


def main():
    project = Path(unreal.Paths.project_dir()).resolve()
    output = Path(os.environ["VL_GASP_CATCH_OUTPUT"]).resolve()
    t3d_dir = Path(os.environ["VL_GASP_CATCH_T3D_DIR"]).resolve()
    fbx_dir = Path(os.environ["VL_GASP_CATCH_FBX_DIR"]).resolve()
    for path in (output, t3d_dir, fbx_dir):
        if path == project or project in path.parents:
            raise RuntimeError("Catch export paths must be outside the UE project: " + str(path))
    if output.exists():
        raise RuntimeError("Catch report already exists: " + str(output))
    t3d_dir.mkdir(parents=True, exist_ok=True)
    fbx_dir.mkdir(parents=True, exist_ok=True)

    report = {
        "format": "vl.gasp.ue-catch-audit.v1",
        "engine": unreal.SystemLibrary.get_engine_version(),
        "project": str(project),
        "root": ANIMS,
        "prefix": PREFIX,
        "status": "incomplete",
        "errors": [],
        "montages": [],
        "sequences": [],
        "scope": "Read-only asset properties + T3D/FBX export; no save, no AnimBlueprint execution.",
    }
    try:
        assets = montage_assets()
        if not assets:
            raise RuntimeError("No AnimMontage asset matches " + PREFIX + " under " + ANIMS)
        by_sequence = {}
        for name, package in assets:
            entry = {"name": name, "package": package}
            try:
                montage = unreal.load_asset(package)
                if not isinstance(montage, unreal.AnimMontage):
                    raise RuntimeError("Asset is not an AnimMontage: " + package)
                # UE 5.8 把部分 Montage 属性留在 protected 区；读取失败时记录
                # unavailable，并用随后导出的 T3D 文本作为同一份资产的证据。
                entry["properties"] = properties(montage, (
                    "sequence_length", "slot_anim_tracks", "composite_sections",
                    "notifies", "anim_notify_tracks", "skeleton", "preview_base_pose"))
                entry["t3d"] = export_file(
                    montage, t3d_dir / (name + ".copy"), unreal.ObjectExporterT3D())
                text = (t3d_dir / (name + ".copy")).read_text(encoding="utf-8", errors="replace")
                references = sorted({match for match in SEQUENCE_PATTERN.findall(text)})
                entry["sequenceReferences"] = references
                if not references:
                    raise RuntimeError("T3D lists no AnimSequence reference: " + name)
                entry["slotNames"] = sorted(set(re.findall(r'SlotName="([^"]*)"', text)))
                entry["sectionCount"] = len(re.findall(r"(?m)^[ \t]*CompositeSections\(\d+\)=", text))
                entry["notifyCount"] = len(re.findall(r"(?m)^[ \t]*Notifies\(\d+\)=", text))
                length = re.search(r"(?m)^[ \t]*SequenceLength=(-?[0-9]+(?:\.[0-9]+)?)", text)
                entry["sequenceLength"] = float(length.group(1)) if length else None
                for reference in references:
                    if reference not in by_sequence:
                        try:
                            by_sequence[reference] = sequence_record(reference, fbx_dir)
                            unreal.log("[VL GASP Catch] Sequence exported " + reference)
                        except Exception:
                            by_sequence[reference] = {
                                "package": reference, "status": "failed",
                                "error": traceback.format_exc()}
                            report["errors"].append({"sequence": reference,
                                                     "error": by_sequence[reference]["error"]})
                missing = [item for item in references
                           if by_sequence[item].get("status") == "failed"]
                entry["status"] = "usable" if not missing else "unusable"
                if missing:
                    entry["missingSequences"] = missing
            except Exception:
                entry["status"] = "failed"
                entry["error"] = traceback.format_exc()
                report["errors"].append({"montage": name, "error": entry["error"]})
            report["montages"].append(entry)
            unreal.log("[VL GASP Catch] Montage %s -> %s" % (name, entry["status"]))
        report["sequences"] = [by_sequence[key] for key in sorted(by_sequence)]
        report["status"] = "complete"
    except Exception:
        report["errors"].append({"error": traceback.format_exc()})
    finally:
        write_json(output, report)
    if report["status"] != "complete":
        raise RuntimeError("Catch audit incomplete; see " + str(output))
    unreal.log("[VL GASP Catch] Complete: " + str(output))


if __name__ == "__main__":
    main()

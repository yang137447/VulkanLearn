"""Read-only UE 5.8 export of the GASP skeleton, curves and notifies.

Run inside the *artifact copy* of the UE project with UE's PythonScript
commandlet; never point this at the source project.  The script only loads
assets and reads properties -- it never saves a package, upgrades the project or
mutates an asset.

Environment:
  VL_GASP_ANIM_CLIPS   path to Maps/SC_gasp/Meshes/SM_mannequin.json; its
                       skinning.clips[].id list selects the clips.
  VL_GASP_ANIM_OUTPUT  new output file path outside the UE project.
  VL_GASP_ANIM_CLIP_IDS
                       optional comma-separated clip ids that replaces the SM
                       list.  Needed when a new clip family (the Catch export)
                       is dumped before ``SM_mannequin.json`` lists it; the
                       ``clipSource`` path is still recorded for provenance.

The JSON is an intermediate audit dump (``vl.gasp.ue-animation-raw.v1``); the
runtime sidecars are produced from it by ``export_gasp_sequences.py``.  Keeping
the UE step separate makes the schema conversion testable without a commandlet
run and keeps BLAKE3/JSON formatting in one place.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import traceback

import unreal

ROOT = "/Game/Characters/UEFN_Mannequin"
ANIMS = ROOT + "/Animations"
MESH = ROOT + "/Meshes/SKM_UEFN_Mannequin"
OUTPUT_FORMAT = "vl.gasp.ue-animation-raw.v1"


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def vector(value):
    return [float(value.x), float(value.y), float(value.z)]


def transform(value):
    rotation = value.rotation
    return {
        "translation": vector(value.translation),
        "rotationXyzw": [float(rotation.x), float(rotation.y),
                         float(rotation.z), float(rotation.w)],
        "scale": vector(value.scale3d),
    }


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


def skeleton_record(mesh, first_animation):
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property("evaluation_type", unreal.AnimDataEvalType.RAW)
    options.set_editor_property("should_retarget", False)
    options.set_editor_property("extract_root_motion", False)
    options.set_editor_property("incorporate_root_motion_into_pose", True)
    options.set_editor_property("optional_skeletal_mesh", mesh)
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(first_animation, 0.0, options)
    if not unreal.AnimPoseExtensions.is_valid(pose):
        raise RuntimeError("UE returned an invalid reference pose")
    bones = []
    for name in unreal.AnimPoseExtensions.get_bone_names(pose):
        parent = unreal.SkeletalMeshEditorSubsystem.get_bone_parent(mesh, name)
        bones.append({
            "name": str(name),
            "parent": str(parent),
            "referenceLocal": transform(unreal.AnimPoseExtensions.get_ref_bone_pose(
                pose, name, unreal.AnimPoseSpaces.LOCAL)),
            "referenceModel": transform(unreal.AnimPoseExtensions.get_ref_bone_pose(
                pose, name, unreal.AnimPoseSpaces.WORLD)),
        })
    skeleton_asset = mesh.get_editor_property("skeleton")
    slots = None
    for property_name in ("slots", "slot_names"):
        try:
            value = skeleton_asset.get_editor_property(property_name)
            slots = [str(item) for item in value]
            break
        except Exception:
            continue
    return {
        "mesh": mesh.get_path_name(),
        "skeleton": serialize(skeleton_asset),
        "slots": slots,
        "bones": bones,
        "units": "UE centimeters; unconverted",
    }


def frame_rate_record(animation, duration, num_frames):
    try:
        rate = unreal.AnimationLibrary.get_sampling_frame_rate(animation)
        numerator = int(rate.numerator)
        denominator = int(rate.denominator)
        if numerator > 0 and denominator > 0:
            return {"numerator": numerator, "denominator": denominator, "source": "ue"}
    except Exception:
        pass
    # GASP is authored at 30 Hz; derive defensively and reject non-integral rates.
    if duration <= 0 or num_frames <= 0:
        return None
    numerator = int(round(num_frames / duration))
    if abs(num_frames / duration - numerator) > 1e-3:
        return None
    return {"numerator": numerator, "denominator": 1, "source": "numFrames/duration"}


def sync_marker_record(markers):
    result = []
    for marker in markers:
        if isinstance(marker, str):
            result.append({"name": marker})
            continue
        try:
            result.append({
                "name": str(marker.get_editor_property("marker_name")),
                "time": float(marker.get_editor_property("time")),
            })
        except Exception:
            result.append(serialize(marker))
    return result


def clip_record(animation):
    library = unreal.AnimationLibrary
    duration = float(library.get_sequence_length(animation))
    num_frames = int(library.get_num_frames(animation))
    record = {
        "name": str(animation.get_name()),
        "package": animation.get_path_name(),
        "duration": duration,
        "numFrames": num_frames,
        "numKeys": int(library.get_num_keys(animation)),
        "rateScale": float(library.get_rate_scale(animation)),
        "frameRate": frame_rate_record(animation, duration, num_frames),
        "settings": properties(animation, (
            "skeleton", "enable_root_motion", "root_motion_root_lock",
            "force_root_lock", "additive_anim_type", "retarget_source")),
        "tracks": [str(name) for name in library.get_animation_track_names(animation)],
        "curves": [],
        "notifies": [],
        "syncMarkers": sync_marker_record(library.get_animation_sync_markers(animation)),
    }
    for name in library.get_animation_curve_names(animation, unreal.RawCurveTrackTypes.RCT_FLOAT):
        times, values = library.get_float_keys(animation, name)
        record["curves"].append({
            "name": str(name),
            "times": [float(value) for value in times],
            "values": [float(value) for value in values],
        })
    # UE 5.8 keeps AnimSequence.Notifies protected; the public per-track query is
    # the only read path.  Track names are kept so the converter can emit a real
    # track name instead of a synthetic one.
    for track in library.get_animation_notify_track_names(animation):
        for event in library.get_animation_notify_events_for_track(animation, track):
            record["notifies"].append({
                "track": str(track),
                "time": float(library.get_anim_notify_event_trigger_time(event)),
                "duration": float(library.get_anim_notify_event_duration(event)),
                "text": serialize(event),
            })
    return record


def find_animation_assets():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.search_all_assets(True)
    by_name = {}
    for asset in registry.get_assets_by_path(ANIMS, recursive=True):
        if str(asset.asset_class_path.asset_name) != "AnimSequence":
            continue
        name = str(asset.asset_name)
        package = str(asset.package_name) + "." + name
        by_name.setdefault(name, []).append(package)
    return by_name


def main():
    project = Path(unreal.Paths.project_dir()).resolve()
    clip_source = Path(os.environ["VL_GASP_ANIM_CLIPS"]).resolve()
    output = Path(os.environ["VL_GASP_ANIM_OUTPUT"]).resolve()
    if output == project or project in output.parents:
        raise RuntimeError("Animation export output must be outside the UE project")
    if output.exists():
        raise RuntimeError("Animation export output already exists: " + str(output))
    selected = json.loads(clip_source.read_text(encoding="utf-8"))["skinning"]["clips"]
    clip_ids = [str(item["id"]) for item in selected]
    explicit = os.environ.get("VL_GASP_ANIM_CLIP_IDS", "")
    explicit_ids = [item.strip() for item in explicit.split(",") if item.strip()]
    if explicit_ids:
        if len(set(explicit_ids)) != len(explicit_ids):
            raise RuntimeError("VL_GASP_ANIM_CLIP_IDS contains duplicates")
        clip_ids = explicit_ids
    report = {
        "format": OUTPUT_FORMAT,
        "engine": unreal.SystemLibrary.get_engine_version(),
        "project": str(project),
        "mesh": MESH,
        "clipSource": str(clip_source),
        "explicitClipIds": explicit_ids or None,
        "status": "incomplete",
        "errors": [],
        "skeleton": None,
        "clips": [],
        "scope": "Read-only asset properties; no AnimBlueprint execution or save.",
    }
    try:
        mesh = unreal.load_asset(MESH)
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError("Mesh is missing or not a SkeletalMesh: " + MESH)
        by_name = find_animation_assets()
        missing = [name for name in clip_ids if name not in by_name]
        if missing:
            raise RuntimeError("AnimSequence asset(s) missing: " + ", ".join(missing))
        duplicate = {name: by_name[name] for name in clip_ids if len(by_name[name]) != 1}
        if duplicate:
            raise RuntimeError("Ambiguous AnimSequence asset(s): " + json.dumps(duplicate))
        first_animation = unreal.load_asset(by_name[clip_ids[0]][0])
        report["skeleton"] = skeleton_record(mesh, first_animation)
        for clip_id in clip_ids:
            try:
                animation = unreal.load_asset(by_name[clip_id][0])
                if not isinstance(animation, unreal.AnimSequence):
                    raise RuntimeError("Not an AnimSequence: " + by_name[clip_id][0])
                report["clips"].append(clip_record(animation))
                unreal.log("[VL GASP Anim] Exported " + clip_id)
            except Exception:
                error = traceback.format_exc()
                report["errors"].append({"clip": clip_id, "error": error})
                unreal.log_error("[VL GASP Anim] " + clip_id + "\n" + error)
        if not report["errors"]:
            report["status"] = "complete"
    except Exception:
        report["errors"].append({"error": traceback.format_exc()})
    finally:
        write_json(output, report)
    if report["status"] != "complete":
        raise RuntimeError("Animation raw export incomplete; see " + str(output))
    unreal.log("[VL GASP Anim] Complete: " + str(output))


if __name__ == "__main__":
    main()

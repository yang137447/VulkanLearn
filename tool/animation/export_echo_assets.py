"""Read-only UE 5.8 export of the Valley Echo skeleton, mesh and locomotion clips.

Run inside a *copy* of the Valley project with UE's PythonScript commandlet.
The script only loads assets and reads properties; it never saves a package.

Environment:
  VL_ECHO_EXPORT_DIR   new output directory outside the UE project
  VL_ECHO_CLIP_IDS     optional comma-separated AnimSequence names (default: all)
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import traceback

import unreal


ROOT = "/Game/AncientContent/Characters/Echo"
MESH = ROOT + "/Meshes/Echo"
SKELETON = ROOT + "/Meshes/Echo_Skeleton"
ANIMS = ROOT + "/Animations"
OUTPUT_FORMAT = "vl.echo.ue-animation-raw.v1"


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
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
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(
        first_animation, 0.0, options)
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
            slots = [str(item) for item in skeleton_asset.get_editor_property(
                property_name)]
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
            return {"numerator": numerator, "denominator": denominator,
                    "source": "ue"}
    except Exception:
        pass
    if duration <= 0 or num_frames <= 0:
        return None
    numerator = int(round(num_frames / duration))
    if abs(num_frames / duration - numerator) > 1e-3:
        return None
    return {"numerator": numerator, "denominator": 1,
            "source": "numFrames/duration"}


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
        "tracks": [str(name) for name in library.get_animation_track_names(
            animation)],
        "curves": [],
        "notifies": [],
        "syncMarkers": sync_marker_record(
            library.get_animation_sync_markers(animation)),
    }
    for name in library.get_animation_curve_names(
            animation, unreal.RawCurveTrackTypes.RCT_FLOAT):
        times, values = library.get_float_keys(animation, name)
        record["curves"].append({
            "name": str(name),
            "times": [float(value) for value in times],
            "values": [float(value) for value in values],
        })
    for track in library.get_animation_notify_track_names(animation):
        for event in library.get_animation_notify_events_for_track(
                animation, track):
            record["notifies"].append({
                "track": str(track),
                "time": float(library.get_anim_notify_event_trigger_time(event)),
                "duration": float(library.get_anim_notify_event_duration(event)),
                "text": serialize(event),
            })
    return record


def export_fbx(asset, filename: Path) -> None:
    filename.parent.mkdir(parents=True, exist_ok=True)
    task = unreal.AssetExportTask()
    task.set_editor_property("object", asset)
    task.set_editor_property("filename", str(filename))
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_identical", True)
    task.set_editor_property("prompt", False)
    task.set_editor_property("use_file_archive", False)
    if not unreal.Exporter.run_asset_export_task(task):
        raise RuntimeError("FBX export failed: " + str(filename))


def locomotion_assets():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.search_all_assets(True)
    clips = []
    for asset in registry.get_assets_by_path(ANIMS, recursive=False):
        if str(asset.asset_class_path.asset_name) != "AnimSequence":
            continue
        clips.append((str(asset.asset_name), str(asset.package_name)))
    clips.sort(key=lambda item: item[0])
    return clips


def main():
    project = Path(unreal.Paths.project_dir()).resolve()
    output = Path(os.environ["VL_ECHO_EXPORT_DIR"].strip()).resolve()
    if output == project or project in output.parents:
        raise RuntimeError("Export directory must be outside the UE project")
    if output.exists():
        raise RuntimeError("Export directory already exists: " + str(output))

    explicit = os.environ.get("VL_ECHO_CLIP_IDS", "")
    requested = [item.strip() for item in explicit.split(",") if item.strip()]
    clips = locomotion_assets()
    if requested:
        if len(set(requested)) != len(requested):
            raise RuntimeError("VL_ECHO_CLIP_IDS contains duplicates")
        by_name = dict(clips)
        missing = [name for name in requested if name not in by_name]
        if missing:
            raise RuntimeError("Unknown clip ids: " + ", ".join(missing))
        clips = [(name, by_name[name]) for name in requested]

    report = {
        "format": OUTPUT_FORMAT,
        "engine": unreal.SystemLibrary.get_engine_version(),
        "project": str(project),
        "mesh": MESH,
        "skeleton": SKELETON,
        "status": "incomplete",
        "errors": [],
        "exported": {"mesh": "", "clips": []},
        "skeleton": None,
        "clips": [],
    }
    try:
        mesh = unreal.load_asset(MESH)
        skeleton = unreal.load_asset(SKELETON)
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError("Mesh is missing or not a SkeletalMesh: " + MESH)
        if not isinstance(skeleton, unreal.Skeleton):
            raise RuntimeError("Skeleton is missing or not a Skeleton: " +
                               SKELETON)
        if not clips:
            raise RuntimeError("No AnimSequence assets found under " + ANIMS)

        source = output / "Source"
        mesh_fbx = source / (mesh.get_name() + ".fbx")
        export_fbx(mesh, mesh_fbx)
        report["exported"]["mesh"] = str(mesh_fbx)

        first_animation = unreal.load_asset(clips[0][1])
        report["skeleton"] = skeleton_record(mesh, first_animation)
        for name, package in clips:
            animation = unreal.load_asset(package)
            if not isinstance(animation, unreal.AnimSequence):
                raise RuntimeError("Not an AnimSequence: " + package)
            fbx = source / (name + ".fbx")
            export_fbx(animation, fbx)
            record = clip_record(animation)
            record["fbx"] = str(fbx)
            report["clips"].append(record)
            report["exported"]["clips"].append(str(fbx))
            unreal.log("[VL Echo Export] " + name)
        report["status"] = "complete"
    except Exception:
        report["errors"].append(traceback.format_exc())
        unreal.log_error("[VL Echo Export] failed\n" + report["errors"][-1])
    finally:
        write_json(output / "ue-anim-raw.json", report)
    if report["status"] != "complete":
        raise RuntimeError("Echo export incomplete; see ue-anim-raw.json")
    unreal.log("[VL Echo Export] Complete: " + str(output))


if __name__ == "__main__":
    main()

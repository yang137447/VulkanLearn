"""Read-only UE 5.8 export of Echo's three AnimMontages.

Run inside the isolated Valley export copy with UE's PythonScript commandlet;
never point this at the source project.  The script loads Montage and
AnimSequence assets, exports the Montage T3D text and (optionally) the
referenced AnimSequence FBX, and writes a JSON report.  It never calls
asset/project save and never mutates an asset.

The Montage -> AnimSequence pairing is read from the T3D text that UE itself
produced (``AnimReference=`` / ``SlotAnimTracks``), so a sequence name is never
guessed from the Montage name.

Environment:
  VL_ECHO_MONTAGE_OUTPUT    new JSON report path outside the UE project
  VL_ECHO_MONTAGE_T3D_DIR   directory for the Montage T3D exports
  VL_ECHO_MONTAGE_FBX_DIR   optional directory for referenced AnimSequence FBX
  VL_ECHO_MONTAGE_NAMES     optional comma-separated Montage asset names

The default names are the three Echo Montages used by the locomotion work:
``VaultOver_Montage``, ``ReachOut_Montage_RM`` and ``ReachOut_End_Montage``.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import traceback

import unreal


ROOT = "/Game/AncientContent/Characters/Echo"
ANIMS = ROOT + "/Animations"
DEFAULT_MONTAGES = (
    "VaultOver_Montage",
    "ReachOut_Montage_RM",
    "ReachOut_End_Montage",
)
SEQUENCE_PATTERN = re.compile(r"/Script/Engine\.AnimSequence'([^']+)'")


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
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


def export_file(asset, path: Path, exporter, options=None) -> dict:
    """Export one asset.  Existing files are never replaced."""
    if path.exists():
        raise RuntimeError("Refusing to overwrite existing export: " + str(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = str(path)
    task.automated = True
    task.replace_identical = False
    task.prompt = False
    # Match export_echo_assets.py's no-file-archive policy so a fresh FBX can be
    # compared with the committed Source bytes instead of embedding archive state.
    task.use_file_archive = False
    if exporter is not None:
        task.exporter = exporter
    if options is not None:
        task.options = options
    result = unreal.Exporter.run_asset_export_task(task)
    return {
        "file": path.name,
        "success": bool(result) and path.exists() and path.stat().st_size > 0,
        "errors": [str(error) for error in task.errors],
        "bytes": path.stat().st_size if path.exists() else 0,
    }


def requested_names() -> list[str]:
    explicit = os.environ.get("VL_ECHO_MONTAGE_NAMES", "")
    names = [item.strip() for item in explicit.split(",") if item.strip()]
    if not names:
        names = list(DEFAULT_MONTAGES)
    if len(set(names)) != len(names):
        raise RuntimeError("VL_ECHO_MONTAGE_NAMES contains duplicates")
    return names


def t3d_summary(text: str) -> dict:
    """Read the structural evidence from UE's own T3D text.

    The authoritative schema conversion lives in ``import_echo_montages.py``.
    This summary exists to make the audit report checkable without re-parsing
    nested structs: it counts sections/notifies/tracks and records names.
    """
    sequence_length_match = re.search(
        r"(?m)^[ \t]*SequenceLength=(-?[0-9]+(?:\.[0-9]+)?)", text)
    slot_names = sorted(set(re.findall(r'SlotName="([^"]*)"', text)))
    section_names = []
    notify_names = []
    for line in text.splitlines():
        if re.match(r"[ \t]*CompositeSections\(\d+\)=", line):
            match = re.search(r'(?<!Next)SectionName="([^"]*)"', line)
            if match:
                section_names.append(match.group(1))
        notify = re.match(r"[ \t]*Notifies\(\d+\)=\((.*)", line)
        if notify:
            match = re.search(r'NotifyName="([^"]*)"', notify.group(1))
            if match:
                notify_names.append(match.group(1))
    notify_tracks = re.findall(r'TrackName="([^"]*)"', text)
    references = sorted(set(SEQUENCE_PATTERN.findall(text)))
    warp_targets = re.findall(r'WarpTargetName="([^"]*)"', text)
    warp_providers = re.findall(r"WarpPointAnimProvider=([A-Za-z]+)", text)
    warp_bones = re.findall(r'WarpPointAnimBoneName="([^"]*)"', text)
    link_methods = sorted(set(re.findall(r'LinkMethod="([^"]*)"', text)))
    sequence_segments = len(re.findall(r"\bAnimReference=", text))
    notify_state_classes = []
    unresolved = []
    for line in text.splitlines():
        event = re.match(r"[ \t]*Notifies\((\d+)\)=\((.*)", line)
        if not event:
            continue
        body = event.group(2)
        name_match = re.search(r'NotifyName="([^"]*)"', body)
        if not name_match:
            continue
        name = name_match.group(1)
        state_match = re.search(
            r"NotifyStateClass=(None|\"[^\"]*\"|'[^']*')", body)
        state_class = state_match.group(1) if state_match else None
        if state_class not in (None, "None"):
            notify_state_classes.append(state_class)
        elif state_class is None and "Notify=" not in body:
            duration_match = re.search(r"\bDuration=(-?[0-9.]+)", body)
            duration = float(duration_match.group(1)) if duration_match else 0.0
            if duration > 0 or "NotifyState" in name:
                unresolved.append(name)
    return {
        "sequenceLength": float(sequence_length_match.group(1))
        if sequence_length_match else None,
        "sectionCount": len(re.findall(r"(?m)^[ \t]*CompositeSections\(\d+\)=", text)),
        "sectionNames": section_names,
        "notifyCount": len(re.findall(r"(?m)^[ \t]*Notifies\(\d+\)=", text)),
        "notifyNames": notify_names,
        "notifyTracks": sorted(set(notify_tracks)),
        "slotNames": slot_names,
        "sequenceReferences": references,
        "sequenceSegmentCount": sequence_segments,
        "multipleSequences": sequence_segments > 1,
        "warpTargets": warp_targets,
        "warpProviders": sorted(set(warp_providers)),
        "warpBones": [bone for bone in warp_bones if bone],
        "linkMethods": link_methods,
        "hasMotionWarpingModifier": "AnimNotifyState_MotionWarping" in text,
        "notifyStateClasses": notify_state_classes,
        "unresolvedNotifyClasses": unresolved,
    }


def notify_events(montage) -> tuple[list[dict], list[str]]:
    """Read the notify structs through UE's Python API.

    T3D can omit ``NotifyStateClass`` when the class asset is missing from the
    isolated content copy.  The Python API returns the same fields, so the
    audit can refuse to classify such an event as an instant notify.
    """
    library = unreal.AnimationLibrary
    events = []
    warnings = []
    try:
        tracks = [str(value) for value in
                  library.get_animation_notify_track_names(montage)]
    except Exception as error:
        return events, ["notify track query failed: " + str(error)]
    for track in tracks:
        try:
            values = library.get_animation_notify_events_for_track(montage, track)
        except Exception as error:
            warnings.append("notify query failed on track %s: %s" % (track, error))
            continue
        for value in values:
            text = value.export_text()
            name = re.search(r'NotifyName="([^"]*)"', text)
            state = re.search(
                r"NotifyStateClass=(None|\"[^\"]*\"|'[^']*')", text)
            notify = re.search(r"Notify=(None|\"[^\"]*\"|'[^']*')", text)
            state_class = state.group(1) if state else None
            notify_class = notify.group(1) if notify else None
            if state_class not in (None, "None"):
                kind = "notifyState"
            elif notify_class is not None and state_class in (None, "None"):
                kind = "notify"
            else:
                kind = "unknown"
                warnings.append("notify %r has neither a class nor a notify object"
                                % (name.group(1) if name else "?"))
            try:
                time = float(library.get_anim_notify_event_trigger_time(value))
                duration = float(library.get_anim_notify_event_duration(value))
            except Exception as error:
                warnings.append("notify timing query failed: " + str(error))
                time = None
                duration = None
            events.append({
                "track": track,
                "name": name.group(1) if name else "",
                "kind": kind,
                "class": state_class if state_class not in (None, "None")
                else notify_class,
                "time": time,
                "duration": duration,
            })
    return events, warnings


def montage_python_summary(montage) -> dict:
    library = unreal.AnimationLibrary
    result = {"slots": [], "sections": [], "notifyEvents": [], "warnings": []}
    try:
        result["slots"] = [str(value) for value in
                           library.get_montage_slot_names(montage)]
    except Exception as error:
        result["warnings"].append("slot query failed: " + str(error))
    try:
        result["sections"] = [str(montage.get_section_name(index))
                              for index in range(montage.get_num_sections())]
    except Exception as error:
        result["warnings"].append("section query failed: " + str(error))
    events, warnings = notify_events(montage)
    result["notifyEvents"] = events
    result["warnings"].extend(warnings)
    return result


def sequence_record(package: str, fbx_dir: Path | None) -> dict:
    asset = unreal.load_asset(package)
    if asset is None:
        raise RuntimeError("AnimSequence asset is missing: " + package)
    if not isinstance(asset, unreal.AnimSequence):
        raise RuntimeError("Referenced asset is not an AnimSequence: " + package)
    library = unreal.AnimationLibrary
    record = {
        "name": str(asset.get_name()),
        "package": asset.get_path_name(),
        "duration": float(library.get_sequence_length(asset)),
        "numFrames": int(library.get_num_frames(asset)),
        "numKeys": int(library.get_num_keys(asset)),
        "rateScale": float(library.get_rate_scale(asset)),
        "skeleton": serialize(asset.get_editor_property("skeleton")),
        "curveCount": len(library.get_animation_curve_names(
            asset, unreal.RawCurveTrackTypes.RCT_FLOAT)),
        "notifyTracks": [str(value) for value in
                         library.get_animation_notify_track_names(asset)],
        "syncMarkers": serialize(library.get_animation_sync_markers(asset)),
    }
    if fbx_dir is not None:
        # Match export_echo_assets.py's default exporter/options so the new
        # bytes can be compared with the committed resource FBX.
        record["fbx"] = export_file(
            asset, fbx_dir / (str(asset.get_name()) + ".fbx"), None)
    return record


def main():
    project = Path(unreal.Paths.project_dir()).resolve()
    output = Path(os.environ["VL_ECHO_MONTAGE_OUTPUT"].strip()).resolve()
    t3d_dir = Path(os.environ["VL_ECHO_MONTAGE_T3D_DIR"].strip()).resolve()
    fbx_setting = os.environ.get("VL_ECHO_MONTAGE_FBX_DIR", "").strip()
    fbx_dir = Path(fbx_setting).resolve() if fbx_setting else None
    for path in (output, t3d_dir):
        if path == project or project in path.parents:
            raise RuntimeError("Montage export paths must be outside the UE project: "
                               + str(path))
    if fbx_dir is not None and (fbx_dir == project or project in fbx_dir.parents):
        raise RuntimeError("Montage FBX directory must be outside the UE project: "
                           + str(fbx_dir))
    if output.exists():
        raise RuntimeError("Montage report already exists: " + str(output))
    t3d_dir.mkdir(parents=True, exist_ok=True)
    if fbx_dir is not None:
        fbx_dir.mkdir(parents=True, exist_ok=True)

    names = requested_names()
    report = {
        "format": "vl.echo.montage-audit.v1",
        "engine": unreal.SystemLibrary.get_engine_version(),
        "project": str(project),
        "root": ANIMS,
        "requested": names,
        "status": "incomplete",
        "errors": [],
        "montages": [],
        "sequences": [],
        "scope": ("Read-only asset properties + T3D/FBX export; no save, "
                  "no AnimBlueprint execution, no layered runtime."),
    }
    by_sequence = {}
    try:
        for name in names:
            package = ANIMS + "/" + name
            entry = {"name": name, "package": package}
            try:
                montage = unreal.load_asset(package)
                if montage is None:
                    raise RuntimeError("AnimMontage asset is missing: " + package)
                if not isinstance(montage, unreal.AnimMontage):
                    raise RuntimeError("Asset is not an AnimMontage: " + package)
                entry["class"] = montage.get_class().get_name()
                # UE 5.8 keeps some Montage fields protected; T3D remains the
                # same-asset evidence when a direct property read fails.
                entry["properties"] = properties(montage, (
                    "sequence_length", "slot_anim_tracks", "composite_sections",
                    "notifies", "anim_notify_tracks", "skeleton"))
                t3d_path = t3d_dir / (name + ".t3d")
                entry["t3d"] = export_file(
                    montage, t3d_path, unreal.ObjectExporterT3D())
                if not entry["t3d"]["success"]:
                    raise RuntimeError("T3D export failed: " + str(t3d_path))

                text = t3d_path.read_text(encoding="utf-8", errors="replace")
                entry["summary"] = t3d_summary(text)
                entry["python"] = montage_python_summary(montage)
                references = entry["summary"]["sequenceReferences"]
                if not references:
                    raise RuntimeError("T3D lists no AnimSequence reference: " + name)
                # Multiple AnimSegments are now represented by vl.anim-montage.v1
                # segments[]; keep the audit report focused on data usability.
                if entry["summary"]["unresolvedNotifyClasses"]:
                    entry["status"] = "warning"
                    entry["warnings"] = [
                        "notify class is absent from the isolated content copy: "
                        + ", ".join(entry["summary"]["unresolvedNotifyClasses"])]
                else:
                    entry["status"] = "usable"
                for reference in references:
                    if reference not in by_sequence:
                        try:
                            by_sequence[reference] = sequence_record(reference, fbx_dir)
                            unreal.log("[VL Echo Montage] Sequence exported "
                                       + reference)
                        except Exception:
                            by_sequence[reference] = {
                                "package": reference,
                                "status": "failed",
                                "error": traceback.format_exc(),
                            }
                            report["errors"].append({
                                "sequence": reference,
                                "error": by_sequence[reference]["error"],
                            })
                missing = [item for item in references
                           if by_sequence[item].get("status") == "failed"]
                if missing:
                    entry["status"] = "unusable"
                    entry["missingSequences"] = missing
            except Exception:
                entry["status"] = "failed"
                entry["error"] = traceback.format_exc()
                report["errors"].append({"montage": name, "error": entry["error"]})
            report["montages"].append(entry)
            unreal.log("[VL Echo Montage] %s -> %s"
                       % (name, entry["status"]))
        report["sequences"] = [by_sequence[key] for key in sorted(by_sequence)]
        report["status"] = "complete"
    except Exception:
        report["errors"].append({"error": traceback.format_exc()})
    finally:
        write_json(output, report)
    if report["status"] != "complete":
        raise RuntimeError("Echo montage audit incomplete; see " + str(output))
    unreal.log("[VL Echo Montage] Complete: " + str(output))


if __name__ == "__main__":
    main()

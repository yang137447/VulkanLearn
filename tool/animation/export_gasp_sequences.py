#!/usr/bin/env python3
"""Export ``<FBX stem>.anim.json`` Sequence sidecars for the GASP clips.

Inputs:
  * the UE raw dump produced by ``export_gasp_ue_raw.py`` (complete float curve
    keys, notify names/times, frame counts, root-motion settings);
  * ``Skeletons/SK_UEFN_Mannequin.json`` for the skeleton id, BLAKE3 and curve
    registry;
  * ``Meshes/SM_mannequin.json`` for the clip list (read-only).

Outputs, next to each FBX:
  Maps/SC_gasp/Source/<stem>.anim.json

``--clips`` restricts the batch to a comma-separated subset of the SM clip ids
(the Catch export dumps only the new family while the other sidecars stay
untouched).  Without it every SM clip is exported and the UE raw dump must match
the SM clip list exactly, which is the historical gate.

The exporter is also a gate: it verifies the Hurdle ``distance_from_ledge``
linear-clamp samples at the BackLedge/BackFloor window ends against the UE
reference values (34.098671 / 90.908951 cm, tolerance 0.5 cm) and recomputes
``dOut = 56.810280``.  The check only runs when the Hurdle clip is in the batch;
a mismatch aborts before any file is written.

Notify policy (decided 2026-10-20): a ``notifyState`` may legitimately extend
past ``SequenceLength`` when the sequence was trimmed (UE keeps the authored
``Duration``).  The sidecar therefore preserves the UE ``time``/``duration``
verbatim; it does not clamp ``time + duration`` to the sequence duration.

Re-running is refused unless ``--force`` is passed.
"""

from __future__ import annotations

import argparse
import bisect
import json
import math
import os
import sys
from fractions import Fraction
from pathlib import Path

from gasp_anim_common import (
    blake3_file,
    enum_token,
    load_json,
    preflight_targets,
    resource_relative,
    write_json,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RESOURCE_ROOT = REPO_ROOT.parent / "VukanLearnResources"
DEFAULT_RAW = REPO_ROOT / "artifacts/anim-sequence-export/ue-anim-raw.json"
MESH_RELATIVE = "Maps/SC_gasp/Meshes/SM_mannequin.json"
SKELETON_RELATIVE = "Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json"
HURDLE_CLIP = "M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot"
HURDLE_CURVE = "distance_from_ledge"
HURDLE_WINDOW_ENDS = (0.598017, 0.831335)
HURDLE_EXPECTED_VALUES = (34.098671, 90.908951)
HURDLE_EXPECTED_DOUT = 56.810280
HURDLE_VALUE_TOLERANCE_CM = 0.5

ROOT_LOCK_NAMES = {
    "REF_POSE": "RefPose",
    "ANIM_FIRST_FRAME": "AnimFirstFrame",
    "ZERO": "Zero",
}
ADDITIVE_NAMES = {
    "AAT_NONE": "None",
    "AAT_LOCAL_SPACE_BASE": "LocalSpaceBase",
    "AAT_ROTATION_OFFSET_MESH_SPACE": "RotationOffsetMeshSpace",
}


class ExportError(RuntimeError):
    pass


def linear_sample(curve: dict, time: float) -> float:
    times = curve["times"]
    values = curve["values"]
    index = bisect.bisect_right(times, time)
    if index == 0:
        return values[0]
    if index >= len(times):
        return values[-1]
    left_time, right_time = times[index - 1], times[index]
    if right_time == left_time:
        return values[index - 1]
    ratio = (time - left_time) / (right_time - left_time)
    return values[index - 1] + (values[index] - values[index - 1]) * ratio


def find_curve(curves: list[dict], name: str) -> dict | None:
    key = name.casefold()
    matches = [curve for curve in curves if curve["name"].casefold() == key]
    if len(matches) > 1:
        raise ExportError("Duplicate (case-insensitive) curve %r" % name)
    return matches[0] if matches else None


def validate_settings(raw_clip: dict) -> dict:
    settings = raw_clip.get("settings")
    if not isinstance(settings, dict):
        raise ExportError("Clip %r has no settings" % raw_clip.get("name"))
    for key in ("enable_root_motion", "force_root_lock", "root_motion_root_lock",
                "additive_anim_type"):
        value = settings.get(key)
        if isinstance(value, dict) and "unavailable" in value:
            raise ExportError("Clip %r setting %s is unavailable: %s"
                              % (raw_clip.get("name"), key, value["unavailable"]))
    enable = settings.get("enable_root_motion")
    force = settings.get("force_root_lock")
    if not isinstance(enable, bool) or not isinstance(force, bool):
        raise ExportError("Clip %r root-motion booleans are not booleans" % raw_clip.get("name"))
    lock_token = enum_token(settings.get("root_motion_root_lock"))
    additive_token = enum_token(settings.get("additive_anim_type"))
    if lock_token not in ROOT_LOCK_NAMES:
        raise ExportError("Clip %r has unknown RootMotionRootLock %r"
                          % (raw_clip.get("name"), lock_token))
    if additive_token not in ADDITIVE_NAMES:
        raise ExportError("Clip %r has unknown AdditiveAnimType %r"
                          % (raw_clip.get("name"), additive_token))
    return {
        "enableRootMotion": enable,
        "forceRootLock": force,
        "rootMotionRootLock": ROOT_LOCK_NAMES[lock_token],
        "additiveAnimType": ADDITIVE_NAMES[additive_token],
    }


def frame_rate_record(raw_clip: dict) -> dict:
    duration = raw_clip["duration"]
    num_frames = raw_clip["numFrames"]
    if duration <= 0 or num_frames <= 0:
        raise ExportError("Clip %r has invalid duration/frame count" % raw_clip.get("name"))
    raw_rate = raw_clip.get("frameRate")
    if isinstance(raw_rate, dict) and "numerator" in raw_rate and "denominator" in raw_rate:
        numerator = int(raw_rate["numerator"])
        denominator = int(raw_rate["denominator"])
    else:
        fraction = Fraction(num_frames / duration).limit_denominator(1001)
        numerator, denominator = fraction.numerator, fraction.denominator
    if numerator <= 0 or denominator <= 0:
        raise ExportError("Clip %r has invalid frame rate %d/%d"
                          % (raw_clip.get("name"), numerator, denominator))
    measured = num_frames / duration
    declared = numerator / denominator
    if abs(measured - declared) > 1e-3:
        raise ExportError("Clip %r frameRate %d/%d does not match numFrames/duration (%.6f)"
                          % (raw_clip.get("name"), numerator, denominator, measured))
    return {"numerator": numerator, "denominator": denominator}


def notify_name(raw_event: dict) -> str:
    text = raw_event.get("text")
    if isinstance(text, dict):
        text = text.get("ueStructText")
    if not isinstance(text, str):
        raise ExportError("Notify event %r has no UE struct text" % raw_event)
    marker = 'NotifyName="'
    start = text.find(marker)
    if start < 0:
        raise ExportError("Notify event text has no NotifyName: " + text[:160])
    start += len(marker)
    end = text.find('"', start)
    if end < 0:
        raise ExportError("Notify event text has a malformed NotifyName")
    name = text[start:end]
    if not name:
        raise ExportError("Notify event has an empty NotifyName")
    return name


def build_clip_document(raw_clip: dict, fbx: Path, skeleton: dict,
                        skeleton_blake3: str) -> tuple[dict, dict]:
    name = str(raw_clip.get("name"))
    duration = float(raw_clip.get("duration"))
    if not math.isfinite(duration) or duration <= 0:
        raise ExportError("Clip %r has an invalid duration" % name)
    num_frames = int(raw_clip.get("numFrames"))
    num_keys = int(raw_clip.get("numKeys"))
    rate_scale = float(raw_clip.get("rateScale"))
    if num_frames <= 0 or num_keys <= 0:
        raise ExportError("Clip %r has invalid frame/key counts" % name)
    if not math.isfinite(rate_scale) or rate_scale <= 0:
        raise ExportError("Clip %r has invalid rateScale %r" % (name, rate_scale))
    frame_rate = frame_rate_record(raw_clip)
    root_motion = validate_settings(raw_clip)

    joint_names = {joint["name"] for joint in skeleton["joints"]}
    sk_curves = {curve_name.casefold(): curve_name for curve_name in skeleton["curves"]}

    tracks = [str(track) for track in raw_clip.get("tracks", [])]
    unknown_tracks = [track for track in tracks if track not in joint_names]
    if unknown_tracks:
        raise ExportError("Clip %r has tracks missing from SK joints: %s"
                          % (name, unknown_tracks[:8]))

    curves = []
    seen_curve_names: set[str] = set()
    for raw_curve in raw_clip.get("curves", []):
        curve_name = str(raw_curve.get("name", ""))
        key = curve_name.casefold()
        if key in seen_curve_names:
            raise ExportError("Clip %r has duplicate curve %r" % (name, curve_name))
        seen_curve_names.add(key)
        if key not in sk_curves:
            raise ExportError("Clip %r curve %r is not registered in SK curves" % (name, curve_name))
        times = [float(value) for value in raw_curve.get("times", [])]
        values = [float(value) for value in raw_curve.get("values", [])]
        if len(times) != len(values):
            raise ExportError("Clip %r curve %r has %d times / %d values"
                              % (name, curve_name, len(times), len(values)))
        if not times:
            # A metadata-only curve has no keys; it is registered in SK but the
            # sidecar can only carry actual keys (schema requires >=1).
            continue
        if len(times) > 100000:
            raise ExportError("Clip %r curve %r exceeds 100000 keys" % (name, curve_name))
        if any(not math.isfinite(time) for time in times):
            raise ExportError("Clip %r curve %r has a non-finite time" % (name, curve_name))
        if any(not math.isfinite(value) for value in values):
            raise ExportError("Clip %r curve %r has a non-finite value" % (name, curve_name))
        if any(right <= left for left, right in zip(times, times[1:])):
            raise ExportError("Clip %r curve %r times are not strictly increasing"
                              % (name, curve_name))
        curves.append({"name": curve_name, "times": times, "values": values})
    if len(curves) > 64:
        raise ExportError("Clip %r exceeds 64 float curves" % name)

    notifies = []
    for raw_event in raw_clip.get("notifies", []):
        track = str(raw_event.get("track", ""))
        if not track:
            raise ExportError("Clip %r has a notify without a track" % name)
        time = float(raw_event.get("time"))
        event_duration = float(raw_event.get("duration"))
        if not math.isfinite(time) or not math.isfinite(event_duration):
            raise ExportError("Clip %r has a non-finite notify time/duration" % name)
        event_name = notify_name(raw_event)
        if time < 0 or time > duration + 1e-5 or event_duration < 0:
            raise ExportError("Clip %r notify %r.%r has an invalid start/duration"
                              % (name, track, event_name))
        # A notifyState keeps its authored duration even when it crosses the
        # trimmed sequence end (Jump Land PoseSearchBlockTransition: t=0.532512,
        # duration=2.467489 -> end=3.0 > SequenceLength=2.766667).  Preserve the
        # UE value instead of clipping it to the sequence length.
        notifies.append({
            "track": track,
            "name": event_name,
            "time": time,
            "duration": event_duration,
        })
    if len(notifies) > 4096:
        raise ExportError("Clip %r exceeds 4096 notifies" % name)

    sync_markers = raw_clip.get("syncMarkers", [])
    if not isinstance(sync_markers, list):
        raise ExportError("Clip %r syncMarkers is not an array" % name)

    document = {
        "format": "vl.anim-sequence.v1",
        "sequence": name,
        "skeleton": skeleton["skeleton"],
        "skeletonBlake3": skeleton_blake3,
        "duration": duration,
        "frameRate": frame_rate,
        "numFrames": num_frames,
        "numKeys": num_keys,
        "rateScale": rate_scale,
        "rootMotion": root_motion,
        "tracks": tracks,
        "floatCurveKeys": curves,
        "notifies": notifies,
        "syncMarkers": sync_markers,
        "sampling": {"interpolation": "linear", "outside": "clamp"},
        "source": {
            "fbx": fbx.name,
            "uePackage": str(raw_clip.get("package", "")),
            "exportedBy": "export_gasp_sequences.py",
        },
    }
    if len(json.dumps(document, ensure_ascii=False).encode("utf-8")) > 8 * 1024 * 1024:
        raise ExportError("Clip %r sidecar exceeds 8 MB" % name)
    return document, {
        "clip": name,
        "curves": len(curves),
        "keys": sum(len(curve["times"]) for curve in curves),
        "notifies": len(notifies),
        "tracks": len(tracks),
        "duration": duration,
    }


def verify_hurdle(document: dict) -> dict:
    curve = find_curve(document["floatCurveKeys"], HURDLE_CURVE)
    if curve is None:
        raise ExportError("Hurdle sidecar has no %r curve" % HURDLE_CURVE)
    values = [linear_sample(curve, time) for time in HURDLE_WINDOW_ENDS]
    deltas = [value - expected for value, expected in zip(values, HURDLE_EXPECTED_VALUES)]
    d_out = abs(values[1] - values[0])
    d_out_delta = d_out - HURDLE_EXPECTED_DOUT
    if any(abs(delta) > HURDLE_VALUE_TOLERANCE_CM for delta in deltas):
        raise ExportError(
            "Hurdle curve verification failed: %s vs %s (delta %s, tolerance %.1f cm)"
            % (values, HURDLE_EXPECTED_VALUES, deltas, HURDLE_VALUE_TOLERANCE_CM))
    if abs(d_out_delta) > HURDLE_VALUE_TOLERANCE_CM:
        raise ExportError("Hurdle dOut %.6f differs from %.6f by %.6f cm"
                          % (d_out, HURDLE_EXPECTED_DOUT, d_out_delta))
    return {
        "times": list(HURDLE_WINDOW_ENDS),
        "values": values,
        "expected": list(HURDLE_EXPECTED_VALUES),
        "deltas": deltas,
        "dOut": d_out,
        "dOutExpected": HURDLE_EXPECTED_DOUT,
        "dOutDelta": d_out_delta,
        "keyCount": len(curve["times"]),
    }


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path,
                        default=Path(os.environ.get("VL_GASP_RESOURCE_ROOT", DEFAULT_RESOURCE_ROOT)))
    parser.add_argument("--raw", type=Path, default=DEFAULT_RAW,
                        help="UE raw animation dump")
    parser.add_argument("--mesh", type=Path, default=None,
                        help="default: <resource-root>/" + MESH_RELATIVE)
    parser.add_argument("--skeleton", type=Path, default=None,
                        help="default: <resource-root>/" + SKELETON_RELATIVE)
    parser.add_argument("--clips", default=None,
                        help="comma-separated SM clip ids to export (default: all)")
    parser.add_argument("--continue-on-error", action="store_true",
                        help="record a failed clip and keep exporting the rest")
    parser.add_argument("--force", action="store_true", help="replace existing .anim.json files")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    arguments = parse_arguments(argv)
    try:
        resource_root = arguments.resource_root.resolve()
        mesh_path = (arguments.mesh or (resource_root / MESH_RELATIVE)).resolve()
        skeleton_path = (arguments.skeleton or (resource_root / SKELETON_RELATIVE)).resolve()
        raw = load_json(arguments.raw.resolve())
        if raw.get("format") != "vl.gasp.ue-animation-raw.v1" or raw.get("status") != "complete":
            raise ExportError("UE raw animation dump is incomplete: " + str(arguments.raw))
        mesh = load_json(mesh_path)
        skeleton = load_json(skeleton_path)
        if skeleton.get("format") != "vl.skeleton.v1":
            raise ExportError("Unexpected skeleton format: " + str(skeleton_path))
        skeleton_blake3 = blake3_file(skeleton_path)
        clip_entries = mesh.get("skinning", {}).get("clips", [])
        if not clip_entries:
            raise ExportError("Mesh has no skinning.clips")
        all_ids = [str(item["id"]) for item in clip_entries]
        requested = None
        if arguments.clips is not None:
            requested = [token.strip() for token in arguments.clips.split(",") if token.strip()]
            if not requested:
                raise ExportError("--clips is empty")
            if len(set(requested)) != len(requested):
                raise ExportError("--clips contains duplicate clip ids")
            unknown = [clip for clip in requested if clip not in set(all_ids)]
            if unknown:
                raise ExportError("--clips ids are not in %s: %s"
                                  % (MESH_RELATIVE, unknown[:8]))
        if requested is None:
            selected_ids = list(all_ids)
        else:
            # Keep SM order so a repeated run is byte-identical regardless of the
            # order the ids were passed in.
            selected_ids = [clip for clip in all_ids if clip in set(requested)]
        raw_by_name = {str(clip.get("name")): clip for clip in raw.get("clips", [])}
        if len(raw_by_name) != len(raw.get("clips", [])):
            raise ExportError("UE raw dump has duplicate clip names")
        missing_raw = [clip for clip in selected_ids if clip not in raw_by_name]
        if missing_raw:
            raise ExportError("UE raw dump is missing clip(s): %s" % missing_raw[:8])
        if requested is None and set(raw_by_name) != set(all_ids):
            extra = sorted(set(raw_by_name) - set(all_ids))
            raise ExportError("Clip list mismatch: extra=%s" % extra[:8])

        documents = []
        summaries = []
        failures = []
        selected_set = set(selected_ids)
        for item in clip_entries:
            clip_id = str(item["id"])
            if clip_id not in selected_set:
                continue
            try:
                fbx = (resource_root / str(item["path"])).resolve()
                if not fbx.is_file():
                    raise ExportError("Missing clip FBX: " + str(fbx))
                if fbx.suffix.lower() != ".fbx":
                    raise ExportError("Clip path is not a .fbx: " + str(fbx))
                document, summary = build_clip_document(
                    raw_by_name[clip_id], fbx, skeleton, skeleton_blake3)
            except (ExportError, OSError, ValueError, KeyError) as error:
                if not arguments.continue_on_error:
                    raise
                failures.append({"clip": clip_id, "error": str(error)})
                continue
            output = fbx.with_name(fbx.stem + ".anim.json")
            documents.append((output, document))
            summary["output"] = resource_relative(output, resource_root)
            summaries.append(summary)

        hurdle = next((document for output, document in documents
                       if document["sequence"] == HURDLE_CLIP), None)
        if hurdle is not None:
            hurdle_check = verify_hurdle(hurdle)
        else:
            hurdle_check = {"skipped": HURDLE_CLIP + " is not in this batch"}

        preflight_targets([output for output, _ in documents], arguments.force)
        total_bytes = 0
        for output, document in documents:
            total_bytes += write_json(output, document, arguments.force)
        print(json.dumps({
            "format": "vl.anim-sequence.v1",
            "clips": len(documents),
            "failed": failures,
            "traversal": sum(1 for summary in summaries
                             if "Traversal" in summary["clip"]),
            "locomotion": sum(1 for summary in summaries
                              if "Traversal" not in summary["clip"]),
            "bytes": total_bytes,
            "hurdle": hurdle_check,
            "items": summaries,
        }, ensure_ascii=False, indent=2))
        return 0 if not failures else 1
    except (ExportError, FileExistsError, OSError, ValueError, KeyError) as error:
        print("export_gasp_sequences.py: error: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

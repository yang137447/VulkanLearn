#!/usr/bin/env python3
"""Convert the GASP traversal AnimMontage T3D exports to JSON.

Input: ``artifacts/gasp-warp-audit/montages/AM_*.copy`` (UE ObjectExporterT3D).
Output: ``Maps/SC_gasp/Animations/AM_*.json``.

Selection is bidirectional: every ``AM_*.copy`` in the input directory must map to
an SM clip id, and every SM clip with a T3D is converted.  ``--clips`` restricts
the batch to a comma-separated subset (the Catch export converts only the new
family while the 10 historical montages stay untouched).

The converter is intentionally faithful to the T3D, not to an older hand-written
C# approximation:

* windows follow the ``Notifies(i)`` array order -- no time sorting;
* ``LinkValue`` / ``Duration`` / ``EndLink.LinkValue`` become ``time`` and
  ``duration``; ``NotifyName`` is preserved;
* ``WarpPointAnimProvider`` defaults to ``None``; omitted ``bWarpRotation``
  defaults to True (UE's default), not False;
* all known provider/easing/section/link values are preserved.  Only data that
  cannot be expressed losslessly (unknown provider/easing/link enum, missing
  Static transform, out-of-range section/window) is rejected;
* the montage id is the UE asset name from the T3D ``ExportPath``; the
  ``sequence`` is the AnimSequence named by the montage's
  ``SlotAnimTracks.AnimReference``, not a name derived from the montage.  GASP's
  ``AM_..._Catch_Cliff_high_standF`` plays ``M_..._Catch_Cliff_high_stand``, so
  the binding has to be read from the data;
* the Sequence path is ``Maps/SC_gasp/Source/<clip>.anim.json`` and
  ``sequenceBlake3`` is the current sidecar digest.  No clip id field is written.

Re-running is refused unless ``--force`` is passed.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from pathlib import Path

from gasp_anim_common import (
    blake3_file,
    load_json,
    preflight_targets,
    resource_relative,
    write_json,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RESOURCE_ROOT = REPO_ROOT.parent / "VukanLearnResources"
DEFAULT_MONTAGES = REPO_ROOT / "artifacts/gasp-warp-audit/montages"
MESH_RELATIVE = "Maps/SC_gasp/Meshes/SM_mannequin.json"
SKELETON_RELATIVE = "Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json"
SOURCE_RELATIVE = "Maps/SC_gasp/Source"
OUTPUT_RELATIVE = "Maps/SC_gasp/Animations"

KNOWN_PROVIDERS = {"None", "Static", "Bone"}
KNOWN_EASINGS = {
    "Linear", "Step", "SinusoidalIn", "SinusoidalOut", "SinusoidalInOut",
    "EaseIn", "EaseOut", "EaseInOut", "ExpoIn", "ExpoOut", "ExpoInOut",
    "CircularIn", "CircularOut", "CircularInOut",
}
KNOWN_LINK_METHODS = {"Absolute", "Relative", "Ratio"}


class ExportError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# T3D helpers
# ---------------------------------------------------------------------------

def balanced(text: str, open_index: int) -> tuple[str, int]:
    """Return the balanced parenthesised span starting at *open_index*."""
    if open_index >= len(text) or text[open_index] != "(":
        raise ExportError("Expected '(' at offset %d" % open_index)
    depth = 0
    in_string = False
    escaped = False
    for index in range(open_index, len(text)):
        character = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth == 0:
                return text[open_index:index + 1], index + 1
    raise ExportError("Unbalanced parentheses in T3D")


def assignments(text: str, name: str) -> list[tuple[int, str]]:
    """Find ``Name(index)=(...)`` assignments and return (index, balanced value)."""
    pattern = re.compile(r"(?m)^[ \t]*" + re.escape(name) + r"\((\d+)\)=[ \t]*\(")
    result = []
    for match in pattern.finditer(text):
        value, _ = balanced(text, match.end() - 1)
        result.append((int(match.group(1)), value))
    return result


def unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1].replace('\\"', '"')
    return value


def property_value(text: str, key: str) -> str | None:
    match = re.search(r"\b" + re.escape(key) + r'=("([^"]*)"|[^\s,)]+)', text)
    if not match:
        return None
    return match.group(2) if match.group(2) is not None else match.group(1)


def property_bool(text: str, key: str, default: bool) -> bool:
    value = property_value(text, key)
    if value is None:
        return default
    lowered = value.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    raise ExportError("%s has non-boolean value %r" % (key, value))


def property_float(text: str, key: str, default: float) -> float:
    value = property_value(text, key)
    if value is None:
        return default
    try:
        result = float(value)
    except ValueError as error:
        raise ExportError("%s has non-numeric value %r" % (key, value)) from error
    if not math.isfinite(result):
        raise ExportError("%s is not finite" % key)
    return result


def object_blocks(text: str, name_prefix: str) -> dict[str, str]:
    """Return ``{object name: body}`` for nested ``Begin Object Name=`` blocks."""
    result: dict[str, str] = {}
    pattern = re.compile(r'Begin Object Name="([^"]+)"')
    for match in pattern.finditer(text):
        name = match.group(1)
        if not name.startswith(name_prefix):
            continue
        position = match.end()
        depth = 1
        body_start = position
        while depth:
            next_begin = text.find("Begin Object ", position)
            next_end = text.find("End Object", position)
            if next_end < 0:
                raise ExportError("Unbalanced Begin/End Object in T3D")
            if 0 <= next_begin < next_end:
                depth += 1
                position = next_begin + len("Begin Object ")
            else:
                depth -= 1
                position = next_end + len("End Object")
                if depth == 0:
                    result[name] = text[body_start:next_end]
    return result


def last_float(text: str, key: str) -> float | None:
    matches = re.findall(r"\b" + re.escape(key) + r"=(-?[0-9]+(?:\.[0-9]+)?(?:[eE][-+]?[0-9]+)?)", text)
    return float(matches[-1]) if matches else None


def end_link_parts(value: str) -> tuple[float | None, str]:
    """Split outer notify fields from the nested EndLink value."""
    match = re.search(r"EndLink=[ \t]*\(", value)
    if not match:
        return None, value
    end_link, end = balanced(value, match.end() - 1)
    return last_float(end_link, "LinkValue"), value[:match.start()] + value[end:]


# ---------------------------------------------------------------------------
# Montage conversion
# ---------------------------------------------------------------------------

def montage_export_path(text: str) -> str:
    match = re.search(
        r"Begin Object Class=/Script/Engine\.AnimMontage[^\n]*?"
        r'ExportPath="[^"]*\'([^\']+)\'"', text, re.S)
    if not match:
        raise ExportError("T3D has no AnimMontage ExportPath")
    return match.group(1)


def parse_static_pose(text: str) -> dict:
    def transform_value(key: str) -> dict:
        match = re.search(r"\b" + key + r"=[ \t]*\(", text)
        if not match:
            raise ExportError("Static warp provider has no %s transform" % key)
        body, _ = balanced(text, match.end() - 1)
        components = {}
        for name in ("X", "Y", "Z", "W"):
            value = property_value(body, name)
            if value is None:
                raise ExportError("Static warp %s.%s is missing" % (key, name))
            components[name] = float(value)
        return components

    rotation = transform_value("Rotation")
    location = transform_value("Translation")
    return {
        "location": [location["X"], location["Y"], location["Z"]],
        "rotation": [rotation["X"], rotation["Y"], rotation["Z"], rotation["W"]],
    }


def parse_warp(body: str) -> dict:
    target = unquote(property_value(body, "WarpTargetName") or "")
    if not target:
        raise ExportError("MotionWarping notify has an empty WarpTargetName")
    if len(target) > 64:
        raise ExportError("WarpTargetName exceeds 64 characters")
    provider = unquote(property_value(body, "WarpPointAnimProvider") or "None")
    if provider not in KNOWN_PROVIDERS:
        raise ExportError("Unknown WarpPointAnimProvider %r" % provider)
    bone = unquote(property_value(body, "WarpPointAnimBoneName") or "")
    if provider == "Bone" and not bone:
        raise ExportError("Bone warp provider has an empty bone name")
    warp = {
        "target": target,
        "provider": provider,
        "bone": bone if provider == "Bone" else "",
        "subtractRemainingRootMotion": property_bool(body, "bSubtractRemainingRootMotion", False),
        "ignoreZAxis": property_bool(body, "bIgnoreZAxis", False),
        "warpRotation": property_bool(body, "bWarpRotation", True),
        "rotationTimeMultiplier": property_float(body, "WarpRotationTimeMultiplier", 1.0),
        "easing": unquote(property_value(body, "AddTranslationEasingFunc") or "Linear"),
    }
    if warp["easing"] not in KNOWN_EASINGS:
        raise ExportError("Unknown AddTranslationEasingFunc %r" % warp["easing"])
    if warp["rotationTimeMultiplier"] <= 0:
        raise ExportError("WarpRotationTimeMultiplier must be > 0")
    if provider == "Static":
        static_pose = parse_static_pose(body)
        warp["staticPose"] = static_pose
    return warp


def parse_sections(text: str, length: float) -> list[dict]:
    raw_sections = assignments(text, "CompositeSections")
    if not raw_sections:
        raise ExportError("Montage has no CompositeSections")
    sections = []
    previous_end = 0.0
    for index, value in sorted(raw_sections):
        name = unquote(property_value(value, "SectionName") or "")
        if not name:
            raise ExportError("Section %d has an empty name" % index)
        link_value = last_float(value, "LinkValue")
        segment_begin = last_float(value, "SegmentBeginTime")
        if link_value is not None and link_value >= 0:
            start = link_value
        elif segment_begin is not None and segment_begin >= 0:
            start = segment_begin
        else:
            start = 0.0 if index == 0 else previous_end
        segment_length = property_float(value, "SegmentLength", -1.0)
        if segment_length <= 0:
            raise ExportError("Section %r has invalid SegmentLength" % name)
        end = start + segment_length
        next_section = unquote(property_value(value, "NextSectionName") or "")
        next_time = 0.0
        link_method = ""
        next_link_match = re.search(r"NextLink=[ \t]*\(", value)
        if next_link_match:
            next_link, _ = balanced(value, next_link_match.end() - 1)
            parsed_time = last_float(next_link, "LinkValue")
            if parsed_time is not None:
                next_time = parsed_time
            link_method = unquote(property_value(next_link, "LinkMethod") or "")
        if not link_method:
            link_method = unquote(property_value(value, "LinkMethod") or "")
        if link_method and link_method not in KNOWN_LINK_METHODS:
            raise ExportError("Unknown section LinkMethod %r" % link_method)
        sections.append({
            "name": name,
            "startTime": start,
            "endTime": end,
            "nextSection": next_section,
            "nextTime": next_time,
            "linkMethod": link_method,
        })
        previous_end = end
    if len({section["name"] for section in sections}) != len(sections):
        raise ExportError("Duplicate section names")
    for section in sections:
        if section["endTime"] <= section["startTime"]:
            raise ExportError("Section %r ends before it starts" % section["name"])
        if section["endTime"] > length + 1e-3:
            raise ExportError("Section %r exceeds montage length" % section["name"])
    if len(sections) == 1 and abs(sections[0]["endTime"] - length) > 1e-3:
        raise ExportError("Single Default section does not match SequenceLength")
    for section in sections:
        target = section["nextSection"]
        if target and target not in {entry["name"] for entry in sections}:
            raise ExportError("Section %r links to unknown section %r" % (section["name"], target))
        if not (0 <= section["nextTime"] <= length + 1e-3):
            raise ExportError("Section %r NextTime is outside the montage" % section["name"])
    return sections


def parse_notifies(text: str, tracks: dict[int, str], length: float,
                   warp_blocks: dict[str, str]) -> list[dict]:
    raw_notifies = assignments(text, "Notifies")
    notifies = []
    for index, value in sorted(raw_notifies):
        name = unquote(property_value(value, "NotifyName") or "")
        if not name:
            raise ExportError("Notify %d has an empty NotifyName" % index)
        end_time, outer = end_link_parts(value)
        start_time = last_float(outer, "LinkValue")
        if start_time is None:
            # T3D omits default-valued properties; AnimNotifyEvent.LinkValue
            # defaults to 0.0 (Hurdle's PoseSearchBranchIn at t=0).
            start_time = 0.0
        duration = property_float(outer, "Duration", 0.0)
        state_class = property_value(outer, "NotifyStateClass")
        notify_class = property_value(outer, "Notify")
        is_warp_notify = ("MotionWarping" in (state_class or "")
                          or "MotionWarping" in (notify_class or ""))
        if duration < 0 or start_time < 0 or start_time > length + 1e-3:
            raise ExportError("Notify %r is outside the montage" % name)
        if is_warp_notify and (duration <= 0 or start_time + duration > length + 1e-3):
            raise ExportError("Warp notify %r is outside the montage" % name)
        if end_time is not None and abs(start_time + duration - end_time) > 1e-3:
            raise ExportError("Notify %r EndLink does not match time+duration" % name)
        track_index = property_value(outer, "TrackIndex")
        track_number = int(track_index) if track_index is not None else 0
        track = tracks.get(track_number, str(track_number))
        if state_class is not None and state_class != "None":
            kind = "notifyState"
            state_name_match = re.search(r"'([^']+)'\"?$", state_class)
            state_name = state_name_match.group(1) if state_name_match else ""
        else:
            kind = "notify"
            state_name = ""
        notify = {
            "kind": kind,
            "track": track,
            "name": name,
            "time": start_time,
            "duration": duration,
        }
        if is_warp_notify:
            body = warp_blocks.get(state_name)
            if body is None:
                raise ExportError("MotionWarping notify %r has no modifier object" % name)
            notify["warp"] = parse_warp(body)
        notifies.append(notify)
    if len(notifies) > 64:
        raise ExportError("Montage exceeds 64 notifies")
    if sum(1 for notify in notifies if "warp" in notify) > 8:
        raise ExportError("Montage exceeds 8 warp windows")
    return notifies


def parse_slot(text: str, skeleton_slots: list[str]) -> str:
    tracks = assignments(text, "SlotAnimTracks")
    if len(tracks) > 1:
        raise ExportError("Multiple SlotAnimTracks cannot be represented by vl.anim-montage.v1")
    if not tracks:
        raise ExportError("Montage has no SlotAnimTracks")
    value = tracks[0][1]
    slot = unquote(property_value(value, "SlotName") or "DefaultSlot")
    if slot not in skeleton_slots:
        raise ExportError("Montage slot %r is not registered in SK slots %s"
                          % (slot, skeleton_slots))
    return slot


def montage_name(text: str) -> str:
    """UE's montage asset name (``montage_export_path`` returns the object path)."""
    name = montage_export_path(text).rsplit(".", 1)[-1]
    if not name or "/" in name:
        raise ExportError("T3D montage object path has no asset name: " + montage_export_path(text))
    return name


def sequence_reference(text: str, t3d_name: str) -> tuple[str, str]:
    """Return ``(clip id, package)`` for the AnimSequence the montage plays.

    The reference is read from ``SlotAnimTracks`s ``AnimReference`` instead of
    being derived from the file name: GASP's ``AM_..._Catch_Cliff_high_standF``
    plays ``M_..._Catch_Cliff_high_stand``, so the pairing is data, not a rule.
    """
    tracks = assignments(text, "SlotAnimTracks")
    if len(tracks) != 1:
        raise ExportError("Montage has %d SlotAnimTracks; one is expected" % len(tracks))
    match = re.search(r'AnimReference="/Script/Engine\.AnimSequence\'([^\']+)\'"', tracks[0][1])
    if not match:
        raise ExportError("SlotAnimTracks has no AnimReference: " + t3d_name)
    package = match.group(1)
    clip_id = package.rsplit(".", 1)[-1]
    if not clip_id:
        raise ExportError("AnimReference has no object name: " + package)
    sections = assignments(text, "CompositeSections")
    if sections and not any(package in value for _, value in sections):
        raise ExportError("CompositeSections do not reference the played sequence: " + package)
    return clip_id, package


def parse_montage(text: str, name: str, clip_id: str, t3d_name: str, skeleton: dict,
                  skeleton_blake3: str, sequence_path: Path,
                  resource_root: Path) -> tuple[dict, dict]:
    if not text:
        raise ExportError("Empty T3D")
    ue_package = montage_export_path(text)
    length_match = re.search(r"\bSequenceLength=(-?[0-9]+(?:\.[0-9]+)?)", text)
    if not length_match:
        raise ExportError("Montage has no SequenceLength")
    length = float(length_match.group(1))
    if not math.isfinite(length) or length <= 0:
        raise ExportError("Montage SequenceLength must be > 0")

    tracks = {index: unquote(property_value(value, "TrackName") or "")
              for index, value in assignments(text, "AnimNotifyTracks")}
    if any(not name for name in tracks.values()):
        raise ExportError("Montage has an unnamed notify track")
    sections = parse_sections(text, length)
    slot = parse_slot(text, skeleton["slots"])
    warp_blocks = object_blocks(text, "AnimNotifyState_MotionWarping_")
    notifies = parse_notifies(text, tracks, length, warp_blocks)

    if not sequence_path.is_file():
        raise ExportError("Missing Sequence sidecar: " + str(sequence_path))
    sequence = load_json(sequence_path)
    if sequence.get("format") != "vl.anim-sequence.v1" or sequence.get("sequence") != clip_id:
        raise ExportError("Sequence sidecar does not describe clip %r: %s" % (clip_id, sequence_path))
    if sequence.get("skeleton") != skeleton["skeleton"]:
        raise ExportError("Sequence %r skeleton mismatch" % clip_id)
    if sequence.get("skeletonBlake3") != skeleton_blake3:
        raise ExportError("Sequence %r skeletonBlake3 mismatch" % clip_id)

    for notify in notifies:
        warp = notify.get("warp")
        if warp and warp["provider"] == "Bone" and warp["bone"] not in {
                joint["name"] for joint in skeleton["joints"]}:
            raise ExportError("Warp bone %r does not exist in SK joints" % warp["bone"])

    document = {
        "format": "vl.anim-montage.v1",
        "montage": name,
        "sequence": resource_relative(sequence_path, resource_root),
        "sequenceBlake3": blake3_file(sequence_path),
        "slot": slot,
        "length": length,
        "sections": sections,
        "notifies": notifies,
        "source": {
            "t3d": t3d_name,
            "uePackage": ue_package,
            "exportedBy": "import_gasp_montages.py",
        },
    }
    summary = {
        "montage": document["montage"],
        "clip": clip_id,
        "t3d": t3d_name,
        "length": length,
        "sequenceDuration": float(sequence["duration"]),
        "slot": slot,
        "sections": len(sections),
        "notifies": len(notifies),
        "warpNotifies": sum(1 for notify in notifies if "warp" in notify),
        "notifyKinds": sorted({notify["kind"] for notify in notifies}),
        "tracks": sorted({notify["track"] for notify in notifies}),
    }
    return document, summary


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path,
                        default=Path(os.environ.get("VL_GASP_RESOURCE_ROOT", DEFAULT_RESOURCE_ROOT)))
    parser.add_argument("--montages", type=Path, default=DEFAULT_MONTAGES)
    parser.add_argument("--mesh", type=Path, default=None,
                        help="default: <resource-root>/" + MESH_RELATIVE)
    parser.add_argument("--skeleton", type=Path, default=None,
                        help="default: <resource-root>/" + SKELETON_RELATIVE)
    parser.add_argument("--source", type=Path, default=None,
                        help="default: <resource-root>/" + SOURCE_RELATIVE)
    parser.add_argument("--clips", default=None,
                        help="comma-separated SM clip ids to convert (default: every match)")
    parser.add_argument("--force", action="store_true", help="replace existing AM_*.json files")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    arguments = parse_arguments(argv)
    try:
        resource_root = arguments.resource_root.resolve()
        mesh = load_json((arguments.mesh or (resource_root / MESH_RELATIVE)).resolve())
        skeleton_path = (arguments.skeleton or (resource_root / SKELETON_RELATIVE)).resolve()
        skeleton = load_json(skeleton_path)
        skeleton_blake3 = blake3_file(skeleton_path)
        source_dir = (arguments.source or (resource_root / SOURCE_RELATIVE)).resolve()
        montage_dir = arguments.montages.resolve()
        clips = [str(item["id"]) for item in mesh.get("skinning", {}).get("clips", [])]
        t3d_files = sorted(montage_dir.glob("AM_*.copy"))
        if not t3d_files:
            raise ExportError("No montage T3D under " + str(montage_dir))
        # The audit directory holds the whole UE traversal montage menu; only the
        # ones whose played sequence is an SM clip can be converted.  Unmatched
        # files are reported instead of silently ignored or rejected.
        by_clip: dict[str, Path] = {}
        unparsed = []
        for path in t3d_files:
            text = path.read_text(encoding="utf-8", errors="replace").lstrip("\ufeff")
            try:
                clip_id, _ = sequence_reference(text, path.name)
            except ExportError:
                unparsed.append(path.name)
                continue
            if clip_id in by_clip:
                raise ExportError("Two montage T3D play clip %r: %s, %s"
                                  % (clip_id, by_clip[clip_id].name, path.name))
            by_clip[clip_id] = path
        unmatched = sorted(set(by_clip) - set(clips))
        if arguments.clips is not None:
            requested = [token.strip() for token in arguments.clips.split(",") if token.strip()]
            if not requested:
                raise ExportError("--clips is empty")
            if len(set(requested)) != len(requested):
                raise ExportError("--clips contains duplicate clip ids")
            unknown = [clip for clip in requested if clip not in set(clips)]
            if unknown:
                raise ExportError("--clips ids are not SM clips: %s" % unknown[:8])
            absent = [clip for clip in requested if clip not in by_clip]
            if absent:
                raise ExportError("No montage T3D plays clip(s): %s" % absent[:8])
            clips = [clip for clip in clips if clip in set(requested)]
        else:
            clips = [clip for clip in clips if clip in by_clip]
        if not clips:
            raise ExportError("No montage T3D plays an SM clip")
        documents = []
        summaries = []
        for clip_id in clips:
            t3d_path = by_clip[clip_id]
            text = t3d_path.read_text(encoding="utf-8", errors="replace").lstrip("\ufeff")
            sequence_path = source_dir / (clip_id + ".anim.json")
            document, summary = parse_montage(
                text, montage_name(text), clip_id, t3d_path.name,
                skeleton, skeleton_blake3, sequence_path, resource_root)
            output = (resource_root / OUTPUT_RELATIVE / (document["montage"] + ".json")).resolve()
            summary["output"] = resource_relative(output, resource_root)
            summary["t3dBytes"] = t3d_path.stat().st_size
            documents.append((output, document))
            summaries.append(summary)

        preflight_targets([output for output, _ in documents], arguments.force)
        total_bytes = 0
        for output, document in documents:
            total_bytes += write_json(output, document, arguments.force)
        print(json.dumps({
            "format": "vl.anim-montage.v1",
            "montages": len(documents),
            "sections": sum(summary["sections"] for summary in summaries),
            "notifies": sum(summary["notifies"] for summary in summaries),
            "warpNotifies": sum(summary["warpNotifies"] for summary in summaries),
            "bytes": total_bytes,
            "unmatchedT3d": unmatched,
            "unparsedT3d": unparsed,
            "items": summaries,
        }, ensure_ascii=False, indent=2))
        return 0
    except (ExportError, FileExistsError, OSError, ValueError, KeyError) as error:
        print("import_gasp_montages.py: error: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

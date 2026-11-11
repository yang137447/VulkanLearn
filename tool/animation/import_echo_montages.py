#!/usr/bin/env python3
"""Convert the Echo AnimMontage T3D exports to ``vl.anim-montage.v1`` JSON.

Input: the three ``*.t3d`` files produced by ``audit_echo_montages.py``.
Output: ``<resource-root>/Maps/SC_valley_echo/Animations/AM_*.json``.

The T3D parsing is shared with ``import_gasp_montages.py`` so both families use
the same section/notify/link/warp rules:

* notifies keep the T3D ``Notifies(i)`` array order;
* the played AnimSequence comes from ``SlotAnimTracks.AnimReference``, never
  from the Montage file name;
* the Sequence sidecar must be a loaded ``skinning.clips[]`` entry for Echo's
  ``SK_Echo`` skeleton, and ``sequenceBlake3`` is its current digest;
* a Montage with more than one AnimSegment keeps the UE timeline structure as
  ``segments[]`` (sequence, timeline span, clip span, rate, loop); a single
  segment keeps the legacy top-level ``sequence`` shape;
* only data that cannot be represented losslessly is rejected (multiple
  SlotAnimTracks, unknown enums, out-of-range sections or warp windows).

The Megascans-style Montage asset names do not carry the ``AM_`` prefix, so the
VL Montage id (and output file name) is ``AM_<asset name>``.  The original UE
asset name is retained in ``source.ueMontageAsset``.

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
from import_gasp_montages import (
    ExportError,
    assignments,
    balanced,
    montage_name,
    object_blocks,
    parse_notifies,
    parse_sections,
    parse_slot,
    property_float,
    property_value,
    unquote,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RESOURCE_ROOT = REPO_ROOT.parent / "VukanLearnResources"
DEFAULT_AUDIT_ROOT = REPO_ROOT / "artifacts/ue-echo-export/montage-audit"
MESH_RELATIVE = "Maps/SC_valley_echo/Meshes/SKM_Echo.json"
SKELETON_RELATIVE = "Maps/SC_valley_echo/Skeletons/SK_Echo.json"
SOURCE_RELATIVE = "Maps/SC_valley_echo/Source"
OUTPUT_RELATIVE = "Maps/SC_valley_echo/Animations"
DEFAULT_MONTAGES = (
    "VaultOver_Montage",
    "ReachOut_Montage_RM",
    "ReachOut_End_Montage",
)


class UnsupportedMontageError(ExportError):
    """The T3D is valid but cannot be represented by vl.anim-montage.v1."""


def vl_montage_id(asset_name: str) -> str:
    return asset_name if asset_name.startswith("AM_") else "AM_" + asset_name


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path,
                        default=Path(os.environ.get(
                            "VL_ECHO_RESOURCE_ROOT", DEFAULT_RESOURCE_ROOT)))
    parser.add_argument("--montages", type=Path,
                        default=Path(os.environ.get(
                            "VL_ECHO_MONTAGE_T3D_DIR", DEFAULT_AUDIT_ROOT / "t3d")))
    parser.add_argument("--audit", type=Path,
                        default=Path(os.environ.get(
                            "VL_ECHO_MONTAGE_REPORT",
                            DEFAULT_AUDIT_ROOT / "ue-montage-audit.json")))
    parser.add_argument("--mesh", type=Path, default=None,
                        help="default: <resource-root>/" + MESH_RELATIVE)
    parser.add_argument("--skeleton", type=Path, default=None,
                        help="default: <resource-root>/" + SKELETON_RELATIVE)
    parser.add_argument("--source", type=Path, default=None,
                        help="default: <resource-root>/" + SOURCE_RELATIVE)
    parser.add_argument("--names", default=None,
                        help="comma-separated UE Montage asset names "
                             "(default: the three Echo Montages)")
    parser.add_argument("--force", action="store_true",
                        help="replace existing AM_*.json files")
    parser.add_argument("--continue-on-error", action="store_true",
                        help="write the representable Montages and report the "
                             "rest instead of aborting the batch")
    return parser.parse_args(argv)


def requested_assets(arguments: argparse.Namespace) -> list[str]:
    if arguments.names is None:
        return list(DEFAULT_MONTAGES)
    names = [token.strip() for token in arguments.names.split(",") if token.strip()]
    if not names:
        raise ExportError("--names is empty")
    if len(set(names)) != len(names):
        raise ExportError("--names contains duplicates")
    return names


def find_t3d(montage_dir: Path, asset_name: str) -> Path:
    path = montage_dir / (asset_name + ".t3d")
    if not path.is_file():
        raise ExportError("Missing Montage T3D: " + str(path))
    return path


def audit_entries(path: Path) -> dict[str, dict]:
    if not path.is_file():
        return {}
    document = load_json(path)
    sequences = {str(entry.get("name")): entry
                 for entry in document.get("sequences", [])}
    result = {}
    for entry in document.get("montages", []):
        record = dict(entry)
        references = (record.get("summary") or {}).get("sequenceReferences") or []
        for reference in references:
            name = str(reference).rsplit(".", 1)[-1]
            if name in sequences:
                record["sequence"] = sequences[name]
                break
        result[str(record.get("name"))] = record
    return result


def check_audit(entry: dict, document: dict, clip_ids: list[str],
                sequence_duration: float) -> list[str]:
    """Cross-check the converted document against the UE-side audit summary."""
    warnings = []
    summary = entry.get("summary", {})
    if summary.get("sequenceLength") is not None:
        if abs(float(summary["sequenceLength"]) - float(document["length"])) > 1e-3:
            raise ExportError("T3D SequenceLength disagrees with the audit summary")
    if summary.get("sectionCount") != len(document["sections"]):
        raise ExportError("T3D section count disagrees with the audit summary")
    if summary.get("notifyCount") != len(document["notifies"]):
        raise ExportError("T3D notify count disagrees with the audit summary")
    section_names = summary.get("sectionNames") or []
    if section_names and section_names != [section["name"]
                                            for section in document["sections"]]:
        warnings.append("section order/name differs from the raw T3D regex order")
    slot_names = summary.get("slotNames") or []
    if slot_names and document["slot"] not in slot_names:
        raise ExportError("converted slot is not in the T3D SlotName list")
    if not slot_names and document["slot"] != "DefaultSlot":
        warnings.append("T3D omits SlotName but the converter selected "
                        + document["slot"])
    references = summary.get("sequenceReferences") or []
    reference_ids = [reference.rsplit(".", 1)[-1] for reference in references]
    if references and any(clip not in clip_ids for clip in reference_ids):
        raise ExportError("converted clip id is not in the T3D sequence references")
    if references and sorted(set(reference_ids)) != sorted(set(clip_ids)):
        raise ExportError("converted segments do not match T3D sequence references")
    sequences = entry.get("sequence") or {}
    if sequences and abs(float(sequences.get("duration", sequence_duration))
                         - sequence_duration) > 1e-3:
        warnings.append("UE sequence duration differs from the sidecar duration")
    return warnings


def check_python_events(entry: dict, document: dict) -> list[str]:
    """Compare the converted notifies with UE's Python notify structs."""
    python = entry.get("python") or {}
    expected = python.get("notifyEvents") or []
    actual = [{
        "name": notify["name"],
        "kind": notify["kind"],
        "time": notify["time"],
        "duration": notify["duration"],
    } for notify in document["notifies"]]
    if not expected:
        if not actual:
            return []
        return ["no UE Python notify events to cross-check"]
    if len(expected) != len(actual):
        raise UnsupportedMontageError(
            "T3D notify count %d disagrees with UE Python notify count %d"
            % (len(actual), len(expected)))
    expected_sorted = sorted(expected, key=lambda item: (
        round(float(item["time"] or 0.0), 4), item["name"], item["kind"]))
    actual_sorted = sorted(actual, key=lambda item: (
        round(float(item["time"] or 0.0), 4), item["name"], item["kind"]))
    for expected_event, actual_event in zip(expected_sorted, actual_sorted):
        if expected_event["name"] != actual_event["name"]:
            raise UnsupportedMontageError(
                "notify name %r disagrees with UE Python %r"
                % (actual_event["name"], expected_event["name"]))
        if expected_event["kind"] != actual_event["kind"]:
            raise UnsupportedMontageError(
                "notify %r kind %r disagrees with UE Python %r"
                % (actual_event["name"], actual_event["kind"],
                   expected_event["kind"]))
        if abs(float(expected_event["time"]) - float(actual_event["time"])) > 1e-4:
            raise UnsupportedMontageError(
                "notify %r time disagrees with UE Python" % actual_event["name"])
        if abs(float(expected_event["duration"])
               - float(actual_event["duration"])) > 1e-4:
            raise UnsupportedMontageError(
                "notify %r duration disagrees with UE Python"
                % actual_event["name"])
    return []


def check_representable(entry: dict | None, asset_name: str) -> None:
    if entry is None:
        return
    summary = entry.get("summary") or {}
    unresolved = summary.get("unresolvedNotifyClasses") or []
    if unresolved:
        raise UnsupportedMontageError(
            "%s has notify state(s) whose class asset is missing from the "
            "isolated UE copy: %s" % (asset_name, ", ".join(unresolved)))


def check_t3d_representable(text: str, asset_name: str) -> None:
    """Reject T3D shapes that one vl.anim-montage.v1 document cannot hold.

    This check does not depend on the optional UE-side JSON report, so a direct
    conversion of a fresh T3D cannot silently produce a Montage that plays the
    wrong sequence or downgrades a notify state to an instant notify.
    """
    for line in text.splitlines():
        notify = re.match(r"[ \t]*Notifies\(\d+\)=\((.*)", line)
        if not notify:
            continue
        body = notify.group(1)
        name_match = re.search(r'NotifyName="([^"]*)"', body)
        name = name_match.group(1) if name_match else ""
        state = re.search(r"NotifyStateClass=(None|\"[^\"]*\")", body)
        plain = re.search(r"\bNotify=(None|\"[^\"]*\")", body)
        if state and state.group(1) != "None":
            continue
        if plain and plain.group(1) != "None":
            continue
        duration_match = re.search(r"\bDuration=(-?[0-9.]+)", body)
        duration = float(duration_match.group(1)) if duration_match else 0.0
        if duration > 0 or "NotifyState" in name:
            raise UnsupportedMontageError(
                "%s notify %r has no resolved class; refusing to guess "
                "notify vs notifyState" % (asset_name, name))


def parse_echo_segments(text: str, length: float, clips: list[str],
                        source_dir: Path, skeleton: dict,
                        skeleton_blake3: str,
                        resource_root: Path) -> tuple[list[dict], list[dict]]:
    """Read SlotAnimTracks.AnimSegments into timeline segments.

    T3D places a segment at ``StartPos``; its source span is
    ``[AnimStartTime, AnimEndTime]``. The Montage timeline duration is the source
    span divided by the asset-level RateScale semantics, so VL keeps
    ``RateScale`` on the Montage and segments use rateScale=1.
    """
    tracks = assignments(text, "SlotAnimTracks")
    if len(tracks) != 1:
        raise ExportError("Montage has %d SlotAnimTracks; one is expected"
                          % len(tracks))
    body = tracks[0][1]
    marker = re.search(r"AnimSegments\s*=\s*\(", body)
    if not marker:
        raise ExportError("SlotAnimTracks has no AnimSegments")
    segments_body, _ = balanced(body, marker.end() - 1)
    content = segments_body[1:-1]
    items = []
    cursor = 0
    while cursor < len(content):
        open_index = content.find("(", cursor)
        if open_index < 0:
            break
        item, cursor = balanced(content, open_index)
        items.append(item)
    if not items:
        raise ExportError("SlotAnimTracks has no AnimSegment")

    segments = []
    summaries = []
    previous_end = 0.0
    for index, item in enumerate(items):
        match = re.search(
            r'AnimReference="/Script/Engine\.AnimSequence\'([^\']+)\'"', item)
        if not match:
            raise ExportError("AnimSegment %d has no AnimReference" % index)
        package = match.group(1)
        clip_id = package.rsplit(".", 1)[-1]
        if clip_id not in clips:
            raise ExportError("Montage segment %d plays %r, which is not an "
                              "SKM_Echo skinning.clips id" % (index, clip_id))
        sequence_path = source_dir / (clip_id + ".anim.json")
        if not sequence_path.is_file():
            raise ExportError("Missing Sequence sidecar: " + str(sequence_path))
        sequence = load_json(sequence_path)
        if sequence.get("format") != "vl.anim-sequence.v1" or \
                sequence.get("sequence") != clip_id:
            raise ExportError("Sequence sidecar does not describe clip %r: %s"
                              % (clip_id, sequence_path))
        if sequence.get("skeleton") != skeleton["skeleton"]:
            raise ExportError("Sequence %r skeleton mismatch" % clip_id)
        if sequence.get("skeletonBlake3") != skeleton_blake3:
            raise ExportError("Sequence %r skeletonBlake3 mismatch" % clip_id)

        start_pos = property_float(item, "StartPos", 0.0)
        clip_start = property_float(item, "AnimStartTime", 0.0)
        clip_end = property_float(item, "AnimEndTime", float(sequence["duration"]))
        timeline_end = start_pos + (clip_end - clip_start)
        if clip_start < 0 or clip_end <= clip_start:
            raise ExportError("AnimSegment %d has an invalid source range" % index)
        if abs(start_pos - previous_end) > 1e-3:
            raise ExportError("AnimSegment %d does not start at the previous "
                              "segment end" % index)
        if timeline_end <= start_pos or timeline_end > length + 1e-3:
            raise ExportError("AnimSegment %d exceeds the Montage timeline" % index)
        segments.append({
            "id": "segment_%d" % index,
            "sequence": resource_relative(sequence_path, resource_root),
            "sequenceBlake3": blake3_file(sequence_path),
            "startTime": start_pos,
            "endTime": timeline_end,
            "clipStartTime": clip_start,
            "clipEndTime": clip_end,
            "rateScale": 1.0,
            "loop": False,
        })
        summaries.append({
            "index": index,
            "clip": clip_id,
            "duration": float(sequence["duration"]),
            "startTime": start_pos,
            "endTime": timeline_end,
            "clipStartTime": clip_start,
            "clipEndTime": clip_end,
        })
        previous_end = timeline_end

    if abs(previous_end - length) > 1e-3:
        raise ExportError("AnimSegments do not cover SequenceLength %s" % length)
    return segments, summaries


def parse_echo_montage(text: str, name: str, clips: list[str],
                       skeleton: dict, skeleton_blake3: str, source_dir: Path,
                       resource_root: Path, t3d_name: str) -> tuple[dict, dict]:
    if not text:
        raise ExportError("Empty T3D")
    length_match = re.search(r"\bSequenceLength=(-?[0-9]+(?:\.[0-9]+)?)", text)
    if not length_match:
        raise ExportError("Montage has no SequenceLength")
    length = float(length_match.group(1))
    if not math.isfinite(length) or length <= 0:
        raise ExportError("Montage SequenceLength must be > 0")
    tracks = {index: unquote(property_value(value, "TrackName") or "")
              for index, value in assignments(text, "AnimNotifyTracks")}
    warp_blocks = object_blocks(text, "AnimNotifyState_MotionWarping_")
    sections = parse_sections(text, length)
    # UE 5.8 runtime jumps to the next section's start when NextLink/NextTime is
    # absent. VL data defaults an empty linkMethod to Absolute/0, so make that
    # equivalence explicit instead of linking Default back to timeline zero.
    starts = {section["name"]: section["startTime"] for section in sections}
    for section in sections:
        target = section["nextSection"]
        if target and abs(section["nextTime"]) <= 1e-9 and starts.get(target, 0) > 0:
            section["nextTime"] = starts[target]
            section["linkMethod"] = "Absolute"
    slot = parse_slot(text, skeleton["slots"])
    notifies = parse_notifies(text, tracks, length, warp_blocks, strict_end_link=False)
    # T3D keeps the authored LinkValue and a separate TriggerTimeOffset. UE Python
    # reports the final trigger time, so the converter must add the offset rather
    # than silently dropping a one-frame boundary shift.
    raw_notifies = [value for _, value in sorted(assignments(text, "Notifies"))]
    if len(raw_notifies) != len(notifies):
        raise ExportError("T3D notify assignment count is inconsistent")
    for notify, raw in zip(notifies, raw_notifies):
        notify["time"] += property_float(raw, "TriggerTimeOffset", 0.0)
        if notify["time"] < 0 or notify["time"] > length + 1e-3:
            raise ExportError("Notify %r is outside the Montage after "
                              "TriggerTimeOffset" % notify["name"])
    rate_scale = property_float(text, "RateScale", 1.0)
    if rate_scale <= 0 or rate_scale > 4:
        raise ExportError("Montage RateScale must be in (0,4]")
    segments, segment_summary = parse_echo_segments(
        text, length, clips, source_dir, skeleton, skeleton_blake3, resource_root)

    package_match = re.search(
        r"Begin Object Class=/Script/Engine\.AnimMontage[^\n]*?"
        r'ExportPath="[^"]*\'([^\']+)\'"', text, re.S)
    document = {
        "format": "vl.anim-montage.v1",
        "montage": name,
        "slot": slot,
        "length": length,
        "sections": sections,
        "notifies": notifies,
        "source": {
            "t3d": t3d_name,
            "uePackage": package_match.group(1) if package_match else "",
            "exportedBy": "import_echo_montages.py",
        },
    }
    if len(segments) == 1:
        document["sequence"] = segments[0]["sequence"]
        document["sequenceBlake3"] = segments[0]["sequenceBlake3"]
    else:
        document["segments"] = segments
    if abs(rate_scale - 1.0) > 1e-9:
        document["rateScale"] = rate_scale

    summary = {
        "montage": document["montage"],
        "clips": [item["clip"] for item in segment_summary],
        "t3d": t3d_name,
        "length": length,
        "rateScale": rate_scale,
        "slot": slot,
        "sections": len(sections),
        "notifies": len(notifies),
        "warpNotifies": sum(1 for notify in notifies if "warp" in notify),
        "segmentCount": len(segments),
        "segments": segment_summary,
        "notifyKinds": sorted({notify["kind"] for notify in notifies}),
        "tracks": sorted({notify["track"] for notify in notifies}),
    }
    return document, summary


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
        if not clips:
            raise ExportError("SKM_Echo has no skinning.clips")

        assets = requested_assets(arguments)
        audit = audit_entries(arguments.audit.resolve())
        documents = []
        summaries = []
        skipped = []
        failed = []
        for asset_name in assets:
            entry = audit.get(asset_name)
            try:
                check_representable(entry, asset_name)
                t3d_path = find_t3d(montage_dir, asset_name)
                text = t3d_path.read_text(encoding="utf-8", errors="replace").lstrip("\ufeff")
                if montage_name(text) != asset_name:
                    raise ExportError("T3D ExportPath asset %r does not match %r"
                                      % (montage_name(text), asset_name))
                check_t3d_representable(text, asset_name)
                document, summary = parse_echo_montage(
                    text, vl_montage_id(asset_name), clips, skeleton,
                    skeleton_blake3, source_dir, resource_root, t3d_path.name)
                # Keep the UE asset name separate from the VL identity so the
                # original package can always be traced back.
                document["source"] = {
                    "t3d": t3d_path.name,
                    "uePackage": document["source"]["uePackage"],
                    "ueMontageAsset": asset_name,
                    "exportedBy": "import_echo_montages.py",
                }
                warnings = []
                if entry is not None:
                    first_duration = float(summary["segments"][0]["duration"])
                    warnings.extend(check_audit(
                        entry, document, summary["clips"], first_duration))
                    warnings.extend(check_python_events(entry, document))
                    python_slots = (entry.get("python") or {}).get("slots") or []
                    if python_slots and document["slot"] not in python_slots:
                        raise UnsupportedMontageError(
                            "converted slot is not in the UE slot list")
                    python_sections = (entry.get("python") or {}).get("sections") or []
                    if python_sections and python_sections != [
                            section["name"] for section in document["sections"]]:
                        raise UnsupportedMontageError(
                            "converted sections disagree with the UE section list")
                else:
                    warnings.append("no UE-side audit entry for this Montage")
                summary["warnings"] = warnings
                output = (resource_root / OUTPUT_RELATIVE
                          / (document["montage"] + ".json")).resolve()
                summary["output"] = resource_relative(output, resource_root)
                documents.append((output, document))
                summaries.append(summary)
            except UnsupportedMontageError as error:
                skipped.append({"montage": asset_name, "reason": str(error)})
                if not arguments.continue_on_error:
                    raise
            except (ExportError, OSError, ValueError, KeyError) as error:
                failed.append({"montage": asset_name, "error": str(error)})
                if not arguments.continue_on_error:
                    raise

        if not documents:
            raise ExportError("No Montage could be represented")
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
            "skipped": skipped,
            "failed": failed,
            "items": summaries,
        }, ensure_ascii=False, indent=2))
        return 1 if failed else 0
    except (ExportError, FileExistsError, OSError, ValueError, KeyError) as error:
        print("import_echo_montages.py: error: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

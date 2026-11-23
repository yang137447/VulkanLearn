"""External-cwd Game tour collector. Default is CPU-only planning, never acceptance."""

import argparse
from collections import Counter
from contextlib import nullcontext
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import threading
import time

from rvt_artifact_binding import read_bound_bytes
from rvt_delivery import require
from rvt_native_adapter import APPROVED_P0_SHA256
from rvt_p0_analyze import distribution, extract
from rvt_quality_binary import (
    decode_bound_capture, SAMPLE_OFFSET, CAPACITY, RESULT_OFFSET, OWNER_OFFSET, SURFACE_OFFSET,
    BYTES, MAGIC,
)
from rvt_run_conditions import process_conditions, quiet_host_audit


C008_CAMERA_SHA256 = "b3d8612073f51ce75823585ea976e0fdeb195d3ae2cf65864fdb44c89c4d8ed0"
PREFIXES = ("RVT_QUALITY_STATE", "RVT_QUALITY_EVENT", "RVT_QUALITY_CAPTURE",
            "RVT_QUALITY_FAILURE", "RVT_QUALITY_PRESENT", "CPU_RAW", "GPU_RAW")
MAX_BATCH = 8
CAPTURE_CLASSES = ("RVT_QUALITY_CAPTURE", "BOUND_CAPTURE_ARTIFACT")
DIAGNOSTIC_FEEDBACK_SHA256 = "276f25fd2320005460897fa4cdf36d3dd5bb73a13b1fd964602d59d20c28487f"


class PendingProducerArtifact(ValueError):
    pass


class PendingQualityState(PendingProducerArtifact):
    def __init__(self, metadata):
        self.identity = {field: metadata[field] for field in
                         ("worldGeneration", "poseId", "frame", "submissionEpoch", "completedEpoch")}
        super().__init__("Pending native quality state: " + json.dumps(self.identity))


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
                    encoding="utf-8")


def reference(path, root):
    root = root.resolve(strict=True)
    path = path.resolve(strict=True)
    require(path.is_relative_to(root), "Raw artifact escaped external evidence root")
    return {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
            "sha256": digest(path)}


def vector(value):
    require(isinstance(value, list) and len(value) == 3 and
            all(type(item) in (int, float) and math.isfinite(item) for item in value),
            "Invalid camera vector")
    return value


def camera_matches(shot, metadata):
    expected = vector(shot["position"])
    actual = vector(metadata["cameraPosition"])
    if max(abs(first - second) for first, second in zip(expected, actual)) > 0.25:
        return False
    target = vector(shot["look_at"])
    observed = vector(metadata["cameraLookAt"])
    desired_ray = [target[index] - expected[index] for index in range(3)]
    actual_ray = [observed[index] - actual[index] for index in range(3)]
    desired_length = math.sqrt(sum(value * value for value in desired_ray))
    actual_length = math.sqrt(sum(value * value for value in actual_ray))
    require(desired_length > 0 and actual_length > 0, "Degenerate camera direction")
    return sum(first * second for first, second in zip(desired_ray, actual_ray)) / (
        desired_length * actual_length) >= 0.999


def camera_command(shot):
    position, target = vector(shot["position"]), vector(shot["look_at"])
    require(position != target, "Camera target equals eye")
    return "camera pose " + " ".join(format(value, ".12g") for value in position + target)


def route(capture, selection, offset=0, count=4):
    shots = capture["shots"]
    require(len({shot["name"] for shot in shots}) == len(shots), "Duplicate camera labels")
    groups = {category: [shot for shot in shots if shot.get("category") == category]
              for category in ("center", "edge", "junction", "outer")}
    require([len(groups[key]) for key in groups] == [16, 24, 9, 4],
            "Camera lock must contain exactly 16 centers/24 edges/9 junctions/4 outer corners")
    named = {shot["name"]: shot for shot in shots}
    require({"far", "mid", "near", "tile00_near", "cross_tile_corner"} <= named.keys(),
            "Missing far/mid/near or movement/teleport cameras")
    if selection == "smoke":
        selected = [named["far"], named["near"], groups["edge"][0], groups["junction"][0]]
    elif selection == "core":
        selected = [named[name] for name in ("far", "mid", "near", "tile00_near", "cross_tile_corner")]
    elif selection == "continuous":
        selected = []
    elif selection == "teleport":
        selected = [named[name] for name in ("far", "near", "tile00_near", "cross_tile_corner")]
    else:
        require(offset >= 0 and 1 <= count <= MAX_BATCH, "Use bounded 1..8 pose batches")
        selected = [shot for shot in shots if shot.get("category") in groups][offset:offset + count]
        require(len(selected) == count, "Batch exceeds 53-pose coverage plan")
    for shot in selected:
        camera_command(shot)
    return selected


def package_inputs(package, mode):
    config = read(package / "config/config.json")
    require(config["resourcePath"] == "resources", "All Game inputs must be package-local")
    require(digest(package / "config/terrainRvtP0Frozen.json") == APPROVED_P0_SHA256,
            "Frozen P0 hash differs; thresholds cannot be changed")
    scene = read(package / "resources" / config["initScene"])
    placements = [item for item in scene["objects"] if item["type"] == "terrain"]
    require(len(placements) == 16, "Full16 Game required")
    names = set()
    for item in placements:
        terrain = read(package / "resources" / item["terrainPath"])
        names.add(terrain["compositedVT"]["packagePath"])
    require(len(names) == 1, "Full16 must share one V2 publication")
    manifest_path = package / "resources" / names.pop()
    require(manifest_path.resolve().is_relative_to(package), "Manifest escaped package")
    manifest = read(manifest_path)
    require(manifest["version"] == 2 and all(
        item["weightSource"] == {"CoverageAtlas": 0, "LocalLayerArray": 1}[mode]
        for item in manifest["production"]["surfaces"]), "Package weight source differs")
    surfaces = {item["surfaceId"]: item for item in manifest["surfaces"]}
    require(len(surfaces) == 16, "Missing Full16 surface identities")
    return config, surfaces, manifest_path


def inventory(package):
    files = {}
    for path in sorted(package.rglob("*")):
        require(not path.is_symlink(), "Installed Game cannot contain symlinks")
        if path.is_file():
            require(path.resolve().is_relative_to(package), "Installed input escaped package")
            files[path.relative_to(package).as_posix()] = {
                "bytes": path.stat().st_size, "sha256": digest(path)}
    return files


def audit_stage_delta(baseline, staged, approved):
    require(set(baseline) == set(staged), "Diagnostic stage added or removed package files")
    changed = {name for name in baseline if baseline[name] != staged[name]}
    require(changed == approved, "Diagnostic stage differs outside exact approved feedback delta: "
            + json.dumps(sorted(changed ^ approved)))
    return [{"path": name, "baseline": baseline[name], "diagnostic": staged[name]}
            for name in sorted(changed)]


def diagnostic_stage_audit(package, handoff_path):
    handoff_path = handoff_path.resolve(strict=True)
    handoff = read(handoff_path)
    prepared_path = handoff_path.with_name("prepared.json")
    prepared = read(prepared_path)
    baseline = Path(prepared["sourceStage"]).resolve(strict=True)
    require(package == Path(handoff["privateDiagnosticStage"]).resolve(strict=True) ==
            Path(prepared["privateTourr4Stage"]).resolve(strict=True) and package != baseline,
            "Diagnostic stage path differs from handoff")
    source_name = "shader/glsl/pass/terrainFeedback.frag"
    require(handoff["sourceAfterSha256"] == prepared["sourceAfterSha256"] ==
            DIAGNOSTIC_FEEDBACK_SHA256 == digest(package / source_name),
            "Diagnostic feedback source SHA256 differs from approved delta")
    require(handoff["sourceBeforeSha256"] == prepared["sourceBeforeSha256"] ==
            digest(baseline / source_name), "Baseline feedback source differs")
    old_text = (baseline / source_name).read_text(encoding="utf-8")
    new_text = (package / source_name).read_text(encoding="utf-8")
    require(old_text.count("int(ceil(compositedMip))") == 1 and
            old_text.replace("int(ceil(compositedMip))",
                             "min(int(floor(compositedMip)) + 1, int(compositedSurface.a))") == new_text,
            "Diagnostic shader changed beyond approved high-mip expression")
    cache_names = {name.replace("\\", "/") for name in prepared["cacheChangedFiles"]}
    require("shader-build-cache.json" in cache_names and len(cache_names) == 7 and
            all(name == "shader-build-cache.json" or
                name.startswith(("pass/terrainFeedback/", "pass/terrainFeedback_"))
                and name.endswith((".spv", ".debug")) for name in cache_names),
            "Handoff includes unapproved shader cache paths")
    approved = {source_name, *("shader/spv/" + name for name in cache_names)}
    base_inventory, stage_inventory = inventory(baseline), inventory(package)
    changes = audit_stage_delta(base_inventory, stage_inventory, approved)
    require(digest(package / "main_game.exe") == digest(baseline / "main_game.exe") ==
            handoff["gameSha256Unchanged"], "Diagnostic Game binary differs from approved original")
    compiled_path = handoff_path.with_name("graphics-cache.json")
    cached_path = handoff_path.with_name("graphics-cache-verification.json")
    compiled, cached = read(compiled_path), read(cached_path)
    require(compiled == prepared["actualGraphicsCompilerReceipt"] and
            len(compiled) == len(cached) == len(handoff["requiredFeedbackGraphicsCacheUpdates"]) == 2,
            "Missing actual compiler/cache-hit receipts for both variants")
    old_cache = read(baseline / "shader/spv/shader-build-cache.json")
    new_cache = read(package / "shader/spv/shader-build-cache.json")
    require(set(old_cache) == set(new_cache) and all(
        old_cache[key] == new_cache[key] for key in old_cache if key != "artifacts") and
        set(old_cache["artifacts"]) == set(new_cache["artifacts"]),
        "Diagnostic changed cache schema or unrelated artifact inventory")
    changed_entries = {key: row for key, row in new_cache["artifacts"].items()
                       if row != old_cache["artifacts"][key]}
    require(len(changed_entries) == 2, "Diagnostic must update exactly two feedback cache entries")
    variants = []
    for expected in handoff["requiredFeedbackGraphicsCacheUpdates"]:
        matches = [row for row in changed_entries.values() if (
            "|shadingModel=" + expected["shadingModel"] + "|") in row["normalizedKey"]]
        require(len(matches) == 1, "Missing expected feedback shading-model cache entry")
        entry = matches[0]
        require("|shaderName=pass/terrainFeedback|" in entry["normalizedKey"] and
                entry["sourceFingerprint"] == expected["sourceFingerprint"],
                "Feedback cache source identity differs from handoff")
        build = next((row for row in compiled if row["normalizedKey"] == entry["normalizedKey"]), None)
        hit = next((row for row in cached if row["normalizedKey"] == entry["normalizedKey"]), None)
        require(build and hit and build["committed"] is True and build["cacheHit"] is False and
                build["shadercInvocations"] == expected["actualShadercInvocations"] == 4 and
                hit["committed"] is True and hit["cacheHit"] is True and hit["shadercInvocations"] == 0 and
                all(row[field] == expected[field] for row in (build, hit)
                    for field in ("sourceFingerprint", "artifactGenerationKey")),
                "Actual compile/cache-hit receipts differ from handoff identity")
        outputs = []
        for role, item in entry["outputs"].items():
            path = (package / "shader" / item["path"]).resolve(strict=True)
            require(path.is_relative_to(package / "shader/spv"), "Feedback cache output escaped stage")
            outputs.append({"role": role, "path": item["path"], "bytes": path.stat().st_size,
                            "sha256": digest(path), "cacheDigest": item["digest"]})
        variants.append({"shadingModel": expected["shadingModel"], "sourceFingerprint": entry["sourceFingerprint"],
                         "artifactGenerationKey": expected["artifactGenerationKey"], "outputs": outputs})

    def inventory_hash(value):
        return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()

    return {"diagnosticStage": True, "scope": "Private feedback-only diagnostic; not a newly verified installation",
            "installationVerified": False,
            "installationManifestScope": handoff["finalPackageManifestStatus"],
            "handoff": {"path": str(handoff_path), "sha256": digest(handoff_path)},
            "preparedReceipt": {"path": str(prepared_path), "sha256": digest(prepared_path)},
            "compilerReceiptSha256": digest(compiled_path), "cacheHitReceiptSha256": digest(cached_path),
            "baselinePackage": str(baseline), "fileCount": len(stage_inventory),
            "unchangedFileCount": len(stage_inventory) - len(changes),
            "baselineInventorySha256": inventory_hash(base_inventory),
            "stageInventorySha256": inventory_hash(stage_inventory),
            "approvedDelta": changes, "sourceSha256": digest(package / source_name),
            "cacheSha256": digest(package / "shader/spv/shader-build-cache.json"),
            "variants": variants, "rendererStarted": False}


class NativeStream:
    def __init__(self, output):
        self.output = output
        self.records = []
        self.states = {}
        self.submitted = set()
        self.offsets = {}
        self.pending = {}
        self.acknowledgements = 0
        self.saved_screenshots = {}
        self.invalid_lines = []
        self.unpaired_completions = {}

    def poll(self, final=False, strict=True):
        for filename in ("stdout.log", "stderr.log"):
            path = self.output / filename
            if not path.exists():
                continue
            with path.open("rb") as stream:
                stream.seek(self.offsets.get(filename, 0))
                data = self.pending.get(filename, b"") + stream.read()
                self.offsets[filename] = stream.tell()
            lines = data.split(b"\n")
            self.pending[filename] = lines.pop()
            for encoded in lines:
                self.line(encoded.decode("utf-8", errors="replace"))
            if final and self.pending[filename]:
                self.defer_line_error(self.pending.pop(filename).decode("utf-8", errors="replace"),
                                      "Terminal log has no terminating newline")
        if strict:
            self.check_pending_log(final)

    def defer_line_error(self, line, reason):
        self.invalid_lines.append({"text": line, "reason": reason,
                                   "firstObservedMonotonic": time.monotonic()})

    def check_pending_log(self, final=False):
        expired = bool(self.invalid_lines) and (
            final or time.monotonic() - self.invalid_lines[0]["firstObservedMonotonic"] >= 12)
        require(not expired, "Permanently invalid native log after terminal/12s timeout: "
                + json.dumps(self.invalid_lines, ensure_ascii=False))
        if final:
            require(not self.unpaired_completions,
                    "Permanently missing native capture submission: "
                    + str(list(self.unpaired_completions)))

    def line(self, line):
        if "Camera pose set: position=" in line:
            self.acknowledgements += 1
        if "Screenshot saved: " in line:
            path = Path(line.split("Screenshot saved: ", 1)[1].strip()).resolve()
            self.saved_screenshots[str(path)] = {"logText": line,
                                                "observerMonotonic": time.monotonic()}
        require(not any(marker in line for marker in (
            "[RVT_QUALITY_FAILURE]", "[RVT_QUALITY_INCOMPLETE]", "Terrain.QualityConvergenceFailed",
            "VUID-", "Validation Error", "Unknown command", "ErrorDeviceLost",
            "Shader cache miss:", "EngineLoop init failed", "Render thread failed")),
            "Native failure: " + line[:800])
        for prefix in PREFIXES:
            marker = "[" + prefix + "] "
            if marker not in line:
                continue
            payload = line.split(marker, 1)[1].strip()
            if prefix == "RVT_QUALITY_CAPTURE":
                match = re.fullmatch(
                    r"frame=(\d+) epoch=(\d+) binary=(.+) visible=(\d+) complete=(\d+)", payload)
                if match is None:
                    self.defer_line_error(line, "Malformed native capture record")
                    continue
                frame, epoch, path, visible, complete = match.groups()
                record = {"frame": int(frame), "submissionEpoch": int(epoch), "binary": path,
                          "visible": int(visible), "complete": int(complete)}
            else:
                try:
                    record = json.loads(payload)
                    require(isinstance(record, dict), "Native log JSON must be an object")
                    if prefix == "RVT_QUALITY_STATE" or (
                        prefix == "RVT_QUALITY_EVENT" and record.get("event") in (
                            "capture_submitted", "capture_completed")):
                        require(all(field in record for field in
                                    ("worldGeneration", "poseId", "submissionEpoch")),
                                "Native log identity fields missing")
                    if prefix == "RVT_QUALITY_EVENT":
                        require(isinstance(record.get("event"), str), "Native event name missing")
                    if prefix == "RVT_QUALITY_STATE":
                        require(all(field in record for field in (
                            "completedEpoch", "visible", "validAtRawTargetPlusOne", "unresolved",
                            "incorrectIdentity", "rootOnly", "elapsedMs", "interactive", "convergenceFailed")),
                            "Native quality state payload incomplete")
                except (ValueError, TypeError) as error:
                    self.defer_line_error(line, str(error))
                    continue
            self.accept_record(prefix, record)

    def accept_record(self, prefix, payload, provenance=None):
        record = {**payload, **(provenance or {}), "nativePrefix": prefix,
                  "observerMonotonic": time.monotonic()}
        self.records.append(record)
        if prefix == "RVT_QUALITY_STATE":
            key = (record["worldGeneration"], str(record["poseId"]), record["submissionEpoch"])
            require(key not in self.states, "Duplicate quality state epoch")
            self.states[key] = record
        if prefix == "RVT_QUALITY_EVENT" and record["event"] in (
                "capture_submitted", "capture_completed"):
            key = (record["worldGeneration"], str(record["poseId"]), record["submissionEpoch"])
            if record["event"] == "capture_submitted":
                require(not self.submitted, "More than one globally in-flight quality capture")
                if key in self.unpaired_completions:
                    self.unpaired_completions.pop(key)
                else:
                    self.submitted.add(key)
            else:
                if key not in self.submitted:
                    self.unpaired_completions[key] = record
                else:
                    self.submitted.remove(key)


def strict_json_bytes(data):
    def pairs(items):
        value = {}
        for key, item in items:
            require(key not in value, "Duplicate JSON object key")
            value[key] = item
        return value

    def constant(value):
        raise ValueError("Nonfinite JSON token: " + value)

    return json.loads(data.decode("utf-8", errors="strict"), object_pairs_hook=pairs,
                      parse_constant=constant)


class RuntimeEvidenceStream(NativeStream):
    dedicated = True

    def __init__(self, output, artifact_capture=True):
        super().__init__(output)
        self.path = output / "runtime-events.jsonl"
        self.envelopes = []
        self.sink_bytes = 0
        self.sink_offset = 0
        self.sink_partial = b""
        self.sink_hash = hashlib.sha256()
        self.footer = None
        self.completed = {}
        self.bound_artifacts = set()
        self.artifact_capture = artifact_capture
        self.pending_artifacts = {}
        self.lifecycle_submissions = {}
        self.lifecycle_ends = {}
        self.aborted = {}
        self.unsubmitted_aborts = {}

    def accept_record(self, prefix, payload, provenance=None):
        event = payload.get("event") if prefix == "RVT_QUALITY_EVENT" else None
        lifecycle = event in ("capture_submitted", "capture_completed", "capture_aborted")
        if not lifecycle:
            return super().accept_record(prefix, payload, provenance)
        key = (payload["worldGeneration"], str(payload["poseId"]), payload["submissionEpoch"])
        require(key not in self.aborted, "Capture event after exact capture abort")
        full = any(field in payload for field in
                   ("captureHandle", "drawSubmissionEpoch", "submitted", "measurementProvided"))
        identity_fields = ("captureHandle", "worldGeneration", "poseId", "frame", "drawSubmissionEpoch")
        if full or event == "capture_aborted":
            require(all(type(payload.get(field)) is int and payload[field] >= (
                0 if field == "frame" else 1) for field in identity_fields),
                "Lifecycle recorded capture identity fields missing/invalid")
            require(type(payload.get("submissionEpoch")) is int and
                    type(payload.get("submitted")) is bool and
                    type(payload.get("measurementProvided")) is bool,
                    "Lifecycle submission/measurement flags missing")
        if event == "capture_aborted":
            require(payload["measurementProvided"] is False and "completedEpoch" not in payload and
                    isinstance(payload.get("reason"), str) and bool(payload["reason"]),
                    "Abort must have actual reason and no measurement/completed epoch")
            identity = tuple(payload[field] for field in identity_fields)
            if payload["submitted"]:
                require(payload["submissionEpoch"] == payload["drawSubmissionEpoch"] and
                        key in self.submitted and key in self.lifecycle_submissions,
                        "Abort does not match an actual pending submission")
                original = self.lifecycle_submissions[key]
                require(all(field in original and original[field] == payload[field]
                            for field in identity_fields), "Abort recorded identity differs from submit")
                require(key not in self.completed and key not in self.lifecycle_ends,
                        "Abort after completed/used raw or double end")
                super().accept_record(prefix, payload, provenance)
                self.submitted.remove(key)
                self.aborted[key] = payload
                self.lifecycle_ends[key] = payload
            else:
                require(payload["submissionEpoch"] == 0 and identity not in self.unsubmitted_aborts,
                        "Unsubmitted abort has nonzero submission or duplicate end")
                require((payload["worldGeneration"], str(payload["poseId"]),
                         payload["drawSubmissionEpoch"]) not in self.lifecycle_submissions,
                        "Unsubmitted abort cannot close an existing submission")
                super().accept_record(prefix, payload, provenance)
                self.unsubmitted_aborts[identity] = payload
            return
        if event == "capture_submitted":
            require(key not in self.lifecycle_submissions and key not in self.lifecycle_ends,
                    "Duplicate submit or submit after capture end")
            if full:
                require(payload["submitted"] is True and payload["measurementProvided"] is False and
                        payload["submissionEpoch"] == payload["drawSubmissionEpoch"],
                        "Submitted capture flags/actual epoch differ")
                require(tuple(payload[field] for field in identity_fields) not in self.unsubmitted_aborts,
                        "Submit after unsubmitted capture abort")
            super().accept_record(prefix, payload, provenance)
            self.lifecycle_submissions[key] = payload
        else:
            require(key not in self.lifecycle_ends and key not in self.completed,
                    "Double capture end/completion")
            original = self.lifecycle_submissions.get(key)
            require(not original or "captureHandle" not in original or full,
                    "Completion cannot drop recorded lifecycle identity/measurement flags")
            if full:
                require(original is not None and key in self.submitted and
                        all(field in original and original[field] == payload[field]
                            for field in identity_fields), "Completion recorded identity differs from submit")
                require(payload["submitted"] is True and payload["measurementProvided"] is True and
                        type(payload.get("completedEpoch")) is int and
                        payload["completedEpoch"] >= payload["submissionEpoch"],
                        "Completion requires actual fenced measurement")
            super().accept_record(prefix, payload, provenance)
            self.lifecycle_ends[key] = payload

    def poll(self, final=False, strict=True):
        if self.path.exists():
            require(not self.path.is_symlink(), "Dedicated sink cannot be a symlink")
            size = self.path.stat().st_size
            require(self.sink_offset <= size <= 128 * 1024 * 1024, "Dedicated sink truncated/over capacity")
            with self.path.open("rb") as source:
                source.seek(self.sink_offset)
                added = source.read()
            self.sink_offset += len(added)
            self.sink_hash.update(added)
            lines = (self.sink_partial + added).split(b"\n")
            self.sink_partial = lines.pop()
            require(len(self.sink_partial) <= 65536, "Dedicated sink line exceeds capacity")
            for line in lines:
                self.consume_line(line + b"\n")
        self.poll_console()
        if self.artifact_capture:
            self.discover_captures()
        if final:
            require(not self.sink_partial, "Dedicated sink ends with a partial/non-newline record")
            require(self.footer is not None, "Dedicated sink missing runtimeEvidenceEnd footer")
            require(not self.unpaired_completions and not self.submitted,
                    "Dedicated sink missing capture submission/completion pair")
            require(not self.pending_artifacts, "Dedicated sink raw artifacts permanently incomplete: "
                    + json.dumps(self.pending_artifacts))
            require(digest(self.path) == self.sink_hash.hexdigest(),
                    "Dedicated sink bytes changed after observation")

    def consume_line(self, encoded):
        require(len(encoded) <= 65536 and len(self.envelopes) < 1000000,
                "Dedicated sink exceeds native record/line limits")
        require(self.footer is None, "Record after runtimeEvidenceEnd footer")
        record = strict_json_bytes(encoded)
        require(type(record) is dict, "Dedicated sink envelope must be an object")
        sequence = record.get("sequence")
        require(type(sequence) is int and sequence == len(self.envelopes) + 1,
                "Dedicated sink sequence must start at1 and be continuous")
        require(type(record.get("steady_ns")) is int and record["steady_ns"] > 0 and
                type(record.get("threadTag")) is int and record["threadTag"] > 0 and
                record.get("outputCategory") == "explicitoutput" and
                isinstance(record.get("eventClass"), str) and bool(record["eventClass"]) and
                type(record.get("payload")) is dict, "Dedicated sink envelope fields invalid")
        require(not self.envelopes or record["steady_ns"] >= self.envelopes[-1]["steady_ns"],
                "Dedicated sink steady clock regressed")
        event_class, payload = record["eventClass"], record["payload"]
        if event_class == "runtimeEvidenceEnd":
            require(payload.get("incomplete") is False and
                    type(payload.get("recordsBeforeTerminal")) is int and
                    payload["recordsBeforeTerminal"] == len(self.envelopes) and
                    type(payload.get("bytesBeforeTerminal")) is int and
                    payload["bytesBeforeTerminal"] == self.sink_bytes,
                    "Dedicated terminal incomplete/record count/byte count mismatch")
            self.footer = record
        self.envelopes.append(record)
        self.sink_bytes += len(encoded)
        require(event_class not in ("RVT_QUALITY_FAILURE", "RVT_QUALITY_INCOMPLETE"),
                "Native failure in dedicated sink: " + json.dumps(record))
        if event_class != "runtimeEvidenceEnd":
            require(event_class != "RVT_QUALITY_CAPTURE" or all(
                key in payload for key in ("frame", "submissionEpoch", "binary", "visible", "complete")),
                "Dedicated RVT_QUALITY_CAPTURE missing raw artifact fields")
            self.accept_record(event_class, payload, {
                "sinkSequence": sequence, "sinkSteadyNs": record["steady_ns"],
                "authority": "dedicated-runtime-evidence"})
            if event_class == "RVT_QUALITY_EVENT" and payload.get("event") == "capture_completed":
                key = (payload["worldGeneration"], str(payload["poseId"]), payload["submissionEpoch"])
                self.completed[key] = record

    def poll_console(self):
        for filename in ("stdout.log", "stderr.log"):
            path = self.output / filename
            if not path.exists():
                continue
            with path.open("rb") as source:
                source.seek(self.offsets.get(filename, 0))
                data = self.pending.get(filename, b"") + source.read()
                self.offsets[filename] = source.tell()
            lines = data.split(b"\n")
            self.pending[filename] = lines.pop()
            for encoded in lines:
                line = encoded.decode("utf-8", errors="replace").rstrip("\r")
                marker = "[Diagnostics][Info] Screenshot saved: "
                if line.startswith(marker):
                    path = Path(line[len(marker):]).resolve()
                    self.saved_screenshots[str(path)] = {
                        "logText": line, "observerMonotonic": time.monotonic(),
                        "authority": "console-writer-completion-only; no structured evidence fallback"}

    def discover_captures(self):
        paths = list((self.output / "quality").glob("capture-*.json"))
        require(len(paths) <= 256, "Raw capture count exceeds unchanged native256 cap")
        for path in paths:
            if str(path) in self.bound_artifacts:
                continue
            try:
                entry = reference(path, self.output)
                metadata = strict_json_bytes(read_bound_bytes(self.output, entry, 65536))
                key = (metadata["worldGeneration"], str(metadata["poseId"]), metadata["submissionEpoch"])
                if key not in self.completed:
                    self.pending_artifacts[path.name] = "Awaiting exact dedicated capture_completed event"
                    continue
                if any(row["nativePrefix"] == "RVT_QUALITY_CAPTURE" and
                       row["submissionEpoch"] == key[2] and row.get("frame") == metadata["frame"]
                       for row in self.records):
                    self.bound_artifacts.add(str(path))
                    self.pending_artifacts.pop(path.name, None)
                    self.pending_artifacts.pop(f"epoch:{key[2]}", None)
                    continue
                record = {"frame": metadata["frame"], "submissionEpoch": metadata["submissionEpoch"],
                          "binary": str(path.with_suffix(".bin")), "visible": 0, "complete": 0}
                bound_metadata, _, _, data = capture_metadata(record, self)
                words = struct.unpack_from("<64I", data)
                record["visible"] = words[3]
                record["complete"] = int(words[3] == words[6] and
                                         all(words[index] == 0 for index in (4, 5, 7, 21, 22)))
                native = self.completed[key]
                self.accept_record("BOUND_CAPTURE_ARTIFACT", record, {
                    "authority": "runner-work-item-from-raw-header-and-dedicated-completion; not a native event",
                    "sinkSequence": native["sequence"], "worldGeneration": bound_metadata["worldGeneration"],
                    "poseId": bound_metadata["poseId"],
                    "completeScope": "Raw fixed ABI flags; full independent decoder still mandatory"})
                self.bound_artifacts.add(str(path))
                self.pending_artifacts.pop(path.name, None)
                self.pending_artifacts.pop(f"epoch:{key[2]}", None)
            except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
                self.pending_artifacts[path.name] = str(error)
        if self.footer is not None:
            matched = {row["submissionEpoch"] for row in self.records
                       if row["nativePrefix"] in CAPTURE_CLASSES}
            for key in self.completed:
                if key[2] not in matched:
                    self.pending_artifacts[f"epoch:{key[2]}"] = "Completed capture has no bound raw export"

    def integrity(self):
        return {"source": str(self.path), "authority": "RuntimeEvidence JSONL only",
                "envelopeCount": len(self.envelopes), "parsedBytes": self.sink_bytes,
                "sha256": digest(self.path) if self.path.exists() else None,
                "footer": self.footer, "pendingArtifacts": self.pending_artifacts,
                "abortedCaptures": list(self.aborted.values()),
                "unsubmittedAborts": list(self.unsubmitted_aborts.values()),
                "stdoutStructuredFallback": False}


def capture_metadata(record, stream):
    require(record["binary"] != "not-exported", "Missing raw quality: original capture cap exhausted")
    binary_path = Path(record["binary"])
    require(binary_path.resolve().is_relative_to(stream.output.resolve()),
            "Raw artifact escaped external evidence root")
    try:
        binary_entry = reference(binary_path, stream.output)
        metadata_entry = reference(binary_path.with_suffix(".json"), stream.output)
        metadata = json.loads(read_bound_bytes(stream.output, metadata_entry, 65536).decode("utf-8"))
        require(isinstance(metadata, dict) and all(field in metadata for field in (
            "moduleOnly", "frame", "poseId", "worldGeneration", "submissionEpoch", "completedEpoch",
            "cameraPosition", "cameraLookAt", "completedMonotonicMs", "binaryAbi", "surfaceIds",
            "protocol", "source")),
            "Quality metadata is incomplete")
        data = read_bound_bytes(stream.output, binary_entry)
        require(len(data) >= 256, "Missing raw quality header")
        words = struct.unpack_from("<64I", data)
        require(words[0] == MAGIC and words[1] in (1, 2) and
                len(data) == BYTES + (12288 if words[1] == 2 else 0),
                "Raw ABI byte count/version not complete")
        for field, offset in (("frame", 8), ("poseId", 10), ("worldGeneration", 12), ("submissionEpoch", 14)):
            require(str(words[offset] | words[offset + 1] << 32) == str(metadata[field]),
                    "Raw capture/metadata frame pose epoch differs")
        require(record["frame"] == metadata["frame"] and
                record["submissionEpoch"] == metadata["submissionEpoch"], "Capture/log frame epoch differs")
        require(reference(binary_path, stream.output) == binary_entry and
                reference(binary_path.with_suffix(".json"), stream.output) == metadata_entry,
                "Producer artifacts changed during stable read")
    except (OSError, ValueError, KeyError, TypeError, UnicodeError, struct.error) as error:
        raise PendingProducerArtifact("Pending producer JSON/bin: " + str(error)) from error
    require(metadata.get("moduleOnly") is False, "Module-only capture is not Game evidence")
    return metadata, binary_entry, metadata_entry, data


def decode_capture(record, stream, surfaces):
    metadata, binary_entry, metadata_entry, data = capture_metadata(record, stream)
    key = (metadata["worldGeneration"], str(metadata["poseId"]), metadata["submissionEpoch"])
    require(not getattr(stream, "dedicated", False) or key not in stream.aborted,
            "Aborted capture cannot provide raw measurement or ready evidence")
    if getattr(stream, "dedicated", False) and key not in stream.completed:
        raise PendingProducerArtifact("Pending exact dedicated capture_completed for raw epoch")
    if getattr(stream, "dedicated", False):
        completion = stream.completed[key]["payload"]
        if "captureHandle" in completion:
            require(completion["frame"] == metadata["frame"] and
                    completion["drawSubmissionEpoch"] == metadata["submissionEpoch"] and
                    completion["completedEpoch"] == metadata["completedEpoch"] and
                    completion["measurementProvided"] is True,
                    "Native completed measurement identity differs from raw metadata")
    if key not in stream.states:
        raise PendingQualityState(metadata)
    state = stream.states[key]
    if state["completedEpoch"] != metadata["completedEpoch"]:
        raise PendingProducerArtifact("Pending native state/raw completed epoch differs")
    require(record["visible"] == state["visible"], "Capture/state denominator differs")
    bias = 0
    for pixel in range(CAPACITY):
        sample = struct.unpack_from("<6f2I", data, SAMPLE_OFFSET + pixel * 32)
        if sample[7]:
            bias = max(bias, sample[5])
    event = {**metadata, "visibleSamples": state["visible"],
             "validAtTargetPlusBias": state["validAtRawTargetPlusOne"],
             "effectiveMipBias": bias, "unresolved": state["unresolved"],
             "incorrectIdentity": state["incorrectIdentity"], "rootOnlySamples": state["rootOnly"],
             "uninitializedDraws": struct.unpack_from("<I", data, 28)[0]}
    counts = decode_bound_capture(data, metadata, event, surfaces)
    require(counts["incorrectIdentity"] == 0 and counts["uninitializedDraws"] == 0,
            "Raw identity failure or uninitialized terrain draw")
    elapsed = state["elapsedMs"]
    require(math.isfinite(elapsed) and elapsed >= 0, "Invalid native pose elapsed time")
    return {"metadata": metadata, "state": state, "counts": counts,
            "samplesArtifact": binary_entry, "metadataArtifact": metadata_entry,
            "complete": record["complete"] == 1,
            "arrivedMonotonicMs": metadata["completedMonotonicMs"] - elapsed,
            "arrivalScope": "Derived from native completedMonotonicMs minus same-epoch Service elapsedMs; "
                            "not observer sleep or an invented arrival event"}


class Tour:
    def __init__(self, stream, surfaces, alive, inject, trace):
        self.stream, self.surfaces = stream, surfaces
        self.alive, self.inject, self.trace = alive, inject, trace
        self.consumed = 0
        self.last_key = None
        self.results = []
        self.terminal = False

    def wait_pose(self, shot, initial=False, witness_only=False, min_world_generation=None):
        require(not self.terminal, "Stop-on-first-failure: tour is terminal")
        start = time.monotonic()
        ack_before = self.stream.acknowledgements
        if not initial:
            self.inject(camera_command(shot))
        pending_state = None
        drained_terminal = False
        try:
            while True:
                self.stream.poll()
                while self.consumed < len(self.stream.records):
                    record = self.stream.records[self.consumed]
                    self.consumed += 1
                    if record["nativePrefix"] not in CAPTURE_CLASSES:
                        continue
                    try:
                        metadata, _, _, _ = capture_metadata(record, self.stream)
                    except PendingProducerArtifact as pending:
                        pending_state = pending
                        self.consumed -= 1
                        break
                    key = (metadata["worldGeneration"], str(metadata["poseId"]))
                    if min_world_generation is not None and key[0] < min_world_generation:
                        continue
                    if not initial and self.last_key is not None and (
                        key[0] < self.last_key[0] or key[0] == self.last_key[0] and
                        int(key[1]) <= int(self.last_key[1])):
                        self.trace({"kind": "ignored-expired-capture", "frame": metadata["frame"],
                                    "poseId": metadata["poseId"],
                                    "worldGeneration": metadata["worldGeneration"],
                                    "submissionEpoch": metadata["submissionEpoch"]})
                        continue
                    if not camera_matches(shot, metadata):
                        continue
                    try:
                        capture = decode_capture(record, self.stream, self.surfaces)
                    except PendingProducerArtifact as pending:
                        pending_state = pending
                        self.consumed -= 1
                        break
                    pending_state = None
                    state, counts = capture["state"], capture["counts"]
                    command_observed = initial or self.stream.acknowledgements > ack_before or (
                        getattr(self.stream, "dedicated", False) and self.last_key is not None and
                        (key[0] > self.last_key[0] or int(key[1]) > int(self.last_key[1])))
                    require(command_observed,
                            "Missing actual CommandBus camera execution acknowledgement")
                    require(initial or record["observerMonotonic"] >= start, "Expired pose observation")
                    elapsed = state["elapsedMs"]
                    require(elapsed <= 10000, "Pose exceeded frozen 10s convergence deadline")
                    ready = capture["complete"] and state["interactive"] is True and (
                        counts["visibleSamples"] > 0 and
                        counts["validAtTargetPlusBias"] / counts["visibleSamples"] >= 0.99 and
                        counts["effectiveMipBias"] <= 1 and counts["unresolved"] == 0 and
                        counts["rootOnlySamples"] == 0 and state["convergenceFailed"] is False)
                    if ready or witness_only:
                        self.last_key = key
                        result = {"label": shot["name"], "category": shot.get("category", "overview"),
                                  "commandAcknowledged": command_observed,
                                  "commandExecutionScope": (
                                      "Actual matching new camera/pose raw plus dedicated native state"
                                      if getattr(self.stream, "dedicated", False) else "Legacy console echo"),
                                  "qualityReady": ready, "witnessOnly": witness_only, **capture}
                        self.results.append(result)
                        self.trace({"kind": "observed-pose", **result})
                        return result
                alive = self.alive()
                expired = time.monotonic() - start >= (86.748 if initial else 12)
                if not alive or expired:
                    if not drained_terminal:
                        self.stream.poll(final=not alive)
                        drained_terminal = True
                        continue
                    require(pending_state is None,
                            "Permanently missing native quality state or invalid producer artifact "
                            "after terminal/timeout and final log drain: "
                            + str(pending_state))
                    require(alive, "Game exited before required pose quality")
                    require(not expired, "Missing quality/arrival evidence or pose timeout")
                time.sleep(0.03)
        except Exception:
            self.terminal = True
            raise


def replay(options, plan, surfaces):
    source = options.replay.resolve(strict=True)
    output = options.output.resolve()
    require(not output.is_relative_to(source) and not source.is_relative_to(output),
            "Replay output must be separate from immutable original evidence")
    originals = {path.name: digest(path) for path in (
        source / "stdout.log", source / "stderr.log", source / "result.json",
        source / "native-observations.json")}
    original_inventory = inventory(source)
    stream = RuntimeEvidenceStream(source) if (source / "runtime-events.jsonl").exists() else NativeStream(source)
    stream.poll(final=True, strict=False)
    shots = read(options.capture_plan)["shots"]
    poses, pending, captures = {}, [], []
    for record in stream.records:
        if record["nativePrefix"] not in CAPTURE_CLASSES:
            continue
        metadata, _, _, _ = capture_metadata(record, stream)
        key = (metadata["worldGeneration"], str(metadata["poseId"]))
        labels = [shot["name"] for shot in shots if camera_matches(shot, metadata)]
        try:
            capture = decode_capture(record, stream, surfaces)
        except PendingQualityState as error:
            pending.append({**error.identity, "cameraLabels": labels})
            continue
        counts, state = capture["counts"], capture["state"]
        ready = capture["complete"] and state["interactive"] is True and (
            state["elapsedMs"] <= 10000 and counts["visibleSamples"] > 0 and
            counts["validAtTargetPlusBias"] / counts["visibleSamples"] >= 0.99 and
            counts["effectiveMipBias"] <= 1 and counts["unresolved"] == 0 and
            counts["rootOnlySamples"] == 0 and state["convergenceFailed"] is False)
        entry = poses.setdefault(key, {"worldGeneration": key[0], "poseId": key[1],
                                      "cameraLabels": labels, "matchedRawCaptures": 0,
                                      "readyObserved": False})
        entry["matchedRawCaptures"] += 1
        entry["lastFrame"] = metadata["frame"]
        entry["lastSubmissionEpoch"] = metadata["submissionEpoch"]
        entry["lastElapsedMs"] = state["elapsedMs"]
        entry["lastCounts"] = counts
        if ready and not entry["readyObserved"]:
            entry["firstReady"] = capture
        entry["readyObserved"] |= ready
        captures.append({"cameraLabels": labels, "qualityReady": ready, **capture})
    for row in pending:
        row["supersededByObservedPose"] = any(
            world == row["worldGeneration"] and int(pose) > int(row["poseId"])
            for world, pose in poses)
    require(all(digest(source / name) == value for name, value in originals.items()),
            "Original evidence changed during offline replay")
    output.mkdir(parents=True, exist_ok=False)
    result = {"status": "offline-replayed", "rendererStarted": False, "acceptancePassed": False,
              "source": str(source), "sourceHashes": originals, "sourceUnchanged": True,
              "poses": list(poses.values()), "unmatchedRawCaptures": pending,
              "scope": "Existing raw/log only; collected nonready observations are not pose convergence"}
    original_plan = read(source / "plan.json")
    labels_observed = {label for pose in poses.values() if pose["readyObserved"]
                       for label in pose["cameraLabels"]}
    result.update(
        originalResultPreserved=True,
        originalResultStatus=read(source / "result.json")["status"],
        invalidNativeLines=stream.invalid_lines,
        unmatchedCompletionEvents=list(stream.unpaired_completions.values()),
        logProtocolComplete=not stream.invalid_lines and not stream.unpaired_completions,
        requestedLabels=[shot["name"] for shot in original_plan["shots"]],
        requestedLabelsWithReadyRaw=[shot["name"] for shot in original_plan["shots"]
                                    if shot["name"] in labels_observed],
        requestedLabelsWithoutReadyRaw=[shot["name"] for shot in original_plan["shots"]
                                       if shot["name"] not in labels_observed])
    result["terminalScreenshots"] = []
    for shot in original_plan["shots"]:
        path = source / (shot["name"] + ".bmp")
        if path.exists():
            proof = completed_screenshot(path, stream, read(source / "process-conditions.json")["resolution"])
            result["terminalScreenshots"].append({"label": shot["name"], "proof": proof})
    require(all(digest(source / name) == value for name, value in originals.items()),
            "Original evidence changed during recovery audit")
    require(inventory(source) == original_inventory,
            "Original raw/images/logs changed during recovery audit")
    result["originalArtifactInventory"] = original_inventory
    result["collectorSha256"] = digest(Path(__file__))
    result["observedPoseArrivals"] = [row for row in stream.records
                                     if row.get("event") == "pose_arrived"]
    result["structuredEvidenceSource"] = "dedicated" if getattr(stream, "dedicated", False) else "legacy-archive"
    if getattr(stream, "dedicated", False):
        result["runtimeEvidence"] = stream.integrity()
    save(output / "replay-result.json", result)
    save(output / "matched-captures.json", captures)
    save(output / "plan.json", plan)
    print(json.dumps({key: result[key] for key in
                      ("status", "sourceUnchanged", "originalResultStatus", "logProtocolComplete",
                       "requestedLabelsWithReadyRaw", "requestedLabelsWithoutReadyRaw", "invalidNativeLines")},
                     ensure_ascii=False, indent=2))
    return 0


def requested_pairs(target, maximum, bias=0):
    low = min(maximum, math.floor(target) + bias)
    return {"feedbackFloorCeil": [math.floor(target), math.ceil(target)],
            "consumerFloorPlusOne": [low, min(maximum, low + 1)]}


def page_key(uv, width, height, mip, repeat=False):
    result = []
    for coordinate, dimension in zip(uv, (width, height)):
        extent = max(1, dimension >> mip)
        addressed = coordinate % 1 if repeat else min(1, max(0, coordinate))
        texel = addressed * extent
        if not repeat:
            texel = min(extent - 0.5, max(0.5, texel))
        result.append(math.floor(texel / 128))
    return [mip, *result]


def raw_distribution(data, metadata, examples=False):
    target_hist, tap_hist, pairs, transitions = Counter(), Counter(), Counter(), Counter()
    low_hist, high_hist = Counter(), Counter()
    requested_keys = {"low": set(), "high": set()}
    actual_keys = {"low": set(), "high": set()}
    witnesses = []
    for pixel in range(CAPACITY):
        sample = struct.unpack_from("<6f2I", data, SAMPLE_OFFSET + pixel * 32)
        if not sample[7]:
            continue
        surface_index, taps = sample[6] & 65535, sample[6] >> 16
        surface = struct.unpack_from("<8I", data, SURFACE_OFFSET + surface_index * 64)
        width, height, maximum = surface[1], surface[2], surface[3] & 255
        repeat = bool(surface[3] & 256)
        target, bias = sample[4:6]
        target_hist[str(target)] += 1
        tap_hist[taps] += 1
        pair = requested_pairs(target, maximum, int(bias))
        pairs[str(pair)] += 1
        witness = {"feedbackPixel": [pixel % 128, pixel // 128],
                   "surfaceId": metadata["surfaceIds"][surface_index],
                   "uv": list(sample[:2]), "majorUv": list(sample[2:4]),
                   "targetMip": target, "bias": bias, "tapCount": taps,
                   "feedbackCenterKeysReconstructed": [
                       page_key(sample[:2], width, height, mip, repeat)
                       for mip in pair["feedbackFloorCeil"]],
                   "branches": []}
        for tap in range(taps):
            alpha = (tap + 0.5) / taps - 0.5
            tap_uv = [sample[axis] + sample[axis + 2] * alpha for axis in (0, 1)]
            for branch, name, histogram in ((0, "low", low_hist), (1, "high", high_hist)):
                packed = struct.unpack_from("<I", data,
                                          RESULT_OFFSET + (pixel * 16 + tap * 2 + branch) * 4)[0]
                entry, mip = packed & 1023, packed >> 10 & 15
                page_x, page_y = packed >> 14 & 63, packed >> 20 & 63
                requested = pair["consumerFloorPlusOne"][branch]
                histogram[mip] += 1
                transitions[f"{name}:target={target}:bias={bias}:requested={requested}:resolved={mip}"] += 1
                predicted = page_key(tap_uv, width, height, requested, repeat)
                requested_keys[name].add((surface_index, *predicted))
                actual_keys[name].add((surface_index, mip, page_x, page_y))
                if examples and pixel in (0, 127, 8256, 16383):
                    owner_offset = OWNER_OFFSET + (entry - 1) * 64
                    owner = struct.unpack_from("<8I", data, owner_offset)
                    witness["branches"].append({
                        "tap": tap, "branch": name, "requestedMip": requested,
                        "rawPlusOneMaximumMip": min(maximum, requested - int(bias) + 1),
                        "requestedTapKeyReconstructed": predicted,
                        "resolvedKeyObserved": [mip, page_x, page_y], "entry": entry,
                        "slot": entry - 1, "anchored": bool(packed & (1 << 27)),
                        "ownerSurfaceId": metadata["surfaceIds"][owner[1]],
                        "ownerKey": [owner[2], owner[3] & 65535, owner[3] >> 16],
                        "ownerWorld": owner[4] | owner[5] << 32,
                        "ownerRevision": owner[6] | owner[7] << 32,
                        "ownerContentHash": data[owner_offset + 32:owner_offset + 64].hex()})
        if witness["branches"]:
            witnesses.append(witness)
    owners = []
    for slot in range(512):
        offset = OWNER_OFFSET + slot * 64
        owner = struct.unpack_from("<8I", data, offset)
        if owner[0]:
            owners.append({"slot": slot, "surfaceId": metadata["surfaceIds"][owner[1]],
                           "mip": owner[2], "x": owner[3] & 65535, "y": owner[3] >> 16,
                           "worldGeneration": owner[4] | owner[5] << 32,
                           "revision": owner[6] | owner[7] << 32,
                           "contentHash": data[offset + 32:offset + 64].hex()})
    return {"visibleSamples": sum(target_hist.values()), "targetMipHistogram": dict(target_hist),
            "tapCountHistogram": dict(tap_hist), "requestedPairComparison": dict(pairs),
            "branchTransitions": dict(transitions), "resolvedLowTapHistogram": dict(low_hist),
            "resolvedHighTapHistogram": dict(high_hist),
            "uniqueRequestedTapKeysReconstructed": {
                name: [list(key) for key in sorted(keys)] for name, keys in requested_keys.items()},
            "uniqueActualResolvedKeys": {
                name: [list(key) for key in sorted(keys)] for name, keys in actual_keys.items()},
            "committedOwnerCount": len(owners),
            "committedOwnerMipHistogram": dict(Counter(owner["mip"] for owner in owners)),
            "committedOwners": owners if examples else None, "knownPixels": witnesses,
            "feedbackComparisonScope": "Reconstructed from raw winner UV/target and immutable package "
                "feedback floor/ceil shader; four RGBA8 feedback attachments were not exported. "
                "Reconstructed keys are not an observed feedback-ring readback."}


def diagnose(options, surfaces):
    source = options.diagnose.resolve(strict=True)
    require(not (source / "runtime-events.jsonl").exists(),
            "Legacy stdout diagnosis forbidden for dedicated runs; use --replay with strict RuntimeEvidence")
    output = options.output.resolve()
    require(not output.is_relative_to(source) and not source.is_relative_to(output),
            "Keep original evidence immutable")
    originals = inventory(source)
    stream = NativeStream(source)
    failure_events, terminal_lines, stats_blocks, production = [], [], [], []
    last_native = None
    for filename in ("stdout.log", "stderr.log"):
        for line_number, line in enumerate((source / filename).read_text(
                encoding="utf-8", errors="replace").splitlines(), 1):
            origin = {"file": filename, "line": line_number}
            if "[RVT_QUALITY_FAILURE] " in line:
                failure_events.append({**origin, **json.loads(line.split("[RVT_QUALITY_FAILURE] ", 1)[1])})
                continue
            if "Render thread failed:" in line or "[RVT_QUALITY_INCOMPLETE]" in line:
                terminal_lines.append({**origin, "text": line})
                continue
            if "[RVT_PRODUCTION_RAW] " in line:
                production.append({**origin, **json.loads(line.split("[RVT_PRODUCTION_RAW] ", 1)[1])})
            if line.startswith("TerrainVT generation="):
                stats_blocks.append({**origin, "header": dict(re.findall(r"(\w+)=([^\s]+)", line)),
                                     "precedingNativeRecord": last_native, "surfaces": []})
            elif line.startswith("  terrain|") and stats_blocks:
                stats_blocks[-1]["surfaces"].append({
                    "surfaceId": line.split()[0], **origin,
                    "values": dict(re.findall(r"(\w+)=([^\s]+)", line))})
            stream.line(line)
            if stream.records:
                last_native = {key: stream.records[-1].get(key)
                               for key in ("nativePrefix", "frame", "poseId", "worldGeneration",
                                           "submissionEpoch", "monotonicMs")}
    captures, unmatched = [], []
    records = [row for row in stream.records if row["nativePrefix"] == "RVT_QUALITY_CAPTURE"]
    for index, record in enumerate(records):
        metadata, binary_entry, metadata_entry, data = capture_metadata(record, stream)
        distribution_data = raw_distribution(data, metadata, examples=index == len(records) - 1)
        try:
            observation = decode_capture(record, stream, surfaces)
        except PendingQualityState as error:
            unmatched.append({**error.identity, "samplesArtifact": binary_entry,
                              "metadataArtifact": metadata_entry, "distribution": distribution_data,
                              "serviceStateBound": False})
            continue
        captures.append({**observation, "distribution": distribution_data})
    require(inventory(source) == originals, "Original run evidence changed during diagnosis")
    package_before = read(source / "package-before.json")
    package_proofs = []
    for name in ("shader/glsl/pass/terrainFeedback.frag",
                 "shader/glsl/terrain/terrainCompositedQuality.comp",
                 "shader/glsl/materialFunction/mf_terrainCompositedVT.glsl"):
        path = options.package / name
        require({"bytes": path.stat().st_size, "sha256": digest(path)} == package_before[name],
                "Package shader differs from executed inventory")
        package_proofs.append({"path": str(path), **package_before[name]})
    injections = [json.loads(line) for line in (source / "injection.jsonl").read_text().splitlines()]
    echoes = [{"line": number, "text": line} for number, line in enumerate(
        (source / "stdout.log").read_text().splitlines(), 1) if "Camera pose set:" in line]
    report = {"status": "cpu-diagnosed-native-failure", "rendererStarted": False,
              "acceptancePassed": False, "originalRun": str(source), "originalRunUnchanged": True,
              "originalFiles": originals, "packageShaderProofs": package_proofs,
              "injections": injections, "cameraExecutionEchoes": echoes,
              "nativeFailures": failure_events, "terminalLines": terminal_lines,
              "rawCaptureCount": len(records),
              "matchedCaptureCount": len(captures), "unmatchedCaptures": unmatched,
              "queueStats": stats_blocks, "productionRecords": production,
              "productionRecordCount": len(production),
              "productionCountScope": "Completed native timing/status records, not inferred total "
                  "successful publications; old payload omits page x/y/slot and publication acceptance.",
              "feedbackRawAvailable": False,
              "scope": "Every actual captured tap and both branches retained; no threshold/budget/camera change"}
    output.mkdir(parents=True, exist_ok=False)
    save(output / "diagnosis.json", report)
    save(output / "all-captures.json", captures)
    save(output / "last-capture-distribution.json", captures[-1])
    print(json.dumps({"status": report["status"], "matchedCaptureCount": len(captures),
                      "rawCaptureCount": len(records),
                      "unmatchedCaptures": [{key: row[key] for key in
                          ("worldGeneration", "poseId", "frame", "submissionEpoch", "serviceStateBound")}
                          for row in unmatched], "productionRecordCount": len(production),
                      "nativeFailures": failure_events, "lastDistribution": {
                          key: captures[-1]["distribution"][key] for key in
                          ("visibleSamples", "targetMipHistogram", "resolvedLowTapHistogram",
                           "resolvedHighTapHistogram", "committedOwnerCount", "committedOwnerMipHistogram")}},
                     ensure_ascii=False, indent=2))
    return 0


def completed_screenshot(path, stream, viewport):
    from run_composited_acceptance import bmp_integrity
    saved = stream.saved_screenshots.get(str(path.resolve()))
    if saved is None or not path.is_file():
        return None
    try:
        integrity = bmp_integrity(path, viewport)
        raw = path.read_bytes()
        declared, offset = (struct.unpack_from("<I", raw, location)[0] for location in (2, 10))
        width, signed_height, _, bits = struct.unpack_from("<iiHH", raw, 18)
        stride = ((width * bits + 31) // 32) * 4
        require(declared == len(raw) == offset + stride * abs(signed_height),
                "Screenshot BMP header/offset/stride/file length differ")
        entry = reference(path, stream.output)
        require(entry["bytes"] == integrity["bytes"] == len(raw) and
                entry["sha256"] == integrity["sha256"] == hashlib.sha256(raw).hexdigest(),
                "Screenshot changed between completed BMP validation and hashing")
        return {"artifact": entry, "bmpIntegrity": integrity, "headerDeclaredBytes": declared,
                "pixelOffset": offset, "rowStride": stride, "writerSavedLog": saved,
                "exactScreenshotFrameBound": False}
    except (OSError, ValueError, struct.error):
        return None


def wait_screenshot(path, stream, viewport, alive, timeout=5):
    started = time.monotonic()
    while True:
        stream.poll()
        proof = completed_screenshot(path, stream, viewport)
        if proof is not None:
            return proof
        running = alive()
        expired = time.monotonic() - started >= timeout
        if not running or expired:
            stream.poll(final=not running)
            proof = completed_screenshot(path, stream, viewport)
            require(proof is not None,
                    "Screenshot incomplete: require writer Screenshot saved plus exact BMP integrity")
            return proof
        time.sleep(0.03)


def rebind_screenshots(options, surfaces):
    source = options.rebind_screenshots.resolve(strict=True)
    output = options.output.resolve()
    require(not output.is_relative_to(source) and not source.is_relative_to(output),
            "Screenshot correction must be separate from original run")
    originals = inventory(source)
    result = read(source / "result.json")
    require(result["exitCode"] == 0 and result["failure"] is None,
            "Terminal screenshot rebind requires original clean run")
    stream = NativeStream(source)
    stream.poll(final=True)
    viewport = read(source / "process-conditions.json")["resolution"]
    journal = [json.loads(line) for line in
               (source / "tour-observations.jsonl").read_text(encoding="utf-8").splitlines()]
    capture_records = {row["submissionEpoch"]: row for row in stream.records
                       if row["nativePrefix"] == "RVT_QUALITY_CAPTURE"}
    shots = {shot["name"]: shot for shot in read(options.capture_plan)["shots"]}
    corrections = []
    for row in journal:
        if row.get("kind") != "post-ready-screenshot":
            continue
        label = row["label"]
        pose = next(item for item in result["successfulObservations"]
                    if item["label"] == label and item["metadata"]["frame"] == row["qualityFrame"])
        observation = decode_capture(capture_records[pose["metadata"]["submissionEpoch"]], stream, surfaces)
        require(camera_matches(shots[label], observation["metadata"]), "Original camera differs")
        proof = completed_screenshot(source / row["artifact"]["path"], stream, viewport)
        require(proof is not None, "Terminal screenshot is incomplete or writer saved log missing")
        corrections.append({
            "label": label, "originalObservation": row,
            "originalBindingStatus": "pendingDuringWrite" if (
                row["artifact"] != proof["artifact"]) else "terminalRevalidated",
            "correctedBinding": proof,
            "qualityFrame": observation["metadata"]["frame"],
            "qualitySubmissionEpoch": observation["metadata"]["submissionEpoch"],
            "actualNativeArrivedMonotonicMs": observation["arrivedMonotonicMs"],
            "actualNativeCompletedReadyMonotonicMs": observation["metadata"]["completedMonotonicMs"],
            "actualElapsedMs": observation["state"]["elapsedMs"],
            "rawCounts": observation["counts"], "samplesArtifact": observation["samplesArtifact"],
            "scope": "Independent post-ready screenshot; saved-log completion is not an exact GPU frame binding"})
    require(len(corrections) == 4, "Expected all four original pilot screenshot observations")
    text = (source / "stdout.log").read_text(encoding="utf-8", errors="replace") + (
        source / "stderr.log").read_text(encoding="utf-8", errors="replace")
    validation_errors = re.findall(r"VUID-[^\s]+|Validation Error[^\n]*|THREADING ERROR[^\n]*", text)
    require(not validation_errors, "Original run contains validation errors")
    require(inventory(source) == originals, "Original files changed during screenshot rebind")
    output.mkdir(parents=True, exist_ok=False)
    receipt = {"status": "terminal-screenshot-bindings-corrected", "rendererStarted": False,
               "acceptancePassed": False, "visualAcceptancePassed": False,
               "visualStatus": "visible-seam-unresolved", "source": str(source),
               "originalFilesUnchanged": True, "originalTimelinePreserved": True,
               "originalImagesUnmodified": True, "validationErrorCount": len(validation_errors),
               "corrections": corrections, "diagnosticStageAudit": result.get("diagnosticStageAudit"),
               "visualNotes": [{"label": "junction_0_0", "category": "physicaltileGeometryOrTESseam",
                                "status": "待定位",
                                "observation": "Original screenshot shows a horizontal broken black line "
                                    "and discontinuity near image center; raw ready does not establish visual pass.",
                                "cause": "Unconfirmed; owner needs independent depth/UV/position diagnostics "
                                    "for geometry/LOD/TES boundaries. No scene, TES, threshold or pixel edits.",
                                "scope": "One junction camera only; full53 and seam comparisons remain required."}]}
    save(output / "screenshot-correction-receipt.json", receipt)
    save(output / "terminal-artifact-inventory.json", {
        "artifactRoot": str(source), "scope": "Actual terminal bytes; original journal pending bindings "
            "are superseded only by the separate correction receipt",
        "artifacts": [{"path": name, **entry} for name, entry in sorted(originals.items())]})
    print(json.dumps({"status": receipt["status"], "validationErrorCount": len(validation_errors),
                      "visualStatus": receipt["visualStatus"], "corrections": [{
                          "label": row["label"], "originalBindingStatus": row["originalBindingStatus"],
                          **row["correctedBinding"]["artifact"]} for row in corrections]}, indent=2))
    return 0


def inject_command(pid, command, output):
    require("\n" not in command and "\r" not in command and len(command) < 4000, "Unsafe console line")
    script = Path(__file__).resolve().parents[1] / "console_inject.ps1"
    argv = ["pwsh", "-NoProfile", "-File", str(script),
            "-InjectProcessId", str(pid), "-InjectLine", command]
    result = subprocess.run(argv, capture_output=True, timeout=8, check=False,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    with (output / "injection.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps({"pid": pid, "command": command, "exitCode": result.returncode,
                                 "observerMonotonic": time.monotonic(),
                                 "stdout": result.stdout.decode("utf-8", "replace"),
                                 "stderr": result.stderr.decode("utf-8", "replace")}) + "\n")
    require(result.returncode == 0, "Actual console injection failed; inspect injection.jsonl")


def memory_watch(process, output, stop):
    from runtime_stress import cpu_memory, gpu_script
    with (output / "wddm.jsonl").open("wb") as gpu_log, (
            output / "private.jsonl").open("w", encoding="utf-8") as cpu_log:
        helper = subprocess.Popen(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", gpu_script(process.pid, 500)],
            stdout=gpu_log, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            while not stop.is_set() and process.poll() is None:
                row = {**cpu_memory(process.pid), "observerMonotonic": time.monotonic()}
                cpu_log.write(json.dumps(row) + "\n")
                cpu_log.flush()
                stop.wait(0.5)
        finally:
            try:
                helper.wait(timeout=3)
            except subprocess.TimeoutExpired:
                helper.kill()
            _, errors = helper.communicate(timeout=5)
            (output / "wddm-helper.stderr").write_bytes(errors)


def raw_report(output, frozen, stream=None):
    text = (output / "stdout.log").read_text(encoding="utf-8", errors="replace")
    if getattr(stream, "dedicated", False):
        gpu = [row["payload"] for row in stream.envelopes if row["eventClass"] == "GPU_RAW"]
        cpu = [row["payload"] for row in stream.envelopes if row["eventClass"] == "CPU_RAW"]
        gpu_errors, cpu_errors = [], []
    else:
        gpu, gpu_errors = extract(text, "[GPU_RAW] ")
        cpu, cpu_errors = extract(text, "[CPU_RAW] ")
    for name, rows in (("gpu-raw.jsonl", gpu), ("cpu-raw.jsonl", cpu)):
        with (output / name).open("w", encoding="utf-8") as stream:
            for row in rows:
                stream.write(json.dumps(row) + "\n")
    missing = []
    if len(gpu) < 1000 or len(cpu) < 1000:
        missing.append("At least 1000 raw CPU and GPU frames required")
    if not gpu or not any(any(isinstance(item, dict) and item.get("name") == "terrainQualityAfterFeedback"
                             for item in (row.get("passes") if isinstance(row.get("passes"), list) else []))
                         for row in gpu):
        missing.append("Framehook terrainQualityAfterFeedback absent: await R5/R6 package")
    for label, rows in (("GPU", gpu), ("CPU", cpu)):
        frames = [row.get("frame") for row in rows]
        if not frames or any(type(frame) is not int for frame in frames) or frames != list(
                range(frames[0], frames[0] + len(frames))):
            missing.append(label + " frame identity missing/noncontiguous")
    sums = []
    for row in gpu:
        passes = row.get("passes")
        if not isinstance(passes, list) or not passes or any(
            not isinstance(item, dict) or type(item.get("ms")) not in (int, float) or
            not math.isfinite(item["ms"]) or item["ms"] < 0 for item in passes):
            missing.append(f"Incomplete GPU raw timings at frame {row.get('frame')}")
            sums = []
            break
        sums.append(sum(item["ms"] for item in passes))
    return {"gpuFrames": len(gpu), "cpuFrames": len(cpu), "parseErrors": gpu_errors + cpu_errors,
            "wholeRunGpuPassSumMs": distribution(sums),
            "missingEvidence": missing, "frozenTargets": frozen["targets"],
            "formalPerformancePassed": False,
            "rawSource": "dedicated-runtime-evidence" if getattr(stream, "dedicated", False) else "legacy-stdout",
            "scope": "Entire process including initial/loading/reload/capture; no 4.618ms acceptance claim"}


def matrix_cases(capture):
    cases = []
    for mode in ("CoverageAtlas", "LocalLayerArray"):
        for worker in (1, 2):
            prefix = f"{mode}-w{worker}"
            for batch, offset in enumerate(range(0, 53, MAX_BATCH), 1):
                count = min(MAX_BATCH, 53 - offset)
                cases.append({"id": f"{prefix}-coverage-{batch:02}", "mode": mode, "worker": worker,
                              "selection": "coverage", "offset": offset, "count": count,
                              "shots": route(capture, "coverage", offset, count)})
            for selection in ("core", "continuous", "teleport"):
                cases.append({"id": f"{prefix}-{selection}", "mode": mode, "worker": worker,
                              "selection": selection, "offset": 0, "count": 4,
                              "shots": route(capture, selection)})
    return cases


def prepare_matrix(options, plan):
    output = options.output.resolve()
    capture = read(options.capture_plan)
    cases = matrix_cases(capture)
    output.mkdir(parents=True, exist_ok=False)
    runner = Path(__file__).resolve()
    commands = []
    for case in cases:
        package_parameter = "{CoveragePackage}" if case["mode"] == "CoverageAtlas" else "{LocalPackage}"
        argv = ["py", "-3", str(runner), "--package", package_parameter,
                "--package-validator", "{PackageValidator}",
                "--capture-plan", str(options.capture_plan.resolve()),
                "--capture-plan-sha256", options.capture_plan_sha256,
                "--worker", str(case["worker"]), "--weight-source", case["mode"],
                "--selection", case["selection"], "--offset", str(case["offset"]),
                "--count", str(case["count"]), "--output", "{EvidenceRoot}/" + case["id"],
                "--execute-batch"]
        commands.append({**case, "argvTemplate": argv, "rendererStarted": False,
                         "executionRequiresMainWindow": True,
                         "installationVerificationRequiredAtExecution": True})
    save(output / "cases.json", commands)
    script = r'''param(
    [Parameter(Mandatory=$true)][string]$CoveragePackage,
    [Parameter(Mandatory=$true)][string]$LocalPackage,
    [Parameter(Mandatory=$true)][string]$PackageValidator,
    [Parameter(Mandatory=$true)][string]$EvidenceRoot,
    [Parameter(Mandatory=$true)][string]$CaseId,
    [switch]$Execute
)
$ErrorActionPreference = 'Stop'
$tourCases = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'cases.json') -Raw | ConvertFrom-Json
$tourCase = @($tourCases | Where-Object { $_.id -eq $CaseId })
if ($tourCase.Count -ne 1) { throw 'Select exactly one known case; no automatic matrix execution.' }
$tourArguments = @($tourCase[0].argvTemplate | Select-Object -Skip 1 | ForEach-Object {
    $tourArgument = $_.Replace('{CoveragePackage}', [IO.Path]::GetFullPath($CoveragePackage))
    $tourArgument = $tourArgument.Replace('{LocalPackage}', [IO.Path]::GetFullPath($LocalPackage))
    $tourArgument = $tourArgument.Replace('{PackageValidator}', [IO.Path]::GetFullPath($PackageValidator))
    $tourArgument.Replace('{EvidenceRoot}', [IO.Path]::GetFullPath($EvidenceRoot))
})
if (-not $Execute) { $tourArguments[$tourArguments.Count - 1] = '--plan' }
& py @tourArguments
exit $LASTEXITCODE
'''
    (output / "run-case.ps1").write_text(script, encoding="utf-8")
    coverage_shots = [shot for shot in capture["shots"]
                      if shot.get("category") in ("center", "edge", "junction", "outer")]
    checklist = {
        "rendererStarted": False, "acceptancePassed": False, "visualAcceptancePassed": False,
        "cameraLock": plan["cameraLock"], "frozenSha256": APPROVED_P0_SHA256,
        "matrix": {"modes": ["CoverageAtlas", "LocalLayerArray"], "workers": [1, 2],
                   "coveragePosesPerCombination": 53, "caseCount": len(cases),
                   "initialFarExcludedFrom53": True, "allFinalPackagesStartAtOffsetZero": True},
        "requiredPerPose": [
            "Actual camera position/direction, World/pose/frame/submission/completed epochs",
            "Raw fixed ABI plus bound metadata and exact dedicated capture_completed/Service STATE",
            "Native arrival-to-completed ready <=10s; complete visible denominator, >=99% raw target+1",
            "Bias<=1, unresolved/incorrectIdentity/rootOnly/uninitialized=0; no budget changes",
            "All taps/both branches histograms, requested/resolved page keys, slot+revision+content identity",
            "Writer-completed full BMP plus hash; separate post-ready image, never an exact-frame claim",
            "Retain visual failures and all failed/incomplete observations; no pixel repair"],
        "poses": coverage_shots,
        "continuousRoute": {
            "from": next(shot for shot in capture["shots"] if shot["name"] == "tile00_near"),
            "to": next(shot for shot in capture["shots"] if shot["name"] == "near"),
            "intermediateActualSnapshotWitnesses": 24,
            "scope": "Interpolated camera commands with real camera/raw witnesses. Native crossing distance/time "
                     "and continuity event is still needed for formal continuous-motion acceptance."},
        "teleportRoute": [shot for shot in capture["shots"]
                          if shot["name"] in ("far", "near", "tile00_near", "cross_tile_corner")],
        "requiredNativeHandoff": {
            "envelope": "sequence from1, steady_ns, threadTag, outputCategory=explicitoutput, eventClass, payload",
            "footer": "runtimeEvidenceEnd last; incomplete=false; exact recordsBeforeTerminal and bytesBeforeTerminal",
            "requiredForQuality": ["RVT_QUALITY_EVENT capture_submitted/capture_completed",
                                   "RVT_QUALITY_STATE with actual elapsedMs and accepted interactive state"],
            "captureBinding": "Either RVT_QUALITY_CAPTURE fields frame/submissionEpoch/binary/visible/complete, "
                              "or fixed ABI export discovered only after exact dedicated capture_completed. "
                              "Artifact work items are explicitly runner-derived, never native events.",
            "packageIntegrationItemsToVerify": [
                "RVT_QUALITY_CAPTURE optional if exact completed-event plus fixed-ABI artifact binding is available",
                "Explicit pose_arrived camera/sequence; current Readiness writer still stdout-only",
                "GPU_RAW and CPU_RAW dedicated events, Framehook terrainQualityAfterFeedback: source wiring "
                "is being updated; final binary/cache/package identity must prove inclusion",
                "RVT_PRODUCTION_RAW page x/y/slot/accepted publication identity",
                "Native crossing/unvisited-destination/World A-B-A transactions",
                "Per-pose loading/present screenshot exact submission completion",
                "All submitted captures complete before footer, or supply explicit native abort/retirement "
                "schema; reader never invents completion of the last in-flight capture"]},
        "oracleMissing": [
            {"item": "Independent lit ROI", "required": "Pinned reference image + camera/render settings/light/"
             "exposure/TAA state, terrain ROI/depth mask, numeric MAE/RMSE/P99 vs original P0 thresholds. "
             "Post-ready BMP alone is insufficient."},
            {"item": "Geometry/LOD/TES seams", "required": "Per-pose independent depth/world-position/UV/"
             "tile identity/LOD/morph/tess/displacement diagnostics across every edge,9junctions,4outercorners. "
             "AB02 accepted finite texCoord1 result does not close this matrix."},
            {"item": "TES numeric oracle", "required": "Bound height source digest, TR/world transforms,"
             "active radius/distance/nonzero displacement, independent R16 comparison <=2units, "
             "baseFineResidentPages=0 witness; no TES-disable workaround."},
            {"item": "Base-channel oracle", "required": "Same-page key/content/revision/albedo-normal-ORM "
             "GPU raw readback vs independent current evaluator. Existing192 border numeric scope is finite "
             "and cannot substitute scene-lit/geometry evidence."},
            {"item": "Dynamic items", "required": "Actual scene object list, animation/physics time/input "
             "trace and independent temporal/occlusion/motion reference; absent fixtures are missing, not passed."},
            {"item": "World reload", "required": "Different B scene inside each final package, native commits/"
             "stale-publication and overlap ledger, A-B-A plus failed-candidate retention. No B asset path invented."}],
        "formalPerformance": "Separate final-package collector and >=1000 CPU/GPU raw frames, Framehook and all "
                             "P0 conditions required. This quality/visual matrix never declares formal performance.",
        "knownVisualStatus": "Original diagnostic junction blackline remains archived visual FAIL; final current "
                             "package must independently re-evaluate every edge/corner after limited AB02.",
        "referenceStage": {"path": str(options.package), "diagnostic": options.diagnostic_stage,
                           "scope": "Preparation reference only. run-case.ps1 requires actual final package paths "
                                    "and native validator; old14/53 are not reused."}}
    if options.local_package:
        local = options.local_package.resolve(strict=True)
        _, _, manifest = package_inputs(local, "LocalLayerArray")
        checklist["localPreparationReference"] = {"path": str(local),
                                                "manifestSha256": digest(manifest),
                                                "scope": "Read-only reference, not final-package acceptance"}
    save(output / "capture-and-oracle-checklist.json", checklist)
    save(output / "plan.json", plan)
    print(json.dumps({"rendererStarted": False, "cases": len(cases),
                      "output": str(output), "executePolicy": "One explicitly selected case only",
                      "formalPerformancePassed": False, "visualAcceptancePassed": False}, indent=2))
    return 0


def verify_installation(package, validator, output):
    require(validator is not None, "Final-package execution requires --package-validator; no diagnostic substitution")
    validator = validator.resolve(strict=True)
    require(validator.name.lower() == "vl_export_game.exe", "Native vl_export_game.exe validator required")
    before = digest(validator)
    result = subprocess.run([str(validator), "--verify-package", str(package)],
                            cwd=output, capture_output=True, timeout=300, check=False,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    (output / "installation-verify.log").write_bytes(result.stdout + result.stderr)
    receipt = {"argv": result.args, "exitCode": result.returncode, "validatorSha256": before,
               "log": reference(output / "installation-verify.log", output)}
    save(output / "installation-verify.json", receipt)
    require(digest(validator) == before and result.returncode == 0,
            "Native final installation closure verification failed; inspect installation-verify.log")
    return receipt


def execute(options, plan, config, surfaces):
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "quality").mkdir()
    (output / "present").mkdir()
    save(output / "plan.json", plan)
    before = inventory(options.package.resolve())
    save(output / "package-before.json", before)
    if options.diagnostic_stage:
        audit = diagnostic_stage_audit(options.package, options.handoff)
        require(audit == plan["diagnosticStageAudit"], "Diagnostic stage/handoff drifted after preparation")
        save(output / "diagnostic-stage-audit.json", audit)
    else:
        verify_installation(options.package, options.package_validator, output)
    environment, conditions = process_conditions(options.package, output, config, options.profile,
                                                   options.worker, "capture-4hz")
    environment["VULKANLEARN_RVT_QUALITY_EVIDENCE_DIR"] = str(output / "present")
    environment["VULKANLEARN_RUNTIME_CACHE_ROOT"] = str(output / "cache")
    environment["VULKANLEARN_RUNTIME_EVIDENCE"] = str(output / "runtime-events.jsonl")
    conditions["environmentOverrides"]["VULKANLEARN_RUNTIME_EVIDENCE"] = environment["VULKANLEARN_RUNTIME_EVIDENCE"]
    conditions["structuredEvidenceSource"] = "Dedicated RuntimeEvidence only; sequence/footer/byte totals required"
    save(output / "process-conditions.json", conditions)
    stream = RuntimeEvidenceStream(output)
    started = time.monotonic()
    stop = threading.Event()
    failure, tour, watcher = None, None, None
    process = None
    missing = ["TES active radius/displacement/independent R16 comparison is not emitted by current package",
               "Per-pose loading_black_drawn/loading_released exact screenshot submission: "
               "current native hook only queues first pair per World"]

    def trace(row):
        with (output / "tour-observations.jsonl").open("a", encoding="utf-8") as target:
            target.write(json.dumps(row, ensure_ascii=False, allow_nan=False) + "\n")
            target.flush()

    context = quiet_host_audit(output / "host-audit.jsonl") if (
        options.profile == "formal-performance") else nullcontext()
    with (output / "stdout.log").open("wb") as stdout, (
            output / "stderr.log").open("wb") as stderr, context:
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        try:
            process = subprocess.Popen(plan["argv"], cwd=output, env=environment, stdout=stdout,
                                       stderr=stderr, creationflags=subprocess.CREATE_NEW_CONSOLE,
                                       startupinfo=startup)
            save(output / "process-start.json", {"pid": process.pid, "argv": plan["argv"],
                                                "cwd": str(output), "startedMonotonic": started})
            watcher = threading.Thread(target=memory_watch, args=(process, output, stop), daemon=True)
            watcher.start()
            inject = lambda command: inject_command(process.pid, command, output)
            tour = Tour(stream, surfaces, lambda: process.poll() is None, inject, trace)
            initial = read(options.capture_plan)["shots"]
            initial = next(shot for shot in initial if shot["name"] == "far")
            tour.wait_pose(initial, initial=True)
            for shot in plan["shots"]:
                result = tour.wait_pose(shot)
                print(json.dumps({"label": shot["name"], "qualityReady": result["qualityReady"],
                                  "elapsedMs": result["state"]["elapsedMs"]}), flush=True)
                path = output / (shot["name"] + ".bmp")
                inject("screenshot " + path.as_posix())
                proof = wait_screenshot(path, stream, config["windowSize"], lambda: process.poll() is None)
                trace({"kind": "post-ready-screenshot", "label": shot["name"],
                       **proof, "qualityFrame": result["metadata"]["frame"]})
            if options.movement or options.selection == "continuous":
                named = {shot["name"]: shot for shot in read(options.capture_plan)["shots"]}
                first, last = named["tile00_near"], named["near"]
                tour.wait_pose(first)
                for step in range(1, 25):
                    alpha = step / 25
                    shot = {"name": f"movement_{step:02}", "category": "movement"}
                    for field in ("position", "look_at"):
                        shot[field] = [start + (end - start) * alpha
                                       for start, end in zip(first[field], last[field])]
                    tour.wait_pose(shot, witness_only=True)
                tour.wait_pose(last)
            if options.reload_b:
                for scene in (options.reload_b, config["initScene"]):
                    old_generation = tour.last_key[0]
                    inject("loadworld " + scene)
                    result = tour.wait_pose(initial, initial=True, min_world_generation=old_generation + 1)
                    require(result["metadata"]["worldGeneration"] > old_generation,
                            "World reload did not advance actual generation")
                missing.append("World candidate/overlap/stale-publication native ledger still requires review")
            if options.profile == "formal-performance":
                deadline = time.monotonic() + 45
                while sum(row["nativePrefix"] == "GPU_RAW" for row in stream.records) < 1000:
                    stream.poll()
                    require(time.monotonic() < deadline and process.poll() is None,
                            "Timeout waiting for 1000 raw GPU frames with quality enabled")
                    time.sleep(0.03)
        except Exception as error:
            failure = f"{type(error).__name__}: {error}"
            trace({"kind": "terminal-failure", "reason": failure})
        finally:
            if process is not None and process.poll() is None:
                try:
                    inject_command(process.pid, "quit", output)
                    process.wait(timeout=20)
                except Exception as error:
                    failure = failure or f"Clean shutdown failed: {error}"
                    process.kill()
                    process.wait(timeout=10)
            stop.set()
            if watcher is not None:
                watcher.join(timeout=10)
    try:
        stream.poll(final=True)
    except Exception as error:
        failure = failure or str(error)
    save(output / "native-observations.json", stream.records)
    save(output / "native-log-integrity.json",
         {"invalidLines": stream.invalid_lines,
          "unmatchedCompletions": list(stream.unpaired_completions.values()),
          "partialLines": {name: value.decode("utf-8", errors="replace")
                           for name, value in stream.pending.items() if value}})
    save(output / "runtime-evidence-integrity.json", stream.integrity())
    after = inventory(options.package.resolve())
    save(output / "package-after.json", after)
    if before != after:
        failure = failure or "Readonly package inventory changed"
    if options.diagnostic_stage:
        try:
            require(diagnostic_stage_audit(options.package, options.handoff) ==
                    plan["diagnosticStageAudit"], "Diagnostic stage/baseline/handoff changed during run")
        except Exception as error:
            failure = failure or str(error)
    timing = raw_report(output, read(options.package / "config/terrainRvtP0Frozen.json"), stream)
    missing.extend(timing["missingEvidence"])
    text = (output / "stdout.log").read_text(encoding="utf-8", errors="replace")
    if process is None or process.returncode != 0 or "EngineLoop teardown complete" not in text:
        failure = failure or "Missing exit0/clean teardown"
    result = {"status": "failed" if failure else "collected-independent-review-required",
              "failure": failure, "acceptancePassed": False, "formalPerformancePassed": False,
              "rendererStarted": process is not None, "pid": process.pid if process else None,
              "exitCode": process.returncode if process else None,
              "packageUnchanged": before == after, "elapsedSeconds": time.monotonic() - started,
              "successfulObservations": tour.results if tour else [], "missingEvidence": missing,
              "rawTiming": timing, "scope": "Evidence collector, no independent acceptance authority"}
    result["runtimeEvidence"] = stream.integrity()
    result["installationVerified"] = not options.diagnostic_stage
    result["visualAcceptancePassed"] = False
    if options.diagnostic_stage:
        result.update(diagnosticStage=True, installationVerified=False,
                      diagnosticStageAudit=plan["diagnosticStageAudit"],
                      scope="Private feedback-only diagnostic pilot; not a formal installed-package acceptance")
    save(output / "result.json", result)
    save(output / "artifacts.json", [reference(path, output) for path in sorted(output.rglob("*"))
                                    if path.is_file() and path.name != "artifacts.json"])
    print(json.dumps({key: result[key] for key in
                      ("status", "failure", "pid", "exitCode", "missingEvidence")}, indent=2))
    return 3 if failure else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--capture-plan", type=Path, required=True)
    parser.add_argument("--capture-plan-sha256", default=C008_CAMERA_SHA256)
    parser.add_argument("--worker", type=int, choices=(1, 2), default=2)
    parser.add_argument("--weight-source", choices=("CoverageAtlas", "LocalLayerArray"), default="CoverageAtlas")
    parser.add_argument("--selection", choices=("smoke", "coverage", "core", "continuous", "teleport"), default="smoke")
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--count", type=int, default=4)
    parser.add_argument("--movement", action="store_true")
    parser.add_argument("--reload-b", help="Different scene already present inside this same package")
    parser.add_argument("--profile", choices=("correctness", "formal-performance"), default="correctness")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--diagnostic-stage", action="store_true",
                        help="Explicit private stage; original installation manifest does not cover its shader delta")
    parser.add_argument("--handoff", type=Path, help="Approved diagnostic shader/cache handoff.json")
    parser.add_argument("--package-validator", type=Path, help="Native final closure verifier; required for execution")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--plan", action="store_true")
    actions.add_argument("--execute-smoke-first", action="store_true")
    actions.add_argument("--execute-batch", action="store_true")
    actions.add_argument("--replay", type=Path, help="CPU-only replay of immutable completed run logs/raw")
    actions.add_argument("--diagnose", type=Path, help="CPU-only per-tap diagnosis of a terminated run")
    actions.add_argument("--rebind-screenshots", type=Path,
                         help="CPU-only terminal correction receipt; never overwrite original logs/images")
    actions.add_argument("--prepare-matrix", action="store_true",
                         help="CPU-only final two-mode/two-worker53+routes commands and capture checklist")
    parser.add_argument("--local-package", type=Path, help="Local-mode package placeholder/reference for matrix preparation")
    options = parser.parse_args()
    package = options.package = options.package.resolve(strict=True)
    output = options.output = options.output.resolve()
    require(not output.exists() and not output.is_relative_to(package) and not package.is_relative_to(output),
            "Use a fresh external evidence/cwd directory")
    require(digest(options.capture_plan) == options.capture_plan_sha256, "Locked C008 camera plan hash differs")
    config, surfaces, manifest_path = package_inputs(package, options.weight_source)
    require(bool(options.handoff) == options.diagnostic_stage,
            "Diagnostic stage requires explicit --diagnostic-stage and --handoff together")
    require(not options.diagnostic_stage or options.profile == "correctness",
            "Private diagnostic stage cannot be used for formal performance acceptance")
    diagnostic_audit = diagnostic_stage_audit(package, options.handoff) if options.diagnostic_stage else None
    if options.execute_smoke_first:
        require(options.weight_source == "CoverageAtlas" and options.worker == 2 and
                options.selection == "smoke" and not options.movement and not options.reload_b and
                options.profile == "correctness", "First execution must be Coverage worker2 four-pose correctness")
    if options.reload_b:
        require(options.reload_b != config["initScene"] and
                (package / "resources" / options.reload_b).resolve(strict=True).is_relative_to(package),
                "A-B-A needs a different B scene inside the package")
    shots = route(read(options.capture_plan), options.selection, options.offset, options.count)
    argv = [str(package / "main_game.exe"), "--initial-scene", config["initScene"],
            "--worker-thread-count", str(options.worker), "--frame-limit", "30",
            "--present-mode", "immediate", "--no-dev-ui",
            "--validation" if options.profile == "correctness" else "--no-validation",
            "--perf-jsonl", str(output / "runtime.perf.jsonl")]
    require(len(subprocess.list2cmdline(argv)) < 8000, "Windows launch command exceeds 8k")
    plan = {"rendererStarted": False, "acceptancePassed": False, "argv": argv, "cwd": str(output),
            "shots": shots, "cameraLock": {"path": str(options.capture_plan.resolve()),
                                         "sha256": digest(options.capture_plan),
                                         "scope": "External test only, never a Game input"},
            "binarySha256": digest(package / "main_game.exe"), "manifestSha256": digest(manifest_path),
            "frozenSha256": APPROVED_P0_SHA256, "captureCap": 256,
            "poseDeadlineMs": 10000, "qualityFraction": 0.99, "maxEffectiveBias": 1,
            "qualityMode": "capture-4hz; original cap unchanged; stop when raw unavailable",
            "matrix": ["CoverageAtlas/w1", "CoverageAtlas/w2", "LocalLayerArray/w1", "LocalLayerArray/w2"],
            "remaining": "53 coverage poses in batches <=8 plus core/movement/teleports/A-B-A; independent review",
            "preparedAt": datetime.now().isoformat()}
    plan["structuredEvidence"] = {
        "source": str(output / "runtime-events.jsonl"), "env": "VULKANLEARN_RUNTIME_EVIDENCE",
        "requiresSequenceFrom1": True, "requiresCompleteFooter": True,
        "checksFooterRecordsAndBytes": True, "stdoutStructuredFallback": False}
    if diagnostic_audit:
        plan.update(diagnosticStage=True, installationVerified=False, diagnosticStageAudit=diagnostic_audit)
    if options.prepare_matrix:
        return prepare_matrix(options, plan)
    if options.rebind_screenshots:
        return rebind_screenshots(options, surfaces)
    if options.diagnose:
        return diagnose(options, surfaces)
    if options.replay:
        return replay(options, plan, surfaces)
    if not (options.execute_smoke_first or options.execute_batch):
        print(json.dumps(plan, ensure_ascii=False, indent=2))
        return 0
    require(sys.platform == "win32", "Native Game/console/WDDM collection requires Windows")
    return execute(options, plan, config, surfaces)


if __name__ == "__main__":
    sys.exit(main())

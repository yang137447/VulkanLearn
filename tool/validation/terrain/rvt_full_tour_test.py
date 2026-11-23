"""CPU-only regression; no Game launch, console injection or GPU dependency."""

import json
from pathlib import Path
import struct
import tempfile
import time
import unittest
from unittest.mock import patch

from rvt_full_tour import (
    NativeStream, PendingQualityState, Tour, camera_matches, decode_capture, reference, route,
    requested_pairs, raw_distribution,
    audit_stage_delta,
    completed_screenshot, wait_screenshot,
    PendingProducerArtifact,
    RuntimeEvidenceStream, matrix_cases, CAPTURE_CLASSES,
)
from rvt_quality_binary_test import QualityBinaryTests
from rvt_quality_binary import OWNER_OFFSET


class FullTourTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name)
        self.stream = NativeStream(self.output)
        self.shot = {"name": "near", "position": [0, 0, 0], "look_at": [0, 0, -5]}

    def capture(self, wrong_identity=False):
        raw, event, surfaces = QualityBinaryTests().fixture()
        if wrong_identity:
            raw[OWNER_OFFSET + 32] ^= 1
        metadata = {**event, "protocol": 1, "source": "gpu-page-table-readback",
                    "binaryAbi": "TVQ1-final-results-v1", "moduleOnly": False,
                    "cameraPosition": [0, 0, 0], "cameraLookAt": [0, 0, -1],
                    "capturedMonotonicMs": 400, "completedMonotonicMs": 500}
        binary = self.output / "capture-1.bin"
        binary.write_bytes(raw)
        binary.with_suffix(".json").write_text(json.dumps(metadata), encoding="utf-8")
        state = {"worldGeneration": 1, "poseId": "7", "submissionEpoch": 2, "completedEpoch": 2,
                 "visible": 1, "validAtRawTargetPlusOne": 1, "unresolved": 0,
                 "incorrectIdentity": 0, "rootOnly": 0, "elapsedMs": 400,
                 "interactive": True, "convergenceFailed": False}
        self.stream.states[(1, "7", 2)] = state
        record = {"nativePrefix": "RVT_QUALITY_CAPTURE", "frame": 4, "submissionEpoch": 2,
                  "binary": str(binary), "visible": 1, "complete": 1,
                  "observerMonotonic": time.monotonic() + 1}
        return record, surfaces

    def test_current_raw_counts_and_frame_pose_epoch_are_recomputed(self):
        record, surfaces = self.capture()
        self.assertEqual(decode_capture(record, self.stream, surfaces)["counts"]["visibleSamples"], 1)
        record["frame"] = 5
        with self.assertRaisesRegex(ValueError, "frame epoch"):
            decode_capture(record, self.stream, surfaces)

    def test_raw_identity_failure(self):
        record, surfaces = self.capture(wrong_identity=True)
        with self.assertRaisesRegex(ValueError, "independent raw decode"):
            decode_capture(record, self.stream, surfaces)

    def test_missingquality(self):
        record, surfaces = self.capture()
        self.stream.states.clear()
        with self.assertRaisesRegex(PendingQualityState, "Pending native quality state"):
            decode_capture(record, self.stream, surfaces)

    def test_raw_arrives_before_later_matching_state(self):
        record, surfaces = self.capture()
        state = self.stream.states.pop((1, "7", 2))
        self.stream.records.append(record)
        polls = []

        def observe(**kwargs):
            polls.append(kwargs)
            if len(polls) == 2:
                self.stream.line("[RVT_QUALITY_STATE] " + json.dumps({**state, "submissionEpoch": 3}))
            if len(polls) == 3:
                self.stream.line("[RVT_QUALITY_STATE] " + json.dumps(state))

        tour = Tour(self.stream, surfaces, lambda: True, lambda command: None, lambda row: None)
        with patch.object(self.stream, "poll", side_effect=observe), patch("rvt_full_tour.time.sleep"):
            result = tour.wait_pose(self.shot, initial=True)
        self.assertEqual(len(polls), 3)
        self.assertTrue(result["qualityReady"])
        self.assertEqual(result["state"]["submissionEpoch"], 2)
        self.assertEqual(result["metadata"]["frame"], 4)

    def partial_artifact_then_restore(self, mutation):
        record, surfaces = self.capture()
        binary = Path(record["binary"])
        metadata = binary.with_suffix(".json")
        original_raw, original_metadata = binary.read_bytes(), metadata.read_bytes()
        mutation(binary, metadata)
        self.stream.records.append(record)
        with self.assertRaises(PendingProducerArtifact):
            decode_capture(record, self.stream, surfaces)
        polls = []

        def observe(**kwargs):
            polls.append(kwargs)
            if len(polls) == 2:
                binary.write_bytes(original_raw)
                metadata.write_bytes(original_metadata)

        tour = Tour(self.stream, surfaces, lambda: True, lambda command: None, lambda row: None)
        with patch.object(self.stream, "poll", side_effect=observe), patch("rvt_full_tour.time.sleep"):
            accepted = tour.wait_pose(self.shot, initial=True)
        self.assertEqual(len(polls), 2)
        self.assertEqual(accepted["metadata"]["frame"], 4)
        self.assertEqual(accepted["metadata"]["submissionEpoch"], 2)
        self.assertTrue(accepted["qualityReady"])

    def test_partial01_empty_metadata_then_complete(self):
        self.partial_artifact_then_restore(lambda binary, metadata: metadata.write_bytes(b""))

    def test_partial02_truncated_metadata_then_complete(self):
        self.partial_artifact_then_restore(lambda binary, metadata: metadata.write_bytes(b'{"frame":'))

    def test_partial03_missing_metadata_then_created(self):
        self.partial_artifact_then_restore(lambda binary, metadata: metadata.unlink())

    def test_partial04_valid_json_missing_fields_then_complete(self):
        self.partial_artifact_then_restore(lambda binary, metadata: metadata.write_bytes(b'{"frame":4}'))

    def test_partial05_empty_binary_then_complete(self):
        self.partial_artifact_then_restore(lambda binary, metadata: binary.write_bytes(b""))

    def test_partial06_short_binary_header_then_complete(self):
        self.partial_artifact_then_restore(lambda binary, metadata: binary.write_bytes(binary.read_bytes()[:128]))

    def test_partial07_complete_header_short_payload_then_complete(self):
        self.partial_artifact_then_restore(lambda binary, metadata: binary.write_bytes(binary.read_bytes()[:4096]))

    def test_partial08_inconsistent_abi_version_then_complete(self):
        def mutate(binary, metadata):
            raw = bytearray(binary.read_bytes())
            struct.pack_into("<I", raw, 4, 2)
            binary.write_bytes(raw)
        self.partial_artifact_then_restore(mutate)

    def test_partial09_stale_metadata_pose_then_actual(self):
        def mutate(binary, metadata):
            document = json.loads(metadata.read_text())
            document["poseId"] = 99
            metadata.write_text(json.dumps(document))
        self.partial_artifact_then_restore(mutate)

    def test_partial10_stale_binary_epoch_then_actual(self):
        def mutate(binary, metadata):
            raw = bytearray(binary.read_bytes())
            struct.pack_into("<I", raw, 14 * 4, 1)
            binary.write_bytes(raw)
        self.partial_artifact_then_restore(mutate)

    def test_terminal_invalid_metadata_is_permanent_not_gpu_ready(self):
        record, surfaces = self.capture()
        Path(record["binary"]).with_suffix(".json").write_bytes(b"")
        self.stream.records.append(record)
        tour = Tour(self.stream, surfaces, lambda: False, lambda command: None, lambda row: None)
        with patch.object(self.stream, "poll"), patch("rvt_full_tour.time.sleep"):
            with self.assertRaisesRegex(ValueError, "Permanently missing.*invalid producer artifact"):
                tour.wait_pose(self.shot, initial=True)
        self.assertEqual(tour.results, [])

    def test_log_json_fragment_waits_for_full_newline_before_parse(self):
        path = self.output / "stdout.log"
        path.write_bytes(b'[CPU_RAW] {"renderFrameInterval')
        self.stream.poll()
        self.assertEqual(self.stream.records, [])
        self.assertEqual(self.stream.invalid_lines, [])
        with path.open("ab") as target:
            target.write(b'Ms":12.5}')
        self.stream.poll()
        self.assertEqual(self.stream.records, [])
        with path.open("ab") as target:
            target.write(b"\n")
        self.stream.poll(final=True)
        self.assertEqual(self.stream.records[0]["renderFrameIntervalMs"], 12.5)

    def test_terminal_log_without_newline_is_explicit_incomplete(self):
        (self.output / "stdout.log").write_bytes(b'[CPU_RAW] {"renderFrameIntervalMs":12.5}')
        with self.assertRaisesRegex(ValueError, "Permanently invalid native log"):
            self.stream.poll(final=True)
        self.assertEqual(self.stream.records, [])

    def test_interleaved_complete_line_is_not_repaired_or_silently_dropped(self):
        path = self.output / "stdout.log"
        path.write_bytes(b'[RVT_QUALITY_EVENT] i{"event":"capture_submitted"}\n'
                         b'[CPU_RAW] {"renderFrameIntervalMs":12.5}\n')
        self.stream.poll()
        self.assertEqual(len(self.stream.invalid_lines), 1)
        self.assertEqual(self.stream.records[0]["renderFrameIntervalMs"], 12.5)
        with self.assertRaisesRegex(ValueError, "Permanently invalid native log"):
            self.stream.poll(final=True)

    def test_interleaved_complete_line_explicitly_expires_at_fixed_log_timeout(self):
        self.stream.line('[GPU_RAW] x{"frame":1}')
        first_seen = self.stream.invalid_lines[0]["firstObservedMonotonic"]
        with patch("rvt_full_tour.time.monotonic", return_value=first_seen + 12):
            with self.assertRaisesRegex(ValueError, "Permanently invalid native log"):
                self.stream.poll()

    def test_completion_before_submission_waits_for_same_epoch(self):
        base = {"worldGeneration": 1, "poseId": 2, "submissionEpoch": 3}
        self.stream.line("[RVT_QUALITY_EVENT] " + json.dumps({**base, "event": "capture_completed"}))
        self.assertEqual(len(self.stream.unpaired_completions), 1)
        self.stream.line("[RVT_QUALITY_EVENT] " + json.dumps({**base, "event": "capture_submitted"}))
        self.stream.poll(final=True)
        self.assertEqual(self.stream.unpaired_completions, {})

    def test_actual_pose_arrived_event_has_sequence_instead_of_capture_epoch(self):
        self.stream.line('[RVT_QUALITY_EVENT] {"event":"pose_arrived","worldGeneration":1,'
                         '"sequence":2,"arrivedMonotonicMs":1234.5}')
        self.stream.poll(final=True)
        self.assertEqual(self.stream.invalid_lines, [])
        self.assertEqual(self.stream.records[0]["sequence"], 2)

    def test_permanently_missing_state_only_fails_after_terminal_final_drain(self):
        record, surfaces = self.capture()
        self.stream.states.clear()
        self.stream.records.append(record)
        tour = Tour(self.stream, surfaces, lambda: False, lambda command: None, lambda row: None)
        with patch.object(self.stream, "poll") as observe, patch("rvt_full_tour.time.sleep"):
            with self.assertRaisesRegex(ValueError, "Permanently missing native quality state"):
                tour.wait_pose(self.shot, initial=True)
        self.assertTrue(any(call.kwargs.get("final") is True for call in observe.call_args_list))
        self.assertEqual(tour.results, [])
        self.assertTrue(tour.terminal)

    def test_completed_epoch_mismatch_never_released_by_late_state(self):
        record, surfaces = self.capture()
        self.stream.states[(1, "7", 2)]["completedEpoch"] = 3
        with self.assertRaisesRegex(ValueError, "completed epoch differs"):
            decode_capture(record, self.stream, surfaces)

    def test_expired_capture_without_service_state_is_ignored_before_decode(self):
        record, surfaces = self.capture()
        self.stream.states.clear()
        self.stream.records.append(record)
        trace = []
        tour = Tour(self.stream, surfaces, lambda: True, lambda command: None, trace.append)
        tour.last_key = (1, "7")
        with patch("rvt_full_tour.time.monotonic", side_effect=[0, 20, 20, 20]):
            with self.assertRaisesRegex(ValueError, "timeout"):
                tour.wait_pose(self.shot)
        self.assertEqual(trace[0]["kind"], "ignored-expired-capture")
        self.assertEqual(tour.results, [])

    def test_missing_raw_is_not_settled(self):
        record, surfaces = self.capture()
        record["binary"] = "not-exported"
        with self.assertRaisesRegex(ValueError, "capture cap"):
            decode_capture(record, self.stream, surfaces)

    def test_expiredpose(self):
        record, surfaces = self.capture()
        self.stream.records.append(record)
        tour = Tour(self.stream, surfaces, lambda: True, lambda command: None, lambda row: None)
        tour.last_key = (1, "7")
        self.stream.acknowledgements = 1
        with patch.object(self.stream, "poll", side_effect=lambda **kwargs: setattr(
                self.stream, "acknowledgements", 2)), patch(
                "rvt_full_tour.time.monotonic", side_effect=[0, 20, 20, 20]):
            with self.assertRaisesRegex(ValueError, "timeout"):
                tour.wait_pose(self.shot)
        self.assertEqual(tour.results, [])

    def test_timeout_and_stop_on_firstfailure(self):
        tour = Tour(self.stream, {}, lambda: True, lambda command: None, lambda row: None)
        with patch("rvt_full_tour.time.monotonic", side_effect=[0, 20, 20, 20]):
            with self.assertRaisesRegex(ValueError, "timeout"):
                tour.wait_pose(self.shot)
        with self.assertRaisesRegex(ValueError, "terminal"):
            tour.wait_pose(self.shot)

    def test_pose_10seconds_cannot_be_relaxed_by_native_ready(self):
        record, surfaces = self.capture()
        self.stream.states[(1, "7", 2)]["elapsedMs"] = 10001
        self.stream.records.append(record)
        tour = Tour(self.stream, surfaces, lambda: True, lambda command: None, lambda row: None)
        with self.assertRaisesRegex(ValueError, "frozen 10s"):
            tour.wait_pose(self.shot, initial=True)

    def test_oneframeconcurrency(self):
        def event(name, epoch):
            return "[RVT_QUALITY_EVENT] " + json.dumps({
                "event": name, "worldGeneration": 1, "poseId": 1, "submissionEpoch": epoch})
        self.stream.line(event("capture_submitted", 1))
        with self.assertRaisesRegex(ValueError, "globally in-flight"):
            self.stream.line(event("capture_submitted", 2))
        self.stream.line(event("capture_completed", 1))
        self.stream.line(event("capture_submitted", 2))

    def test_native_failure_is_terminal_even_after_initial_ready(self):
        with self.assertRaisesRegex(ValueError, "Native failure"):
            self.stream.line('[RVT_QUALITY_FAILURE] {"event":"quality_failed"}')

    def test_camera_direction_and_position_are_observed(self):
        self.assertTrue(camera_matches(self.shot, {
            "cameraPosition": [0, 0, 0], "cameraLookAt": [0, 0, -1]}))
        self.assertFalse(camera_matches(self.shot, {
            "cameraPosition": [1, 0, 0], "cameraLookAt": [1, 0, -1]}))

    def test_integer_target_feedback_ceil_omits_consumers_high_branch(self):
        pair = requested_pairs(0.0, 13)
        self.assertEqual(pair["feedbackFloorCeil"], [0, 0])
        self.assertEqual(pair["consumerFloorPlusOne"], [0, 1])
        fractional = requested_pairs(0.25, 13)
        self.assertEqual(fractional["feedbackFloorCeil"], fractional["consumerFloorPlusOne"])

    def test_distribution_keeps_every_tap_and_both_branches(self):
        record, surfaces = self.capture()
        metadata = json.loads(Path(record["binary"]).with_suffix(".json").read_text())
        result = raw_distribution(Path(record["binary"]).read_bytes(), metadata, examples=True)
        self.assertEqual(result["visibleSamples"], 1)
        self.assertEqual(result["resolvedLowTapHistogram"], {0: 1})
        self.assertEqual(result["resolvedHighTapHistogram"], {1: 1})
        self.assertEqual(result["committedOwnerCount"], 2)
        self.assertEqual(len(result["knownPixels"][0]["branches"]), 2)

    def test_no_external_artifact_escape(self):
        outside = self.output.parent / "nonexistent-tour-test.bin"
        with self.assertRaises((ValueError, FileNotFoundError)):
            reference(outside, self.output)

    def test_diagnostic_inventory_only_accepts_exact_approved_delta(self):
        baseline = {"main_game.exe": {"sha256": "binary"}, "feedback.frag": {"sha256": "old"},
                    "cache.json": {"sha256": "old-cache"}, "resources/page.bin": {"sha256": "asset"}}
        stage = {**baseline, "feedback.frag": {"sha256": "new"}, "cache.json": {"sha256": "new-cache"}}
        approved = {"feedback.frag", "cache.json"}
        self.assertEqual(len(audit_stage_delta(baseline, stage, approved)), 2)
        with self.assertRaisesRegex(ValueError, "outside exact approved"):
            audit_stage_delta(baseline, {**stage, "resources/page.bin": {"sha256": "tampered"}}, approved)
        with self.assertRaisesRegex(ValueError, "outside exact approved"):
            audit_stage_delta(baseline, {**stage, "cache.json": baseline["cache.json"]}, approved)

    def test_diagnostic_inventory_rejects_added_or_removed_inputs(self):
        baseline = {"main_game.exe": {}, "feedback.frag": {"sha256": "old"}}
        with self.assertRaisesRegex(ValueError, "added or removed"):
            audit_stage_delta(baseline, {**baseline, "extra.bin": {}}, {"feedback.frag"})
        with self.assertRaisesRegex(ValueError, "added or removed"):
            audit_stage_delta(baseline, {"main_game.exe": {}}, {"feedback.frag"})

    def bmp(self):
        raw = bytearray(70)
        raw[:2] = b"BM"
        struct.pack_into("<I", raw, 2, len(raw))
        struct.pack_into("<I", raw, 10, 54)
        struct.pack_into("<IiiHHII", raw, 14, 40, 2, 2, 1, 24, 0, 16)
        return raw

    def test_partial_bmp_is_pending_even_after_writer_saved_log(self):
        path = self.output / "shot.bmp"
        raw = self.bmp()
        path.write_bytes(raw[:60])
        self.stream.line("Screenshot saved: " + str(path))
        self.assertIsNone(completed_screenshot(path, self.stream, [2, 2]))
        path.write_bytes(raw)
        proof = completed_screenshot(path, self.stream, [2, 2])
        self.assertEqual(proof["artifact"]["bytes"], 70)
        self.assertEqual(proof["rowStride"], 8)
        self.assertEqual(proof["headerDeclaredBytes"], 70)
        self.assertFalse(proof["exactScreenshotFrameBound"])

    def test_complete_bmp_without_saved_log_remains_pending(self):
        path = self.output / "shot.bmp"
        path.write_bytes(self.bmp())
        self.assertIsNone(completed_screenshot(path, self.stream, [2, 2]))
        self.stream.line("Screenshot saved: " + str(self.output / "other.bmp"))
        self.assertIsNone(completed_screenshot(path, self.stream, [2, 2]))

    def test_screenshot_wait_retries_until_log_and_complete_payload(self):
        path = self.output / "shot.bmp"
        raw = self.bmp()
        polls = []

        def observe(**kwargs):
            polls.append(kwargs)
            if len(polls) == 1:
                path.write_bytes(raw[:60])
            if len(polls) == 2:
                self.stream.line("Screenshot saved: " + str(path))
            if len(polls) == 3:
                path.write_bytes(raw)

        with patch.object(self.stream, "poll", side_effect=observe), patch("rvt_full_tour.time.sleep"):
            proof = wait_screenshot(path, self.stream, [2, 2], lambda: True)
        self.assertEqual(len(polls), 3)
        self.assertEqual(proof["artifact"]["bytes"], len(raw))

    def test_screenshot_invalid_extent_offset_and_trailing_bytes_rejected(self):
        path = self.output / "shot.bmp"
        self.stream.line("Screenshot saved: " + str(path))
        path.write_bytes(self.bmp())
        self.assertIsNone(completed_screenshot(path, self.stream, [3, 2]))
        raw = self.bmp()
        struct.pack_into("<I", raw, 10, 55)
        path.write_bytes(raw)
        self.assertIsNone(completed_screenshot(path, self.stream, [2, 2]))
        raw = self.bmp() + b"extra"
        struct.pack_into("<I", raw, 2, len(raw))
        path.write_bytes(raw)
        self.assertIsNone(completed_screenshot(path, self.stream, [2, 2]))

    def test_terminal_partial_screenshot_never_produces_bound_artifact(self):
        path = self.output / "shot.bmp"
        path.write_bytes(self.bmp()[:60])
        self.stream.line("Screenshot saved: " + str(path))
        with patch.object(self.stream, "poll"):
            with self.assertRaisesRegex(ValueError, "Screenshot incomplete"):
                wait_screenshot(path, self.stream, [2, 2], lambda: False)

    def test_smoke_four_and_complete53_plan(self):
        shots = [{"name": name, "position": [0, 1, 0], "look_at": [0, 0, 0]}
                 for name in ("far", "mid", "near", "tile00_near", "cross_tile_corner")]
        for category, count in (("center", 16), ("edge", 24), ("junction", 9), ("outer", 4)):
            shots.extend({"name": f"{category}{index}", "category": category,
                          "position": [0, 1, 0], "look_at": [0, 0, 0]} for index in range(count))
        capture = {"shots": shots}
        self.assertEqual(len(route(capture, "smoke")), 4)
        self.assertEqual(route(capture, "coverage", 52, 1)[0]["category"], "outer")
        self.assertEqual(len(route(capture, "coverage", 0, 8)), 8)
        with self.assertRaisesRegex(ValueError, "1..8"):
            route(capture, "coverage", 0, 53)


class DedicatedEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name).resolve()
        self.path = self.output / "runtime-events.jsonl"

    def encode(self, events, incomplete=False):
        lines = []
        for sequence, (category, payload) in enumerate(events, 1):
            lines.append(json.dumps({"sequence": sequence, "steady_ns": 1000 + sequence,
                                     "threadTag": 42, "outputCategory": "explicitoutput",
                                     "eventClass": category, "payload": payload},
                                    ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n")
        terminal = {"sequence": len(lines) + 1, "steady_ns": 1001 + len(lines), "threadTag": 42,
                    "outputCategory": "explicitoutput", "eventClass": "runtimeEvidenceEnd",
                    "payload": {"incomplete": incomplete, "recordsBeforeTerminal": len(lines),
                                "bytesBeforeTerminal": sum(len(line) for line in lines)}}
        lines.append(json.dumps(terminal, separators=(",", ":")).encode("utf-8") + b"\n")
        return lines

    def reader(self):
        return RuntimeEvidenceStream(self.output, artifact_capture=False)

    def test_dedicated_sequence_and_exact_utf8_terminal_bytes(self):
        lines = self.encode([("NOTE", {"text": "地形\n边界"}), ("CPU_RAW", {"frame": 9})])
        self.path.write_bytes(b"".join(lines))
        reader = self.reader()
        reader.poll(final=True)
        self.assertEqual(reader.footer["payload"]["bytesBeforeTerminal"], sum(map(len, lines[:-1])))
        self.assertEqual(reader.footer["payload"]["recordsBeforeTerminal"], 2)
        self.assertEqual(reader.integrity()["parsedBytes"], self.path.stat().st_size)
        self.assertFalse(reader.integrity()["stdoutStructuredFallback"])

    def test_dedicated_partial_record_waits_for_newline(self):
        lines = self.encode([("CPU_RAW", {"frame": 9})])
        self.path.write_bytes(lines[0][:20])
        reader = self.reader()
        reader.poll()
        self.assertEqual(reader.envelopes, [])
        with self.path.open("ab") as target:
            target.write(lines[0][20:] + lines[1])
        reader.poll(final=True)
        self.assertEqual(len(reader.envelopes), 2)

    def test_no_stdout_fallback_even_if_console_has_valid_or_corrupt_json(self):
        (self.output / "stdout.log").write_bytes(
            b'[RVT_QUALITY_EVENT] i{"event":"capture_submitted"}\n'
            b'[CPU_RAW] {"frame":999}\n')
        self.path.write_bytes(b"".join(self.encode([("CPU_RAW", {"frame": 9})])))
        reader = self.reader()
        reader.poll(final=True)
        self.assertEqual([row["frame"] for row in reader.records], [9])
        self.assertEqual(reader.invalid_lines, [])

    def test_missing_dedicated_file_fails_even_with_complete_stdout(self):
        (self.output / "stdout.log").write_bytes(b'[CPU_RAW] {"frame":999}\n')
        with self.assertRaisesRegex(ValueError, "missing runtimeEvidenceEnd"):
            self.reader().poll(final=True)

    def test_missing_footer_fails(self):
        self.path.write_bytes(self.encode([("CPU_RAW", {"frame": 9})])[0])
        with self.assertRaisesRegex(ValueError, "missing runtimeEvidenceEnd"):
            self.reader().poll(final=True)

    def test_sequence_start_gap_duplicate_and_reorder_fail(self):
        for value in (0, 2, True, 9):
            with self.subTest(sequence=value):
                lines = self.encode([("CPU_RAW", {"frame": 9})])
                row = json.loads(lines[0])
                row["sequence"] = value
                lines[0] = json.dumps(row).encode() + b"\n"
                self.path.write_bytes(b"".join(lines))
                with self.assertRaisesRegex(ValueError, "sequence"):
                    self.reader().poll(final=True)
        lines = self.encode([("CPU_RAW", {"frame": 9}), ("CPU_RAW", {"frame": 10})])
        self.path.write_bytes(lines[0] + lines[0] + lines[2])
        with self.assertRaisesRegex(ValueError, "sequence"):
            self.reader().poll(final=True)

    def test_terminal_byte_and_record_counts_cannot_be_inferred(self):
        for field in ("bytesBeforeTerminal", "recordsBeforeTerminal"):
            with self.subTest(field=field):
                lines = self.encode([("NOTE", {"text": "地形"})])
                terminal = json.loads(lines[-1])
                terminal["payload"][field] += 1
                lines[-1] = json.dumps(terminal).encode() + b"\n"
                self.path.write_bytes(b"".join(lines))
                with self.assertRaisesRegex(ValueError, "byte count mismatch"):
                    self.reader().poll(final=True)

    def test_incomplete_terminal_is_failure(self):
        self.path.write_bytes(b"".join(self.encode([("CPU_RAW", {"frame": 9})], incomplete=True)))
        with self.assertRaisesRegex(ValueError, "terminal incomplete"):
            self.reader().poll(final=True)

    def test_record_after_footer_is_failure(self):
        lines = self.encode([("CPU_RAW", {"frame": 9})])
        self.path.write_bytes(b"".join(lines) + lines[0])
        with self.assertRaisesRegex(ValueError, "Record after"):
            self.reader().poll(final=True)

    def test_bad_json_duplicate_keys_nonfinite_and_utf8_are_failure(self):
        for data in (b'i{"sequence":1}\n', b'{"sequence":1,"sequence":1}\n',
                     b'{"payload":NaN}\n', b'{"text":"\xff"}\n', b"\n"):
            with self.subTest(data=data):
                self.path.write_bytes(data)
                with self.assertRaises((ValueError, UnicodeError)):
                    self.reader().poll(final=True)

    def test_terminal_line_without_newline_is_failure(self):
        self.path.write_bytes(b"".join(self.encode([("CPU_RAW", {"frame": 9})]))[:-1])
        with self.assertRaisesRegex(ValueError, "partial/non-newline"):
            self.reader().poll(final=True)

    def test_sink_rewrite_or_truncation_cannot_reuse_observed_records(self):
        lines = self.encode([("CPU_RAW", {"frame": 9})])
        self.path.write_bytes(b"".join(lines))
        reader = self.reader()
        reader.poll()
        changed = b"".join(lines).replace(b'"frame":9', b'"frame":8')
        self.path.write_bytes(changed)
        with self.assertRaisesRegex(ValueError, "bytes changed"):
            reader.poll(final=True)
        self.path.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "truncated"):
            reader.poll(final=True)

    def test_raw_is_bound_only_to_exact_dedicated_completed_epoch(self):
        raw, event, surfaces = QualityBinaryTests().fixture()
        quality = self.output / "quality"
        quality.mkdir()
        binary = quality / "capture-1.bin"
        binary.write_bytes(raw)
        metadata = {**event, "protocol": 1, "source": "gpu-page-table-readback",
                    "binaryAbi": "TVQ1-final-results-v1", "moduleOnly": False,
                    "cameraPosition": [0, 0, 0], "cameraLookAt": [0, 0, -1],
                    "capturedMonotonicMs": 400, "completedMonotonicMs": 500}
        binary.with_suffix(".json").write_text(json.dumps(metadata))
        state = {"worldGeneration": 1, "poseId": "7", "submissionEpoch": 2, "completedEpoch": 2,
                 "visible": 1, "validAtRawTargetPlusOne": 1, "unresolved": 0, "incorrectIdentity": 0,
                 "rootOnly": 0, "elapsedMs": 400, "interactive": True, "convergenceFailed": False}
        base = {"worldGeneration": 1, "poseId": "7", "submissionEpoch": 2}
        events = [("RVT_QUALITY_EVENT", {**base, "event": "capture_submitted"}),
                  ("RVT_QUALITY_STATE", state),
                  ("RVT_QUALITY_EVENT", {**base, "event": "capture_completed"})]
        lines = self.encode(events)
        self.path.write_bytes(b"".join(lines[:2]))
        reader = RuntimeEvidenceStream(self.output)
        reader.poll()
        self.assertEqual([row for row in reader.records if row["nativePrefix"] in CAPTURE_CLASSES], [])
        with self.path.open("ab") as target:
            target.write(b"".join(lines[2:]))
        reader.poll(final=True)
        candidates = [row for row in reader.records if row["nativePrefix"] == "BOUND_CAPTURE_ARTIFACT"]
        self.assertEqual(len(candidates), 1)
        observation = decode_capture(candidates[0], reader, surfaces)
        self.assertEqual(observation["metadata"]["frame"], 4)
        self.assertEqual(observation["counts"]["validAtTargetPlusBias"], 1)
        self.assertIn("not a native event", candidates[0]["authority"])

    def lifecycle_payload(self, event, submitted=True):
        payload = {"event": event, "worldGeneration": 1, "poseId": 7, "frame": 4,
                   "drawSubmissionEpoch": 2, "submissionEpoch": 2 if submitted else 0,
                   "captureHandle": 42, "submitted": submitted,
                   "measurementProvided": event == "capture_completed"}
        if event == "capture_completed":
            payload["completedEpoch"] = 2
        if event == "capture_aborted":
            payload["reason"] = "normalShutdown"
        return payload

    def feed_lifecycle(self, payloads):
        self.path.write_bytes(b"".join(self.encode([("RVT_QUALITY_EVENT", row) for row in payloads])))
        reader = self.reader()
        reader.poll(final=True)
        return reader

    def test_abort01_exact_submitted_abort_closes_only_pending_without_measurement(self):
        reader = self.feed_lifecycle([self.lifecycle_payload("capture_submitted"),
                                      self.lifecycle_payload("capture_aborted")])
        self.assertEqual(reader.submitted, set())
        self.assertEqual(reader.completed, {})
        self.assertEqual(len(reader.aborted), 1)
        self.assertEqual(reader.footer["payload"]["recordsBeforeTerminal"], 2)
        self.assertFalse(any(row["nativePrefix"] in CAPTURE_CLASSES for row in reader.records))

    def test_abort02_unsubmitted_abort_is_not_a_submission_or_measurement(self):
        reader = self.feed_lifecycle([self.lifecycle_payload("capture_aborted", submitted=False)])
        self.assertEqual(reader.submitted, set())
        self.assertEqual(reader.completed, {})
        self.assertEqual(reader.aborted, {})
        self.assertEqual(len(reader.unsubmitted_aborts), 1)

    def test_abort03_wrong_recorded_identity_cannot_close_submission(self):
        for field in ("worldGeneration", "poseId", "frame", "captureHandle", "drawSubmissionEpoch"):
            with self.subTest(field=field):
                aborted = self.lifecycle_payload("capture_aborted")
                aborted[field] += 1
                with self.assertRaises(ValueError):
                    self.feed_lifecycle([self.lifecycle_payload("capture_submitted"), aborted])

    def test_abort04_duplicate_abort_is_a_double_end(self):
        with self.assertRaisesRegex(ValueError, "after exact capture abort"):
            self.feed_lifecycle([self.lifecycle_payload("capture_submitted"),
                                 self.lifecycle_payload("capture_aborted"),
                                 self.lifecycle_payload("capture_aborted")])

    def test_abort05_completed_after_abort_cannot_manufacture_a_measurement(self):
        with self.assertRaisesRegex(ValueError, "after exact capture abort"):
            self.feed_lifecycle([self.lifecycle_payload("capture_submitted"),
                                 self.lifecycle_payload("capture_aborted"),
                                 self.lifecycle_payload("capture_completed")])

    def test_abort06_abort_after_completed_raw_measurement_fails(self):
        with self.assertRaises(ValueError):
            self.feed_lifecycle([self.lifecycle_payload("capture_submitted"),
                                 self.lifecycle_payload("capture_completed"),
                                 self.lifecycle_payload("capture_aborted")])

    def test_abort07_unsubmitted_false_cannot_pair_existing_submission(self):
        reader = self.reader()
        reader.accept_record("RVT_QUALITY_EVENT", self.lifecycle_payload("capture_submitted"))
        with self.assertRaisesRegex(ValueError, "Unsubmitted abort cannot close"):
            reader.accept_record("RVT_QUALITY_EVENT",
                                 self.lifecycle_payload("capture_aborted", submitted=False))
        self.assertEqual(reader.submitted, {(1, "7", 2)})
        self.assertEqual(reader.aborted, {})

    def test_abort08_measurement_claim_or_completed_epoch_is_forbidden(self):
        for override in ({"measurementProvided": True}, {"completedEpoch": 2}, {"reason": ""}):
            with self.subTest(override=override):
                aborted = {**self.lifecycle_payload("capture_aborted"), **override}
                with self.assertRaisesRegex(ValueError, "no measurement"):
                    self.feed_lifecycle([self.lifecycle_payload("capture_submitted"), aborted])

    def test_abort09_raw_from_aborted_capture_never_becomes_ready(self):
        self.stream = NativeStream(self.output)
        record, surfaces = FullTourTests.capture(self)
        reader = self.feed_lifecycle([self.lifecycle_payload("capture_submitted"),
                                      self.lifecycle_payload("capture_aborted")])
        with self.assertRaisesRegex(ValueError, "Aborted capture cannot provide"):
            decode_capture(record, reader, surfaces)
        self.assertEqual(reader.completed, {})

    def test_abort10_used_raw_rejects_abort_and_wrong_completion_identity(self):
        self.stream = NativeStream(self.output)
        record, surfaces = FullTourTests.capture(self)
        state = self.stream.states[(1, "7", 2)]
        events = [("RVT_QUALITY_EVENT", self.lifecycle_payload("capture_submitted")),
                  ("RVT_QUALITY_STATE", state),
                  ("RVT_QUALITY_EVENT", self.lifecycle_payload("capture_completed"))]
        self.path.write_bytes(b"".join(self.encode(events)))
        reader = self.reader()
        reader.poll(final=True)
        self.assertEqual(decode_capture(record, reader, surfaces)["counts"]["validAtTargetPlusBias"], 1)
        with self.assertRaises(ValueError):
            reader.accept_record("RVT_QUALITY_EVENT", self.lifecycle_payload("capture_aborted"))
        reader.completed[(1, "7", 2)]["payload"]["frame"] = 99
        with self.assertRaisesRegex(ValueError, "completed measurement identity differs"):
            decode_capture(record, reader, surfaces)

    def test_abort11_completion_cannot_drop_submitted_record_identity_fields(self):
        legacy_completion = {"event": "capture_completed", "worldGeneration": 1,
                             "poseId": 7, "submissionEpoch": 2}
        with self.assertRaisesRegex(ValueError, "cannot drop recorded lifecycle identity"):
            self.feed_lifecycle([self.lifecycle_payload("capture_submitted"), legacy_completion])

    def test_matrix_keeps_53_per_mode_worker_and_separates_routes(self):
        capture = {"shots": [{"name": name, "position": [0, 1, 0], "look_at": [0, 0, 0]}
                             for name in ("far", "mid", "near", "tile00_near", "cross_tile_corner")]}
        for category, count in (("center", 16), ("edge", 24), ("junction", 9), ("outer", 4)):
            capture["shots"].extend({"name": f"{category}{index}", "category": category,
                                     "position": [0, 1, 0], "look_at": [0, 0, 0]}
                                    for index in range(count))
        cases = matrix_cases(capture)
        self.assertEqual(len(cases), 40)
        for mode in ("CoverageAtlas", "LocalLayerArray"):
            for worker in (1, 2):
                selected = [case for case in cases if case["mode"] == mode and case["worker"] == worker]
                labels = [shot["name"] for case in selected if case["selection"] == "coverage"
                          for shot in case["shots"]]
                self.assertEqual(len(labels), 53)
                self.assertEqual(len(set(labels)), 53)
                self.assertEqual({case["selection"] for case in selected},
                                 {"coverage", "core", "continuous", "teleport"})


if __name__ == "__main__":
    unittest.main()

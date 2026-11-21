"""CPU contracts for the real Game runner, never GPU/World acceptance."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import rvt_fault_matrix as matrix
import rvt_game_fault_runner as runner
from rvt_fault_matrix_test import fixture
from rvt_native_adapter import FaultEvidence, fault_frame_witness


def envelope(sequence, category, payload):
    return {"sequence": sequence, "steady_ns": sequence * 100, "threadTag": 7,
            "outputCategory": "explicitoutput", "eventClass": category, "payload": payload}


def write_sink(path, rows, footer=True):
    encoded = [json.dumps(row, separators=(",", ":")).encode("utf-8") + b"\n" for row in rows]
    if footer:
        encoded.append(json.dumps(envelope(len(rows) + 1, "runtimeEvidenceEnd", {
            "incomplete": False, "recordsBeforeTerminal": len(rows),
            "bytesBeforeTerminal": sum(map(len, encoded))}), separators=(",", ":")).encode() + b"\n")
    path.write_bytes(b"".join(encoded))


def frames():
    identity = {"identityVersion": 2, "renderFrame": 701, "worldGeneration": 42, "submitEpoch": 702}
    return [envelope(1, "CPU_RAW", {**identity, "event": "frame-timing", "acquiredReady": True,
                                   "renderFrameIntervalMs": 789.123}),
            envelope(2, "GPU_RAW", {**identity, "fenceCompletedEpoch": 702, "readbackSequence": 3})]


class GameFaultRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.package = fixture(self.root)
        self.capture = self.root / "camera.json"
        matrix.save(self.capture, {"shots": [
            {"name": "far", "position": [1, 2, 3], "look_at": [0, 0, 0]},
            {"name": "tile00_near", "position": [4, 5, 6], "look_at": [0, 0, 0]}]})

    def tearDown(self):
        self.temp.cleanup()

    def args(self, output):
        return ["--package", str(self.package), "--output", str(output),
                "--capture-plan", str(self.capture)]

    def evidence(self, payloads):
        result = FaultEvidence(self.root / "events.jsonl")
        result.records = [envelope(index, "RVT_EVIDENCE", payload)
                          for index, payload in enumerate(payloads, 1)]
        return result

    def request(self, kind):
        return {"kind": kind, "faultId": "cpu-contract", "worldGeneration": 42,
                "surfaceId": "terrain|Landscape_00", "mip": 3, "x": 0, "y": 0, "revision": 7,
                "status": 2, "submitResult": -1, "replacementRevision": 8,
                "testAdmissionLimitBytes": 1, "slot": 4, "writeEpoch": 11}

    def test_both_modes_workers_all_ten_cases_plan_without_process(self):
        local = fixture(self.root / "local")
        manifest = local / "resources/VT/world/manifest.json"
        document = matrix.read_json(manifest)
        document["production"]["surfaces"][0]["weightSource"] = 1
        matrix.save(manifest, document)
        output = self.root / "plan"
        with patch("subprocess.Popen", side_effect=AssertionError("GPU forbidden")):
            self.assertEqual(matrix.main(self.args(output) + [
                "--package-local", str(local), "--workers", "1", "2"]), 0)
        report = matrix.read_json(output / "matrix.json")
        self.assertEqual(len(report["jobs"]), 40)
        self.assertEqual({(row["weightSource"], row["worker"]) for row in report["jobs"]},
                         {(0, 1), (0, 2), (1, 1), (1, 2)})
        self.assertFalse(report["acceptancePassed"])
        self.assertFalse(report["rendererStarted"])
        self.assertTrue(all("--execute" in row["executeCommand"] for row in report["jobs"]))
        self.assertFalse((output / "private").exists())

    def test_old_case_alias_uses_real_schema_and_no_fake_mutation(self):
        output = self.root / "alias"
        self.assertEqual(matrix.main(self.args(output) + ["--case", "submit_failure"]), 0)
        jobs = matrix.read_json(output / "matrix.json")["jobs"]
        self.assertEqual([row["case"] for row in jobs], ["submitFail"])

    def test_execute_fixture_is_rejected_before_any_game_or_verifier(self):
        with redirect_stdout(io.StringIO()), patch("subprocess.Popen",
            side_effect=AssertionError("No process")):
            self.assertEqual(matrix.main(self.args(self.root / "execute") + [
                "--execute", "--case", "statusNonzero",
                "--binary-sha256", matrix.sha256(self.package / "main_game.exe")]), 1)

    def test_invalid_live_page_and_identity_bounds_fail_cpu(self):
        for flag, value in (("--mip", "-1"), ("--world-generation", "0"),
                            ("--slot-publish-epoch", "0"), ("--status", "4294967296")):
            with redirect_stdout(io.StringIO()):
                self.assertEqual(matrix.main(self.args(self.root / ("bad-" + flag[2:])) +
                                             [flag, value]), 1)

    def test_request_uses_actual_world_revision_and_no_token_fields(self):
        snapshot = {"worldGeneration": 993, "revision": 71, "surfaceId": "terrain|real"}
        options = SimpleNamespace(status=4, submit_result=-1, replacement_revision=None,
                                  admission_limit=7, slot=None, slot_publish_epoch=None)
        target = {"mip": 3, "x": 2, "y": 1}
        request = runner.request_for("submitFail", snapshot, target, "explicit", options)
        self.assertEqual(request["worldGeneration"], 993)
        self.assertEqual(request["revision"], 71)
        self.assertNotIn("contentIdentity", request)
        self.assertNotIn("submissionEpoch", request)
        self.assertEqual(runner.request_for("staleRevision", snapshot, target, "stale", options)[
            "replacementRevision"], 72)
        with self.assertRaisesRegex(ValueError, "never infer"):
            runner.request_for("slotReuse", snapshot, target, "slot", options)

    def test_dedicated_footer_exact_bytes_and_incremental_partial_line(self):
        path = self.root / "sink"
        write_sink(path, frames())
        data = path.read_bytes()
        path.write_bytes(data[:31])
        evidence = FaultEvidence(path)
        self.assertEqual(evidence.poll(), [])
        with path.open("ab") as stream:
            stream.write(data[31:])
        evidence.poll(final=True)
        self.assertEqual(len(evidence.records), 3)
        self.assertEqual(evidence.bytes, len(data))

    def test_missing_footer_and_incomplete_footer_fail(self):
        path = self.root / "sink"
        write_sink(path, frames(), footer=False)
        with self.assertRaisesRegex(ValueError, "runtimeEvidenceEnd"):
            FaultEvidence(path).poll(final=True)
        write_sink(path, frames())
        path.write_bytes(path.read_bytes().replace(b'"incomplete":false', b'"incomplete":true'))
        with self.assertRaisesRegex(ValueError, "terminal incomplete"):
            FaultEvidence(path).poll(final=True)

    def test_duplicate_json_sequence_footer_bytes_or_after_terminal_fail(self):
        path = self.root / "sink"
        write_sink(path, frames())
        valid = path.read_bytes()
        for broken in (
            valid.replace(b'"sequence":2', b'"sequence":1'),
            valid.replace(b'"bytesBeforeTerminal":', b'"bytesBeforeTerminal":999,"ignored":'),
            valid + json.dumps(envelope(4, "CPU_RAW", {})).encode() + b"\n",
            valid.replace(b'"threadTag":7', b'"threadTag":7,"threadTag":8'),
        ):
            path.write_bytes(broken)
            with self.assertRaises(ValueError):
                FaultEvidence(path).poll(final=True)

    def test_raw_frames_match_recorded_identity_not_readback_counter(self):
        result = fault_frame_witness(frames(), 42)
        self.assertEqual(result["gpu"]["payload"]["renderFrame"], 701)
        self.assertEqual(result["gpu"]["payload"]["readbackSequence"], 3)
        self.assertIsNone(fault_frame_witness(frames(), 43))
        self.assertIsNone(fault_frame_witness(frames(), 42, after_epoch=702))

    def test_late_readback_of_pre_fault_cpu_frame_is_not_later_world_render(self):
        self.assertIsNone(fault_frame_witness(frames(), 42, after_sequence=1))
        rows = frames()
        rows[1]["payload"]["fenceCompletedEpoch"] = 701
        self.assertIsNone(fault_frame_witness(rows, 42))
        rows = frames()
        rows[1]["payload"]["worldGeneration"] = 43
        self.assertIsNone(fault_frame_witness(rows, 42))

    def test_duplicate_gpu_and_old_counter_only_identity_fail(self):
        with self.assertRaisesRegex(ValueError, "Duplicate GPU"):
            fault_frame_witness(frames() + [frames()[1]], 42)
        with self.assertRaisesRegex(ValueError, "R9 recording identity"):
            fault_frame_witness([envelope(1, "GPU_RAW", {"frame": 3})], 42)

    def test_missing_native_owner_or_missing_event_never_passes(self):
        evidence = self.evidence([])
        self.assertIsNone(runner.fault_terminal("statusNonzero", evidence, self.request("statusNonzero")))
        evidence.records = [envelope(1, "RVT_FAULT_CONTROL",
            {"faultId": "cpu-contract", "result": "pendinghookmissing"})]
        with self.assertRaisesRegex(ValueError, "Native fault request rejected"):
            runner.control_event(evidence, "cpu-contract")

    def test_status_preserves_actual_failure_backoff_no_publication(self):
        request = self.request("statusNonzero")
        injected = {**request, "event": "fault-injection", "originalStatus": 0, "injectedStatus": 2}
        failed = {**request, "event": "production-failure", "status": 2,
                  "publishedChannels": 0, "writeEpoch": 13, "fenceCompletedEpoch": 13,
                  "attempts": 1, "maxAttempts": 3, "backoffTicks": 2}
        self.assertIsNotNone(runner.fault_terminal("statusNonzero", self.evidence([injected, failed]), request))
        for key, value in (("publishedChannels", 3), ("attempts", 4), ("fenceCompletedEpoch", 12),
                           ("revision", 8)):
            with self.assertRaises(ValueError):
                runner.fault_terminal("statusNonzero", self.evidence([injected, {**failed, key: value}]), request)

    def test_submit_actual_abort_counts_and_exact_task_cannot_be_faked(self):
        request = self.request("submitFail")
        consumed = {**request, "event": "submit-fault-consumed", "abortTicket": 8,
                    "submissionEpoch": 13, "contentIdentity": "a" * 64}
        aborted = {**consumed, "event": "submit-abort-observed", "rawSubmitResult": None,
                   "sceneSubmitCalls": 0, "publishedChannels": 0, "recordedTasks": 1, "abortedTasks": 1,
                   "activeGenerationBefore": 42, "activeGenerationAfter": 42}
        self.assertIsNotNone(runner.fault_terminal("submitFail", self.evidence([consumed, aborted]), request))
        for key, value in (("rawSubmitResult", -4), ("sceneSubmitCalls", 1), ("abortedTasks", 0),
                           ("activeGenerationAfter", 43), ("abortTicket", 9)):
            with self.assertRaises(ValueError):
                runner.fault_terminal("submitFail", self.evidence([consumed, {**aborted, key: value}]), request)

    def test_budget_only_lower_actual_admission_can_be_observed(self):
        request = self.request("budgetReject")
        actual = {**request, "event": "candidate-budget-admission", "validStrictLowerLimit": True,
                  "rejected": True, "testLimit": 1, "actualFrozenLimit": 10, "actualDemand": 9}
        self.assertIsNotNone(runner.fault_terminal("budgetReject", self.evidence([actual]), request))
        for key, value in (("actualFrozenLimit", 1), ("actualDemand", 1), ("rejected", False)):
            with self.assertRaises(ValueError):
                runner.fault_terminal("budgetReject", self.evidence([{**actual, key: value}]), request)

    def test_stale_requires_actual_hold_unchanged_old_task_and_newer_commit(self):
        request = self.request("staleRevision")
        held = {**request, "event": "actual-completion-held", "writeEpoch": 10,
                "reservation": 2, "contentIdentity": "a" * 64}
        delivered = {**request, "event": "actual-stale-completion-delivered", "writeEpoch": 10,
                     "completedEpoch": 20, "reservation": 2, "oldContentIdentity": "a" * 64,
                     "oldRevision": 7, "normalRuntimeAccepted": False, "cancelledOrTimedOut": False}
        evidence = self.evidence([held, delivered])
        with self.assertRaisesRegex(ValueError, "actual newer joint commit"):
            runner.fault_terminal("staleRevision", evidence, request)
        evidence.records.insert(1, envelope(2, "RVT_WORLD_CANDIDATE", {"worldGeneration": 42, "revision": 8}))
        evidence.records[-1]["sequence"] = 3
        self.assertIsNotNone(runner.fault_terminal("staleRevision", evidence, request))
        evidence.records[-1]["payload"]["normalRuntimeAccepted"] = True
        with self.assertRaises(ValueError):
            runner.fault_terminal("staleRevision", evidence, request)

    def test_slot_requires_actual_armed_dto_and_boundary_then_new_occupant(self):
        request = self.request("slotReuse")
        original = {"slot": 4, "reservation": 8, "publishEpoch": 11,
                    "publishedToken": {key: request[key] for key in
                        ("worldGeneration", "surfaceId", "mip", "x", "y", "revision")}}
        observed = {"original": original, "current": {"slot": 4, "reservation": 9,
                    "reservedToken": {"revision": 8}}, "replacementReservation": 9,
                    "releasedAtBoundary": True, "replacementObserved": True,
                    "replacementStillPresent": True, "retirementObserved": True,
                    "blockedByEpoch": 1, "blockedByOwner": 2, "releaseCompletedEpoch": 15,
                    "releaseRetireEpoch": 14, "releaseReferenceEpoch": 15}
        armed = {**request, "event": "slot-watch-armed", "observation": {"original": original}}
        replacement = {**request, "event": "slot-watch-replacement-observed", "observation": observed}
        self.assertIsNone(runner.fault_terminal("slotReuse", self.evidence([replacement]), request))
        self.assertIsNotNone(runner.fault_terminal("slotReuse", self.evidence([armed, replacement]), request))
        observed["releaseCompletedEpoch"] = 13
        with self.assertRaises(ValueError):
            runner.fault_terminal("slotReuse", self.evidence([armed, replacement]), request)

    def test_native_duplicate_one_shot_event_and_timeout_cannot_pass(self):
        request = self.request("slotReuse")
        with self.assertRaisesRegex(ValueError, "did not finish"):
            runner.fault_terminal("slotReuse", self.evidence([{**request, "event": "slot-watch-timeout"}]), request)
        request = self.request("budgetReject")
        event = {**request, "event": "candidate-budget-admission"}
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            runner.fault_terminal("budgetReject", self.evidence([event, event]), request)

    def test_shader_runtime_outputs_restore_after_exception_without_touching_source(self):
        private = self.root / "disposable"
        shader = private / "shader/spv"
        shader.mkdir(parents=True)
        (shader / "cache.json").write_bytes(b"original")
        output = self.root / "evidence"
        output.mkdir()
        with self.assertRaisesRegex(RuntimeError, "CPU interruption"):
            with runner.restore_shader_outputs(private, output):
                (shader / "cache.json").write_bytes(b"changed")
                (shader / "new.spv").write_bytes(b"new")
                raise RuntimeError("CPU interruption")
        self.assertEqual((shader / "cache.json").read_bytes(), b"original")
        self.assertFalse((shader / "new.spv").exists())
        self.assertEqual((self.package / "main_game.exe").read_bytes(), b"CPU fixture, never executable")

    def test_private_root_bytes_restore_if_actual_launch_path_fails(self):
        private, output = self.root / "private", self.root / "run-evidence"
        matrix.copy_package(self.package, private)
        output.mkdir()
        matrix.save(output / "private-copy.json", {
            "privatePackage": str(private), "sourcePackage": str(self.package)})
        manifest = private / "resources/VT/world/manifest.json"
        document = matrix.read_json(manifest)
        document["surfaces"] = [{"surfaceId": "terrain|Landscape_00", "width": 8192, "height": 8192}]
        matrix.save(manifest, document)
        before = matrix.inventory(private)
        options = SimpleNamespace(capture_plan=self.capture, baseline_camera="far", camera="tile00_near",
                                  mip=3, page_x=0, page_y=0)
        with patch.object(runner, "verify_mutated", return_value={"scope": "CPU restore test only"}), \
             patch.object(runner, "Game", side_effect=ValueError("CPU launch-path failure")):
            with self.assertRaisesRegex(ValueError, "CPU launch-path failure"):
                runner.run_case(private, output, options, "root_payload")
        self.assertEqual(matrix.inventory(private), before)
        self.assertTrue(matrix.read_json(output / "mutation.json")["restored"])


if __name__ == "__main__":
    unittest.main()

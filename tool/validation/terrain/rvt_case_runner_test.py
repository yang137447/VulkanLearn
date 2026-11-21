"""CPU runner scheduling/mutation and raw GPU sample rejection tests."""

import copy
import json
from pathlib import Path
import tempfile
import unittest

from run_rvt_case import mutation, schedule
from rvt_sample_evidence import quality_counts
from rvt_artifact_binding import bind_artifact, load_quality_json
from rvt_delivery import artifact
from rvt_snapshot_lock import validate_snapshot


class ArtifactBindingTests(unittest.TestCase):
    def test_changed_size_or_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "raw.bin"
            path.write_bytes(b"actual GPU bytes")
            entry = dict(artifact(path), path="raw.bin")
            self.assertEqual(bind_artifact(root, entry), path.resolve())
            path.write_bytes(b"changed GPU data")
            with self.assertRaises(ValueError):
                bind_artifact(root, entry)

    def test_escaped_and_oversized_artifact_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "raw.bin"
            path.write_bytes(b"data")
            entry = dict(artifact(path), path="raw.bin")
            with self.assertRaisesRegex(ValueError, "bounded size"):
                bind_artifact(root, entry, maximum=3)
            entry["path"] = "../raw.bin"
            with self.assertRaises(ValueError):
                bind_artifact(root, entry)

    def test_binary_without_pinned_decoder_is_not_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "raw.bin"
            path.write_bytes(b"not guessed ABI")
            with self.assertRaisesRegex(ValueError, "pinned decoder"):
                load_quality_json(root, dict(artifact(path), path="raw.bin"))


class SnapshotLockTests(unittest.TestCase):
    def fixture(self, root):
        snapshot = root / "snapshot"
        files = []
        for name, data in (
                ("bin/main_game.exe", b"game"), ("bin/main_editor.exe", b"editor"),
                ("bin/vl_export_game.exe", b"export"),
                ("bin/vl_build_flavor.main_game.json", b'{"flavor":"game","editorEnabled":false}'),
                ("shader/glsl/test.comp", b"void main(){}"),
                ("shader/spv/shader-build-cache.json", b"{}"), ("shader/spv/test.spv", b"spv")):
            path = snapshot / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            files.append(dict(artifact(path), path=name))
        receipt = root / "snapshot.json"
        receipt.write_text(json.dumps({"kind": "rvt-stage-snapshot", "version": 1,
            "status": "locked", "owner": "Euclid", "configuration": "Release",
            "root": str(snapshot.resolve()), "files": files}))
        return snapshot, receipt

    def test_exact_snapshot_can_be_validated_without_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot, receipt = self.fixture(Path(temporary))
            self.assertEqual(validate_snapshot(receipt)[0], snapshot.resolve())

    def test_live_shader_change_and_unlisted_spv_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            snapshot, receipt = self.fixture(Path(temporary))
            (snapshot / "shader/spv/extra.spv").write_bytes(b"unlisted")
            with self.assertRaisesRegex(ValueError, "tree differs"):
                validate_snapshot(receipt)


class RunnerTests(unittest.TestCase):
    def test_mutation_restores_exact_payload_after_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "payload"
            original = bytes(range(128))
            path.write_bytes(original)
            with self.assertRaises(RuntimeError):
                with mutation((path, "corrupt-root", 32), root):
                    self.assertNotEqual(path.read_bytes(), original)
                    raise RuntimeError("injected")
            self.assertEqual(path.read_bytes(), original)

    def test_fault_case_requires_explicit_real_hook(self):
        with self.assertRaisesRegex(ValueError, "native scenario"):
            schedule("production_failure", {"shots": []}, None)

    def test_pose_commands_are_actual_camera_commands(self):
        commands = schedule("tour", {"shots": [
            {"category": "center", "position": [0, 1, 2], "look_at": [0, 0, 0]}]}, None)
        self.assertEqual(commands[0], "camera pose 0 1 2 0 0 0")
        self.assertEqual(commands.count("terrainvt stats"), 4)


class SampleTests(unittest.TestCase):
    def fixture(self):
        surface = {"width": 8192, "height": 8192, "contentHash": "a" * 64}
        owner = {"slot": 0, "surfaceId": "terrain|Landscape_00", "mip": 0, "x": 0, "y": 0,
                 "contentIdentity": "a" * 64, "revision": 1, "worldGeneration": 1}
        low = {"requestedMip": 0, "resolvedMip": 0, "x": 0, "y": 0, "entry": 1,
               "slotOwner": owner, "traversal": [{"mip": 0, "x": 0, "y": 0, "entry": 1}]}
        high = copy.deepcopy(low)
        high.update(requestedMip=1, resolvedMip=1, traversal=[{"mip": 1, "x": 0, "y": 0, "entry": 1}])
        high["slotOwner"]["mip"] = 1
        sample = {"surfaceId": "terrain|Landscape_00", "targetMip": 0.2, "effectiveMipBias": 0,
                  "feedbackPixel": [64, 64], "tapCount": 1,
                  "maxMip": 13, "expectedContentIdentity": "a" * 64, "expectedRevision": 1,
                  "taps": [{"low": low, "high": high}]}
        event = {"frame": 4, "worldGeneration": 1, "poseId": "p1", "visibleSamples": 1,
                 "validAtTargetPlusBias": 1, "effectiveMipBias": 0, "unresolved": 0,
                 "incorrectIdentity": 0, "rootOnlySamples": 0, "uninitializedDraws": 0}
        document = {"protocol": 1, "source": "gpu-page-table-readback", "captureScope": "all-fragment-taps",
                    "frame": 4, "worldGeneration": 1, "poseId": "p1", "submissionEpoch": 2,
                    "completedEpoch": 2, "uninitializedDraws": 0, "samples": [sample]}
        document.update(samplingDomain="feedback-visible-terrain", feedbackExtent=[128, 128],
                        sampleCapacity=16384, sampleCount=1, terrainVisibleCount=1,
                        overflowCount=0, droppedTerrainSamples=0)
        return document, event, {"terrain|Landscape_00": surface}

    def test_current_gpu_counts_recomputed(self):
        document, event, surfaces = self.fixture()
        self.assertEqual(quality_counts(document, event, surfaces)["validAtTargetPlusBias"], 1)

    def test_wrong_slot_identity_cannot_be_hidden_by_pass_counts(self):
        document, event, surfaces = self.fixture()
        document["samples"][0]["taps"][0]["low"]["slotOwner"]["revision"] = 0
        with self.assertRaisesRegex(ValueError, "aggregate differs"):
            quality_counts(document, event, surfaces)

    def test_cpu_estimate_and_center_only_cannot_claim_draw_quality(self):
        for field, value in (("source", "cpu-current-mapping"), ("captureScope", "center-only")):
            document, event, surfaces = self.fixture()
            document[field] = value
            with self.assertRaises(ValueError):
                quality_counts(document, event, surfaces)

    def test_pending_fence_cannot_claim_current_frame_readback(self):
        document, event, surfaces = self.fixture()
        document["completedEpoch"] = 1
        with self.assertRaises(ValueError):
            quality_counts(document, event, surfaces)

    def test_overflow_or_selectively_dropped_terrain_cannot_pass(self):
        for field in ("overflowCount", "droppedTerrainSamples"):
            document, event, surfaces = self.fixture()
            document[field] = 1
            with self.assertRaisesRegex(ValueError, "overflowing"):
                quality_counts(document, event, surfaces)

    def test_missing_captured_tap_cannot_pass(self):
        document, event, surfaces = self.fixture()
        document["samples"][0]["tapCount"] = 2
        with self.assertRaisesRegex(ValueError, "taps"):
            quality_counts(document, event, surfaces)


if __name__ == "__main__":
    unittest.main()

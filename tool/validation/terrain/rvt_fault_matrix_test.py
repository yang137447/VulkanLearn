"""CPU regression tests for exact mutations, isolation and honest fault results."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import rvt_fault_matrix as matrix


def fixture(root):
    package = root / "original"
    world_root = package / "resources/VT/world"
    world_root.mkdir(parents=True)
    (package / "config").mkdir()
    matrix.save(package / "config/config.json", {
        "resourcePath": "resources", "initScene": "scene.json"})
    frozen = matrix.ROOT / "config/terrainRvtP0Frozen.json"
    (package / "config/terrainRvtP0Frozen.json").write_bytes(frozen.read_bytes())
    (package / "config/terrainRvtP0Frozen.json.sha256").write_text(matrix.FROZEN_SHA256)
    (package / "main_game.exe").write_bytes(b"CPU fixture, never executable")
    matrix.save(package / "game-export-manifest.json", {"files": []})
    matrix.save(package / "resources/scene.json", {
        "objects": [{"type": "terrain", "name": "Landscape_00", "terrainPath": "terrain.json"}]})
    matrix.save(package / "resources/terrain.json", {
        "compositedVT": {"packagePath": "VT/world/manifest.json"}})
    (world_root / "payload.vlcvt").write_bytes(b"VL" + b"\0" * 100)
    (world_root / "image.bin").write_bytes(b"required image")
    (world_root / "raw.bin").write_bytes(b"required raw author bytes")
    matrix.save(world_root / "archived-mi.json", {
        "material": "shader/glsl/M_terrain.json", "parameters": {}})
    page = {"surfaceId": "terrain|Landscape_00", "mip": 3, "x": 0, "y": 0,
            "offset": 64, "size": 16, "hash": "f" * 64}
    matrix.save(world_root / "manifest.json", {
        "version": 2, "package": "payload.vlcvt", "roots": [page], "pages": [page],
        "production": {"surfaces": [{
            "surfaceId": "terrain|Landscape_00", "weightSource": 0,
            "runtimeMaterialInstancePath": "archived-mi.json",
            "inputs": ["image"] * 15}],
            "objects": [{"objectId": "image", "kind": 0, "path": "image.bin"},
                        {"objectId": "raw", "kind": 3, "path": "raw.bin"}]}})
    return package


class FaultMatrixTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name).resolve()
        self.package = fixture(self.root)

    def tearDown(self):
        self.temporary.cleanup()

    def owned(self):
        evidence = self.root / "case"
        evidence.mkdir()
        private = evidence / "game"
        matrix.copy_package(self.package, private)
        matrix.save(evidence / "private-copy.json", {
            "privatePackage": str(private), "sourcePackage": str(self.package)})
        return private, evidence

    def test_plan_default_has_exact_bytes_and_never_starts_process(self):
        output = self.root / "plan"
        with patch.object(subprocess, "Popen", side_effect=AssertionError("No processes")):
            self.assertEqual(matrix.main(["--package", str(self.package), "--output", str(output)]), 0)
        report = matrix.read_json(output / "matrix.json")
        self.assertFalse(report["rendererStarted"])
        self.assertFalse(report["acceptancePassed"])
        self.assertEqual(len(report["patches"]), 5)
        self.assertEqual(report["patches"][0]["offset"], 64)
        self.assertEqual(report["patches"][0]["beforeHex"], "00")
        self.assertEqual(report["patches"][0]["afterHex"], "01")
        self.assertIn("max_budget", report["hooks"]["missing"])
        self.assertIn("slot_reuse", report["hooks"]["missing"])
        self.assertIn("max_budget", report["coverageMissing"])
        self.assertIn("--console-command", report["plannedRuns"][0]["argv"])
        self.assertIn("N/A", report["notApplicable"]["cache_corrupt"])

    def test_all_cpu_mutations_restore_exact_inventory_without_subprocess(self):
        before = matrix.inventory(self.package)
        output = self.root / "prepared"
        with patch.object(subprocess, "Popen", side_effect=AssertionError("No processes")):
            self.assertEqual(matrix.main(["--prepare", "--package", str(self.package),
                                          "--output", str(output)]), 0)
        report = matrix.read_json(output / "matrix.json")
        self.assertTrue(report["sourceUnchanged"])
        self.assertFalse(report["acceptancePassed"])
        self.assertEqual(before, matrix.inventory(self.package))
        for result in report["results"]:
            self.assertTrue(result["privateInventoryRestored"])
            receipt = matrix.read_json(output / result["case"] / "mutation.json")
            self.assertTrue(receipt["restored"])
            self.assertNotEqual(receipt["original"]["sha256"], receipt["mutated"]["sha256"])

    def test_kind3_is_required_even_when_rawmode_false_and_unreferenced_by_images(self):
        manifest = self.package / "resources/VT/world/manifest.json"
        document = matrix.read_json(manifest)
        document["production"]["rawMode"] = False
        matrix.save(manifest, document)
        records = matrix.mutations(self.package)[1]
        raw = next(row for row in records if row["case"] == "production_raw_kind3")
        self.assertEqual(raw["objectKind"], 3)
        self.assertEqual(raw["objectId"], "raw")
        self.assertTrue(raw["path"].endswith("/raw.bin"))

    def test_restore_after_runner_exception(self):
        private, evidence = self.owned()
        record = matrix.mutations(self.package)[1][0]
        record["mutatedSha256"] = matrix.virtual_hash(self.package / record["path"], record)
        before = matrix.inventory(private)
        with self.assertRaisesRegex(RuntimeError, "test failure"):
            with matrix.patched(private, record, evidence):
                raise RuntimeError("test failure")
        self.assertEqual(matrix.inventory(private), before)
        self.assertTrue(matrix.read_json(evidence / "mutation.json")["restored"])

    def test_refuse_original_even_with_forged_self_receipt(self):
        matrix.save(self.root / "private-copy.json", {
            "privatePackage": str(self.package), "sourcePackage": str(self.package)})
        with self.assertRaisesRegex(ValueError, "original"):
            with matrix.patched(self.package, {}, self.root):
                self.fail("Should not enter mutation")

    @unittest.skipUnless(os.name == "nt", "Windows MAX_PATH preflight")
    def test_long_windows_copy_refused_before_writing(self):
        destination = self.root / ("x" * 190)
        with self.assertRaisesRegex(ValueError, "shorter --private-root"):
            matrix.copy_package(self.package, destination)
        self.assertFalse(destination.exists())

    def test_reject_traversal_and_existing_destination(self):
        with self.assertRaises(ValueError):
            matrix.contained(self.package, "../original/main_game.exe")
        with self.assertRaises(ValueError):
            matrix.copy_package(self.package, self.package)

    def test_reject_hardlinked_input(self):
        os.link(self.package / "main_game.exe", self.root / "shared")
        with self.assertRaisesRegex(ValueError, "Hard-linked"):
            matrix.regular_tree(self.package)
        (self.root / "shared").unlink()

    def test_large_sparse_payload_does_not_use_readbytes(self):
        payload = self.package / "resources/VT/world/payload.vlcvt"
        offset = 1024 * 1024 * 1024 + 17
        with payload.open("r+b") as stream:
            stream.seek(offset)
            stream.write(b"\x80")
        with patch.object(Path, "read_bytes", side_effect=AssertionError("Whole read forbidden")):
            record = matrix.xor_patch(self.package, payload, offset, "large")
            self.assertEqual(record["beforeHex"], "80")
            self.assertEqual(record["afterHex"], "81")
            changed_hash = matrix.virtual_hash(payload, record)
        with payload.open("r+b") as stream:
            stream.seek(offset)
            stream.write(b"\x81")
        self.assertEqual(changed_hash, matrix.sha256(payload))

    def test_json_version_and_actual_material_switch_mutation(self):
        records = {row["case"]: row for row in matrix.mutations(self.package)[1]}
        version = records["v1_version"]
        self.assertEqual(version["beforeHex"], "32")
        self.assertEqual(version["afterHex"], "31")
        material = records["legacy_material_switch"]
        after = json.loads(bytes.fromhex(material["afterHex"]))
        self.assertEqual(after["parameters"]["u_terrainLayerMode"], 0)

    def test_crashes_timeouts_and_nonzero_exits_are_never_expected_reject(self):
        for code in (-11, 2, 3221225477):
            self.assertEqual(matrix.classify_process({
                "exitCode": code, "timedOut": False, "launchError": None},
                "EngineLoop teardown complete", "reject"), "crash-or-unexpected-exit")
        base = {"exitCode": 1, "timedOut": False, "launchError": None}
        self.assertEqual(matrix.classify_process(base, "", "reject"), "incomplete")
        self.assertEqual(matrix.classify_process(base, "EngineLoop teardown complete", "reject"),
                         "clean-rejection")
        self.assertEqual(matrix.classify_process(dict(base, timedOut=True), "", "reject"),
                         "timeout-or-launch-failure")

    def test_no_manual_pass_from_missing_events_or_just_exit_one(self):
        evidence = self.root / "logs"
        evidence.mkdir()
        result = matrix.process([sys.executable, "-c",
                                 "print('EngineLoop teardown complete'); raise SystemExit(1)"],
                                self.root, evidence, "real-process", 30)
        self.assertEqual(result["exitCode"], 1)
        self.assertGreater(result["pid"], 0)
        self.assertEqual(matrix.verify_rejection(result), "incomplete-native-diagnostic")
        assessed = matrix.assess("production_raw_kind3", result, result, {})
        self.assertEqual(assessed["status"], "incomplete")
        self.assertFalse(assessed["acceptancePassed"])

    def test_actual_native_rejection_json_preserved_and_crash_not_accepted(self):
        evidence = self.root / "native"
        evidence.mkdir()
        result = matrix.process([sys.executable, "-c",
                                 "import json; print(json.dumps({'ok': False, 'error': "
                                 "'VT production input integrity mismatch'})); raise SystemExit(1)"],
                                self.root, evidence, "native-fixture", 30)
        self.assertEqual(matrix.verify_rejection(result), "native-installation-rejected")
        self.assertIn("integrity mismatch", matrix.log_text(result))
        self.assertEqual(matrix.verify_rejection(dict(result, exitCode=3221225477)),
                         "not-a-verified-rejection")

    def test_event_envelope_and_sequence_are_required_not_boolean_pass(self):
        event = {"protocol": 1, "event": "rejection", "runId": "native-actual",
                 "sequence": 1, "monotonicMs": 12.5, "frame": 1, "worldGeneration": 1}
        line = "[RVT_EVIDENCE] " + json.dumps(event)
        self.assertEqual(matrix.events_from(line), ([event], []))
        self.assertTrue(matrix.events_from(line + "\n" + line)[1])
        self.assertTrue(matrix.events_from('[RVT_EVIDENCE] {"passed":true}')[1])
        self.assertTrue(matrix.events_from("[RVT_EVIDENCE] invalid")[1])

    def test_real_timeout_retains_exit_and_never_counts_as_rejection(self):
        evidence = self.root / "timeout"
        evidence.mkdir()
        result = matrix.process([sys.executable, "-c", "import time; time.sleep(5)"],
                                self.root, evidence, "timeout", 0.05)
        self.assertTrue(result["timedOut"])
        self.assertIsInstance(result["exitCode"], int)
        self.assertEqual(matrix.verify_rejection(result), "not-a-verified-rejection")

    def test_execute_rejects_small_cpu_fixture_before_launch(self):
        with redirect_stdout(io.StringIO()):
            with patch.object(subprocess, "Popen", side_effect=AssertionError("No GPU")):
                self.assertEqual(matrix.main(["--execute", "--package", str(self.package),
                                              "--output", str(self.root / "game-attempt")]), 1)

    def test_export_inventory_failure_is_not_store_validation_proof(self):
        evidence = self.root / "inventory-reject"
        evidence.mkdir()
        result = matrix.process([sys.executable, "-c",
                                 "import json; print(json.dumps({'ok': False, 'error': "
                                 "'Package file digest mismatch: resources/raw.bin'})); "
                                 "raise SystemExit(1)"],
                                self.root, evidence, "inventory", 30)
        self.assertEqual(matrix.installation_layer(result), "export-inventory-digest")
        self.assertEqual(matrix.verify_rejection(result), "native-installation-rejected")

    def test_missing_kind3_is_coverage_gap_not_a_pass(self):
        manifest = self.package / "resources/VT/world/manifest.json"
        document = matrix.read_json(manifest)
        document["production"]["objects"] = document["production"]["objects"][:1]
        matrix.save(manifest, document)
        output = self.root / "no-raw"
        self.assertEqual(matrix.main(["--package", str(self.package), "--output", str(output)]), 0)
        self.assertIn("production_raw_kind3", matrix.read_json(output / "matrix.json")["coverageMissing"])

    def test_missing_hook_case_is_explicitly_plannable_without_fake_mutation(self):
        output = self.root / "missing-hook"
        with patch.object(subprocess, "Popen", side_effect=AssertionError("No native hook")):
            self.assertEqual(matrix.main(["--package", str(self.package), "--output", str(output),
                                          "--case", "max_budget"]), 0)
        report = matrix.read_json(output / "matrix.json")
        self.assertEqual(report["patches"], [])
        self.assertEqual(report["coverageMissing"], ["max_budget"])
        self.assertFalse(report["acceptancePassed"])

    def test_reuse_evidence_directory_is_refused(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(matrix.main(["--package", str(self.package),
                                          "--output", str(self.root)]), 1)


if __name__ == "__main__":
    unittest.main()

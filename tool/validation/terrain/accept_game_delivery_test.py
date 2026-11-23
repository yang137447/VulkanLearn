"""CPU tests for bounded delivery fault mutations; no renderer is launched."""

import json
import tempfile
from pathlib import Path
import unittest

from accept_game_delivery import (
    NEGATIVE_CASES, negative_payload, runtime_fault_policy, validate_recovery, payload_corruption,
)
from rvt_delivery import IncompleteEvidence


class CutoverMutationTests(unittest.TestCase):
    def setUp(self):
        self.document = {
            "type": "terrain", "heightmapPath": "height.png",
            "compositedVT": {"enabled": True, "packagePath": "VT/tile/manifest.json",
                             "virtualResolution": [8192, 8192],
                             "productionSourcePath": "Generated/world.author.json"},
        }
        self.original = json.dumps(self.document, indent=2).encode()

    def test_missing_cutover_removes_only_composited_block(self):
        result = json.loads(negative_payload("tr-missing-composited-vt", self.original))
        self.assertEqual(result, {key: value for key, value in self.document.items() if key != "compositedVT"})
        self.assertTrue(json.loads(self.original)["compositedVT"]["enabled"])

    def test_disabled_cutover_preserves_world_reference(self):
        result = json.loads(negative_payload("tr-disabled-composited-vt", self.original))
        expected = json.loads(self.original)
        expected["compositedVT"]["enabled"] = False
        self.assertEqual(result, expected)

    def test_restore_returns_exact_bytes_after_exception(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "terrain.json"
            path.write_bytes(self.original)
            for name in ("tr-missing-composited-vt", "tr-disabled-composited-vt"):
                try:
                    path.write_bytes(negative_payload(name, self.original))
                    raise RuntimeError("simulated failed runtime assertion")
                except RuntimeError:
                    pass
                finally:
                    path.write_bytes(self.original)
                self.assertEqual(path.read_bytes(), self.original)

    def test_cutover_is_in_default_negative_set(self):
        self.assertIn("tr-missing-composited-vt", NEGATIVE_CASES)
        self.assertIn("tr-disabled-composited-vt", NEGATIVE_CASES)

    def test_unknown_fault_rejected(self):
        with self.assertRaises(ValueError):
            negative_payload("unknown", self.original)

    def test_old_budgets_are_negative_inputs_only(self):
        for name, field in (("tr-resident-pages", "residentPages"), ("tr-height-budget", "heightBudgetBytes")):
            changed = json.loads(negative_payload(name, self.original))
            self.assertIn(field, changed["compositedVT"])
            self.assertNotIn(field, json.loads(self.original)["compositedVT"])

    def test_v1_mutation_downgrades_real_v2(self):
        original = json.dumps({"version": 2}).encode()
        self.assertEqual(json.loads(negative_payload("wrong-version", original)), {"version": 1})

    def test_packaged_payload_damage_recovers_but_production_input_rejects_world(self):
        self.assertEqual(runtime_fault_policy("corrupt-root"), "rebuild-from-production")
        self.assertEqual(runtime_fault_policy("corrupt-coarse"), "rebuild-from-production")
        self.assertEqual(runtime_fault_policy("corrupt-production-input"), "reject-world")
        self.assertNotIn("missing-page-package", NEGATIVE_CASES)
        self.assertNotIn("corrupt-page", NEGATIVE_CASES)

    def test_corruption_targets_selected_page_without_damaging_package_header(self):
        original = bytes(range(64))
        changed = negative_payload("corrupt-root", original, 48)
        self.assertEqual(changed[:48], original[:48])
        self.assertNotEqual(changed[48], original[48])
        self.assertEqual(changed[49:], original[49:])

    def test_exit_zero_does_not_prove_root_recovery(self):
        with self.assertRaises(IncompleteEvidence):
            validate_recovery("EngineLoop teardown complete", {"surfaceId": "terrain|Landscape_00"}, True)

    def test_absent_record_is_never_damaged_payload_evidence(self):
        for reason in ("absent", "not-packaged", "missing-root-record"):
            self.assertFalse(payload_corruption({"kind": "root", "reason": reason}))
        self.assertFalse(payload_corruption({"kind": "root"}))
        self.assertTrue(payload_corruption({"kind": "root", "expectedDigest": "a" * 64,
                                            "observedDigest": "b" * 64}))


if __name__ == "__main__":
    unittest.main()

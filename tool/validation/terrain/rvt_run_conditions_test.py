"""CPU process-profile and raw sample completeness tests; no Game run."""

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from rvt_run_conditions import process_conditions, analyze_complete_raw, finite_production_suite


class ConditionsTests(unittest.TestCase):
    def config(self):
        return {"ui": {"enabled": False}, "windowSize": [1280, 720], "msaa": 2}

    def test_formal_conditions_lock_profile_and_external_quality_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.dict("os.environ", {"VL_GPU_TIMING_RAW": "0", "VL_UNKNOWN": "leaked"}, clear=False):
                env, contract = process_conditions(root / "game", root / "evidence", self.config(),
                                                    "formal-performance", 2, "capture-4hz")
            self.assertFalse(contract["validation"])
            self.assertEqual(env["VL_GPU_TIMING_RAW"], "1")
            self.assertNotIn("VL_UNKNOWN", env)
            self.assertEqual(env["VULKANLEARN_RVT_QUALITY_DIR"], str((root / "evidence/quality").resolve()))

    def test_ui_on_worker_one_and_capture_disabled_not_formal(self):
        for worker, quality, config in ((1, "capture-4hz", self.config()),
                (2, "paired-steady", self.config()),
                (2, "capture-4hz", dict(self.config(), ui={"enabled": True}))):
            with self.assertRaises(ValueError):
                process_conditions(Path("game"), Path("evidence"), config, "formal-performance", worker, quality)

    def test_missing_raw_frames_or_quality_cannot_pass(self):
        result = analyze_complete_raw('[CPU_RAW] {"renderFrameIntervalMs":33}\n', [])
        self.assertEqual(result["status"], "incomplete")
        self.assertFalse(result["formalPerformancePassed"])

    def test_null_gpu_queries_cannot_be_silently_filtered(self):
        text = '[GPU_RAW] {"frame":0,"passes":[{"name":"geometry","ms":null}]}\n'
        result = analyze_complete_raw(text, [])
        self.assertTrue(any("Incomplete GPU query" in item for item in result["problems"]))

    def test_partial_production_cannot_substitute_full10936(self):
        with self.assertRaisesRegex(ValueError, "every unique page"):
            finite_production_suite([{"weightSource": 0, "mip": 13, "x": 0, "y": 0}])


if __name__ == "__main__":
    unittest.main()

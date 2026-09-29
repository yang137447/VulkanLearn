"""Pure-logic checks for the VL offline stress driver; never launches main.exe."""

import importlib.util
import unittest
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    "stress_vl_integration", Path(__file__).with_name("stress_vl_integration.py"))
stress = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stress)


class ExportSelectionTest(unittest.TestCase):
    def test_selects_viewport_among_process_windows(self):
        self.assertEqual(stress.select_renderer_window([
            (11, "VulkanRenderer - FPS: 60"), (12, "Default IME")]), 11)
        with self.assertRaisesRegex(RuntimeError, "expected one"):
            stress.select_renderer_window([(11, "VulkanRenderer"), (12, "VulkanRenderer - FPS: 60")])

    def test_only_new_export_under_resource_root(self):
        root = Path("C:/ResourceRoot")
        old = root / "Generated/OfflineRenderer/old"
        new = root / "Generated/OfflineRenderer/new"
        log = f"[Diagnostics][Info] Offline output: {old}\n"
        start = len(log)
        self.assertIsNone(stress.export_path_since(log, start, root))
        self.assertEqual(stress.export_path_since(
            log + f"debug> [Diagnostics][Info] Offline output: {new}\n", start, root), new)

    def test_rejects_unrelated_output(self):
        root = Path("C:/ResourceRoot")
        with self.assertRaisesRegex(ValueError, "outside configured"):
            stress.export_path_since(
                "[Diagnostics][Info] Offline output: C:/Other/Generated/OfflineRenderer/new\n", 0, root)


class ManifestContractTest(unittest.TestCase):
    def setUp(self):
        self.scene = Path("C:/ResourceRoot") / stress.SCENE
        self.manifest = {
            "scene": str(self.scene), "worldGeneration": 1, "samples": 1,
            "targetSamples": 1, "complete": True, "inputDigest": "a" * 64,
            "integrator": {"width": 640, "height": 360, "seed": 17, "bounces": 2},
            "outputs": {name: "b" * 64 for name in (
                "combined.pfm", "direct.pfm", "indirect.pfm", "combined.ppm")},
        }

    def test_final_identity(self):
        stress.check_manifest(self.manifest, self.scene, (640, 360), 17, 2)

    def test_stale_settings_and_incomplete_aov_rejected(self):
        self.manifest["integrator"]["seed"] = 9
        with self.assertRaisesRegex(ValueError, "integrator.seed"):
            stress.check_manifest(self.manifest, self.scene, (640, 360), 17, 2)
        self.manifest["integrator"]["seed"] = 17
        del self.manifest["outputs"]["indirect.pfm"]
        with self.assertRaisesRegex(ValueError, "indirect.pfm"):
            stress.check_manifest(self.manifest, self.scene, (640, 360), 17, 2)

    def test_wrong_scene_and_resolution_rejected(self):
        with self.assertRaisesRegex(ValueError, "manifest scene"):
            stress.check_manifest(self.manifest, Path("C:/Other/scene.json"), (640, 360), 17, 2)
        with self.assertRaisesRegex(ValueError, "integrator.width"):
            stress.check_manifest(self.manifest, self.scene, (800, 450), 17, 2)


class RenderExitContractTest(unittest.TestCase):
    def setUp(self):
        self.scene = Path("C:/ResourceRoot") / stress.SCENE
        self.manifest = {
            "scene": str(self.scene), "worldGeneration": 1,
            "samples": 2, "targetSamples": stress.RENDER_EXIT_SAMPLES,
            "complete": False, "inputDigest": "a" * 64,
            "integrator": {"width": 640, "height": 360, "seed": 101},
            "outputs": {name: "b" * 64 for name in (
                "combined.pfm", "direct.pfm", "indirect.pfm", "combined.ppm")},
        }

    def test_new_incomplete_film_is_accepted_but_old_target_is_skipped(self):
        self.assertTrue(stress.check_partial_manifest(self.manifest, self.scene, (640, 360)))
        self.manifest["targetSamples"] = 32
        self.assertFalse(stress.check_partial_manifest(self.manifest, self.scene, (640, 360)))

    def test_complete_or_empty_film_is_not_in_progress(self):
        self.manifest["samples"] = 0
        with self.assertRaisesRegex(ValueError, "incomplete published film"):
            stress.check_partial_manifest(self.manifest, self.scene, (640, 360))
        self.manifest["samples"] = stress.RENDER_EXIT_SAMPLES
        self.manifest["complete"] = True
        with self.assertRaisesRegex(ValueError, "incomplete published film"):
            stress.check_partial_manifest(self.manifest, self.scene, (640, 360))

    def test_stale_identity_and_missing_output_digest_are_rejected(self):
        self.manifest["integrator"]["seed"] = 17
        with self.assertRaisesRegex(ValueError, "wrong seed"):
            stress.check_partial_manifest(self.manifest, self.scene, (640, 360))
        self.manifest["integrator"]["seed"] = 101
        self.manifest["integrator"]["width"] = 1280
        with self.assertRaisesRegex(ValueError, "wrong dimensions"):
            stress.check_partial_manifest(self.manifest, self.scene, (640, 360))
        self.manifest["integrator"]["width"] = 640
        del self.manifest["outputs"]["indirect.pfm"]
        with self.assertRaisesRegex(ValueError, "indirect.pfm"):
            stress.check_partial_manifest(self.manifest, self.scene, (640, 360))

    def test_teardown_requires_ordered_markers_zero_exit_and_clean_stderr(self):
        log = "[Diagnostics][Info] Quit event received\n" \
              "[Diagnostics][Info] EngineLoop teardown complete.\n"
        self.assertIsNone(stress.teardown_issue(0, log, ""))
        self.assertIn("exit code", stress.teardown_issue(1, log, ""))
        self.assertIn("ordered", stress.teardown_issue(
            0, "EngineLoop teardown complete.\nQuit event received\n", ""))
        self.assertIn("renderer failure", stress.teardown_issue(
            0, log + "Render thread failed: device lost\n", ""))
        self.assertIn("not empty", stress.teardown_issue(0, log, "terminate called\n"))

    def test_pass_requires_driver_close_and_no_cleanup_error(self):
        result = {"cleanClose": True, "closeRequested": True}
        self.assertTrue(stress.run_succeeded(result))
        for key, value in (("closeError", "timed out"), ("error", "export failed"),
                           ("closeRequested", False), ("cleanClose", False)):
            failed = {**result, key: value}
            self.assertFalse(stress.run_succeeded(failed))


if __name__ == "__main__":
    unittest.main()

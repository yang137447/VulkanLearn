"""CPU-only tests for false-pass rejection in the S4 evidence checker."""

import json
from pathlib import Path
import struct
import tempfile
import unittest

from editor_s4 import STAGES, analyze, rgb_pixels
from editor_s4_prepare import closure, prepare


def bitmap(path, color, alpha=0):
    pixels = bytes((color, color, color, alpha)) * 4
    header = b"BM" + struct.pack("<IHHI", 54 + len(pixels), 0, 0, 54)
    header += struct.pack("<IiiHHIIiiII", 40, 2, -2, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    path.write_bytes(header + pixels)


class EvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.placement = "terrain|fixture"
        rows = []
        paints = ("p0", "p1", "p0", "p1", "p1", "p1", "p1") + ("p2",) * 7
        heights = ("h0", "h0", "h0", "h0", "h1", "h0") + ("h1",) * 8
        colors = (10, 20, 10, 20, 30, 20, 30, 40, 50, 40, 40, 40, 40, 40)
        snapshots = {}
        for index, stage in enumerate(STAGES):
            rows.append(f"EDITOR_S4_STAGE {stage}")
            for sample in range(2):
                rows.append(f"EDITOR_S4_SAMPLE {stage} {sample}")
                rows.append("TerrainVT generation=1 surfaces=1 previewReadbacks=0 "
                            f"staleBakeResults={int(index >= 7)}")
                revision = min(index, 9)
                dirty = 2 if sample == 0 and stage != "baseline" else 0
                rows.append(f"  {self.placement} generation=1 revision={revision} sourceRevision=1 "
                            f"resident=8/16 queued=0 reading=0 dirty={dirty} stale=0 "
                            "readFailures=0 discarded=0 pendingEditorUpdates=0 pendingMaterialUpdates=0")
            snapshots[stage] = {
                "stage": stage, "height": heights[index], "paint": paints[index],
                "height16": heights[index] + "16", "liveUv": 7.0 if stage == "material" else 4.0,
                "materialDiskHash": "mi1" if stage == "conflict" else "mi0",
                "generation": 1, "placement": self.placement, "totalPages": 100}
            rows.append("EDITOR_S4_STATE " + json.dumps(snapshots[stage]))
            if stage == "rapid":
                for completed in range(-1, 24):
                    rows.append("EDITOR_S4_STAMP " + json.dumps({
                        "completedStamp": completed, "worldHash": f"stamp{completed}"}))
            if stage in ("material", "material_revert", "material_reject"):
                rows.append("EDITOR_S4_PREVIEW " + json.dumps({
                    "stage": stage, "status": "Failed" if stage == "material_reject" else "Completed",
                    "liveSwapCommitted": stage != "material_reject", "replacesLiveResource": False}))
            if stage == "conflict":
                rows.append("EDITOR_S4_EXTERNAL " + json.dumps({
                    "event": "injected", "hostTickSerial": 100, "elapsedMs": 10000}))
                rows.append("EDITOR_S4_FEEDBACK " + json.dumps({
                    "stage": stage, "message": f"Terrain source changed: {self.placement}; unsaved edits conflict.",
                    "hostTickSerial": 200, "elapsedMs": 12000, "stageElapsedMs": 2000,
                    "feedbackChanged": True}))
            if stage == "conflict_recover":
                rows.append("EDITOR_S4_EXTERNAL " + json.dumps({
                    "event": "restored", "hostTickSerial": 400, "elapsedMs": 14000}))
            bitmap(self.directory / f"{stage}.bmp", colors[index])
        self.log = "\n".join(rows)
        (self.directory / "process.json").write_text('{"exitCode":0}')
        saved = {name: {"before": "a", "after": "b"} for name in ("paintPath", "heightmapPath")}
        saved["paintPath"]["manifest"] = {
            "kind": "terrainLayerWeights", "version": 2, "contentSha256": "b"}
        (self.directory / "save-files.json").write_text(json.dumps(saved))
        reopened = {name: {**snapshots["save"], "stage": name, "generation": generation}
                    for name, generation in (("reopen", 1), ("reload", 2))}
        lifecycle = {"bootstrapCook": {"exitCode": 0}, "savedCook": {"exitCode": 0},
                     "reopen": {"exitCode": 0}, "authorBeforeCook": {"a": "saved"},
                     "authorAfterCook": {"a": "saved"}, "cookedBeforeSave": {"fixture": "old"},
                     "cookedAfterSave": {"fixture": "new"}, "reopenSettled": True, "snapshots": reopened}
        (self.directory / "lifecycle.json").write_text(json.dumps(lifecycle))
        for name in reopened:
            bitmap(self.directory / f"{name}.bmp", 40)

    def check(self, text=None):
        (self.directory / "editor.log").write_text(text if text is not None else self.log)
        return analyze(self.directory, self.placement)

    def report(self):
        return json.loads((self.directory / "editor-s4-report.json").read_text())

    def test_complete_synthetic_evidence_passes(self):
        self.assertEqual(self.check(), 0)

    def test_missing_stage_cannot_pass(self):
        self.assertNotEqual(self.check(self.log.replace("EDITOR_S4_STAGE paint\n", "")), 0)

    def test_unchanged_cpu_paint_fails(self):
        self.assertEqual(self.check(self.log.replace('"p1"', '"p0"')), 2)

    def test_rapid_edits_without_world_change_fail(self):
        self.assertEqual(self.check(self.log.replace('"p2"', '"p1"')), 2)

    def test_rapid_stamp_without_real_change_fails(self):
        self.assertEqual(self.check(self.log.replace('"stamp0"', '"stamp-1"')), 2)

    def test_missing_per_stamp_hashes_is_incomplete(self):
        self.assertEqual(self.check(self.log.replace("EDITOR_S4_STAMP ", "OTHER ")), 3)

    def test_full_terrain_dirty_fails(self):
        self.assertEqual(self.check(self.log.replace("dirty=2", "dirty=100")), 2)

    def test_unsettled_stale_fails(self):
        self.assertEqual(self.check(self.log.replace("stale=0", "stale=1")), 2)

    def test_unrequested_dirty_is_allowed_when_resident_stale_and_queues_are_zero(self):
        self.assertEqual(self.check(self.log.replace("dirty=0", "dirty=3")), 0)
        self.assertEqual(self.report()["dirtyNonresident"]["paint"], 3)

    def test_dirty_nonresident_does_not_ignore_pending_requests(self):
        self.assertEqual(self.check(self.log.replace("dirty=0", "dirty=3").replace("queued=0", "queued=1")), 2)

    def test_small_unchanged_undo_region_cannot_hide_in_global_average(self):
        for name in ("baseline", "paint", "paint_undo"):
            bitmap(self.directory / f"{name}.bmp", 10)
        for name in ("paint", "paint_undo"):
            path = self.directory / f"{name}.bmp"
            raw = bytearray(path.read_bytes())
            raw[54:57] = bytes((13, 13, 13))
            path.write_bytes(raw)
        self.assertEqual(self.check(), 2)
        self.assertTrue(any("edited-pixel restoration residual" in item
                            for item in self.report()["failures"]))

    def test_missing_obsolete_result_is_incomplete(self):
        self.assertEqual(self.check(self.log.replace("staleBakeResults=1", "staleBakeResults=0")), 3)

    def test_alpha_only_change_is_not_gpu_evidence(self):
        bitmap(self.directory / "paint.bmp", 10, alpha=255)
        self.assertEqual(self.check(), 2)
        self.assertTrue(any("GPU screenshot did not change" in item for item in self.report()["failures"]))

    def test_undo_gpu_mismatch_fails(self):
        bitmap(self.directory / "paint_undo.bmp", 100)
        self.assertEqual(self.check(), 2)

    def test_save_without_disk_change_fails(self):
        (self.directory / "save-files.json").write_text(
            '{"heightmapPath":{"before":"a","after":"a"}}')
        self.assertEqual(self.check(), 2)

    def test_abnormal_process_exit_fails(self):
        (self.directory / "process.json").write_text('{"exitCode":1}')
        self.assertEqual(self.check(), 2)

    def test_early_gpu_failure_does_not_claim_unreached_save_failed(self):
        (self.directory / "process.json").write_text('{"exitCode":3221226505}')
        text = self.log.split("EDITOR_S4_STAGE paint_undo")[0] + "\nValidation Error VUID-test"
        self.assertEqual(self.check(text), 2)
        report = self.report()
        self.assertIn("save", report["execution"]["unreachedStages"])
        self.assertFalse(any(item.startswith("Save did not change") for item in report["failures"]))

    def test_missing_save_manifest_fails(self):
        saved = json.loads((self.directory / "save-files.json").read_text())
        saved["paintPath"].pop("manifest")
        (self.directory / "save-files.json").write_text(json.dumps(saved))
        self.assertEqual(self.check(), 2)

    def test_validation_error_fails(self):
        self.assertEqual(self.check(self.log + "\nValidation Error VUID-s4-negative"), 2)

    def test_rgb_decoder_ignores_alpha(self):
        first, second = self.directory / "first.bmp", self.directory / "second.bmp"
        bitmap(first, 30, 0)
        bitmap(second, 30, 255)
        self.assertEqual(rgb_pixels(first), rgb_pixels(second))

    def lifecycle(self, mutate):
        path = self.directory / "lifecycle.json"
        document = json.loads(path.read_text())
        mutate(document)
        path.write_text(json.dumps(document))

    def test_failed_cook_fails(self):
        self.lifecycle(lambda doc: doc["savedCook"].update(exitCode=1))
        self.assertEqual(self.check(), 2)

    def test_cook_mutating_saved_authors_fails(self):
        self.lifecycle(lambda doc: doc.update(authorAfterCook={"a": "changed"}))
        self.assertEqual(self.check(), 2)

    def test_unchanged_cooked_dependencies_fail(self):
        self.lifecycle(lambda doc: doc.update(cookedAfterSave=doc["cookedBeforeSave"]))
        self.assertEqual(self.check(), 2)

    def test_saved_world_not_reopened_fails(self):
        self.lifecycle(lambda doc: doc["snapshots"]["reopen"].update(paint="old-paint"))
        self.assertEqual(self.check(), 2)

    def test_reload_without_generation_change_fails(self):
        self.lifecycle(lambda doc: doc["snapshots"]["reload"].update(generation=1))
        self.assertEqual(self.check(), 2)

    def test_missing_lifecycle_is_incomplete(self):
        (self.directory / "lifecycle.json").unlink()
        self.assertEqual(self.check(), 3)

    def test_no_actual_preview_result_is_incomplete(self):
        self.assertEqual(self.check(self.log.replace("EDITOR_S4_PREVIEW ", "OTHER ")), 3)

    def test_rejected_stale_preview_cannot_commit(self):
        text = self.log.replace('"status": "Failed", "liveSwapCommitted": false',
                                '"status": "Completed", "liveSwapCommitted": true')
        self.assertEqual(self.check(text), 2)

    def test_missing_actual_host_conflict_is_incomplete(self):
        self.assertEqual(self.check(self.log.replace("unsaved edits conflict", "changed")), 3)

    def test_preexisting_feedback_cannot_count_as_a_new_external_conflict(self):
        self.assertEqual(self.check(self.log.replace('"feedbackChanged": true', '"feedbackChanged": false')), 3)

    def test_short_external_observation_window_is_incomplete(self):
        self.assertEqual(self.check(self.log.replace('"stageElapsedMs": 2000', '"stageElapsedMs": 500')), 3)

    def test_fresh_cook_cannot_already_be_stale_at_baseline(self):
        feedback = '\nEDITOR_S4_FEEDBACK ' + json.dumps({
            "stage": "baseline", "message": f"Terrain source changed: {self.placement}; reload and Cook."})
        self.assertEqual(self.check(self.log + feedback), 2)

    def test_conflict_must_preserve_live_uv(self):
        text = self.log.replace('"liveUv": 4.0, "materialDiskHash": "mi1"',
                                '"liveUv": 10.0, "materialDiskHash": "mi1"')
        self.assertEqual(self.check(text), 2)


class ClosureTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "source"
        self.root.mkdir()
        documents = {
            "scene.json": {"objects": [
                {"type": "terrain", "name": "fixture", "terrainPath": "terrain.json"},
                {"type": "camera", "position": [0, 1, 0], "look_at": [0, 0, 0]}]},
            "terrain.json": {"type": "terrain", "heightmapPath": "height.png",
                             "materialInstancePath": "material.json",
                             "paintPath": "new.vlpaint",
                             "compositedVT": {"enabled": True, "packagePath": "Cook/manifest.json"}},
            "material.json": {"type": "materialInstance", "material": "shader/glsl/M_terrain.json",
                              "textures": {"layerAlbedo": "texture.json"}},
            "texture.json": {"type": "textureArray", "sources": ["source.png"]},
            "capture_plan.json": {"shots": [{"name": "near", "position": [0, 3, 4], "look_at": [0, 0, 0]}]},
        }
        for name, document in documents.items():
            (self.root / name).write_text(json.dumps(document))
        for name in ("height.png", "source.png", "unrelated-other-topic.bin"):
            (self.root / name).write_bytes(b"payload")

    def test_only_reachable_closure_is_copied_without_hardlinks(self):
        destination = self.base / "copy"
        report = prepare(self.root, "scene.json", destination, project=self.base)
        self.assertNotIn("unrelated-other-topic.bin", report["files"])
        self.assertTrue((destination / "Maps").is_dir())
        self.assertEqual((destination / "source.png").read_bytes(), b"payload")
        self.assertFalse((destination / "source.png").samefile(self.root / "source.png"))
        (destination / "height.png").write_bytes(b"edited")
        self.assertEqual((self.root / "height.png").read_bytes(), b"payload")
        material = json.loads((destination / "material.json").read_text())
        self.assertNotIn("u_heightBlendDistanceFade", material.get("parameters", {}))

    def test_retired_material_parameters_are_removed_from_private_copy(self):
        path = self.root / "material.json"
        material = json.loads(path.read_text())
        material["parameters"] = {
            "u_heightBlendDistanceFade": 1, "u_terrainLayerMode": 1,
            "u_layerForceMip": 3, "u_uvScale": 7,
        }
        path.write_text(json.dumps(material))
        destination = self.base / "clean-copy"
        prepare(self.root, "scene.json", destination, project=self.base)
        copied = json.loads((destination / "material.json").read_text())
        self.assertEqual(copied["parameters"], {"u_uvScale": 7})
        self.assertEqual(json.loads(path.read_text())["parameters"]["u_terrainLayerMode"], 1)

    def test_missing_required_payload_fails(self):
        (self.root / "source.png").unlink()
        with self.assertRaisesRegex(ValueError, "Missing fixture reference"):
            closure(self.root, "scene.json", project=self.base)

    def test_render_graph_materials_join_the_fixture_closure(self):
        config = self.base / "config"
        config.mkdir()
        (config / "renderGraphConfig.json").write_text(
            '{"passes":[{"materialInstancePath":"Common/Materials/MI_shadow.json"}]}')
        material = self.root / "Common/Materials/MI_shadow.json"
        material.parent.mkdir(parents=True)
        material.write_text('{"type":"materialInstance","material":"shader/glsl/M_shadow.json"}')
        files, _ = closure(self.root, "scene.json", project=self.base)
        self.assertIn(material.resolve(), files)

    def test_traversal_reference_fails(self):
        (self.root / "texture.json").write_text('{"sources":["../escape.png"]}')
        with self.assertRaises(ValueError):
            closure(self.root, "scene.json", project=self.base)

    def test_missing_output_packages_are_not_author_errors(self):
        _, optional = closure(self.root, "scene.json", project=self.base)
        self.assertEqual(optional, ["Cook/manifest.json", "new.vlpaint"])


if __name__ == "__main__":
    unittest.main()

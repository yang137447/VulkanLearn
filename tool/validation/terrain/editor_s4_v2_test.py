"""CPU tests for V2 acceptance planning and evidence rejection, never renderer tests."""

import tempfile
from pathlib import Path
import unittest

from editor_s4_v2 import native_checks, scenario
from editor_s4_prepare import closure
from run_composited_acceptance import write_json


class V2EvidenceTests(unittest.TestCase):
    def native(self):
        token = dict(worldGeneration=12, activeWorldGeneration=12, revision=5,
                     activeRevision=5, contentIdentity="new", activeContentIdentity="new",
                     ticket=31, editSeq=101, surface="terrain|Landscape_00")
        result = [dict(token, event=kind) for kind in ("queue", "prepared", "submitted", "published")]
        result += [dict(token, event="page-table-write"), dict(token, event="dirty-ack", state="Published")]
        result += [
            dict(event="stale-rejected", reason="world-generation", pageTableWritten=False),
            dict(event="baseline-v2", storeVersion=2, v1SidecarReads=0, verifiedProductionObjects=True,
                 capturedDependencyCount=2, matchesStoredAuthorDependencies=True),
            dict(event="dependency-reload", beforeDigest="old", afterDigest="new", candidatePublished=True),
            dict(event="frame-input-immutable", retainedThroughFence=True, **{
                name: {"beforeDigest": name, "afterDigest": name}
                for name in ("weights", "normalFrame", "tangentFrame")}),
            dict(event="neighbor-dirty", sourceSurface="terrain|Landscape_00",
                 targetSurface="terrain|Landscape_01", pageKeys=["mip0:0:0"]),
            dict(event="author-save")]
        return result

    def test_complete_synthetic_trace_only_validates_checker(self):
        self.assertEqual(native_checks(self.native()), ([], []))

    def test_missing_native_trace_is_incomplete(self):
        failures, missing = native_checks([])
        self.assertFalse(failures)
        self.assertGreater(len(missing), 10)

    def test_enqueue_and_submitted_never_ack_dirty(self):
        for state in ("Queued", "Prepared", "Submitted", "Failed", "Stale"):
            rows = self.native()
            next(row for row in rows if row["event"] == "dirty-ack")["state"] = state
            self.assertIn("Dirty acknowledged before Published", native_checks(rows)[0])

    def test_late_generation_or_content_cannot_write_new_table(self):
        for field in ("worldGeneration", "revision", "contentIdentity"):
            rows = self.native()
            next(row for row in rows if row["event"] == "page-table-write")[field] = "old"
            self.assertIn("Native publication token mismatch", native_checks(rows)[0])

    def test_changed_weight_or_frame_snapshot_fails(self):
        for field in ("weights", "normalFrame", "tangentFrame"):
            rows = self.native()
            frame = next(row for row in rows if row["event"] == "frame-input-immutable")
            frame[field]["afterDigest"] = "mutated"
            self.assertTrue(native_checks(rows)[0])

    def test_v1_reads_or_unverified_v2_objects_fail(self):
        for field, value in (("v1SidecarReads", 1), ("verifiedProductionObjects", False),
                             ("capturedDependencyCount", 0), ("matchesStoredAuthorDependencies", False)):
            rows = self.native()
            next(row for row in rows if row["event"] == "baseline-v2")[field] = value
            self.assertTrue(native_checks(rows)[0])

    def test_publication_without_prepared_or_in_wrong_order_fails(self):
        rows = self.native()
        rows = [row for row in rows if row["event"] != "prepared"]
        self.assertIn("Published ticket lacks Queue/Prepared/Submitted history", native_checks(rows)[0])
        rows = self.native()
        rows[1], rows[3] = rows[3], rows[1]
        self.assertIn("Published ticket lifecycle is out of order", native_checks(rows)[0])

    def test_plan_spans_adjacent_surfaces_cancel_reload_and_three_states(self):
        link = dict(sourceEdge="maxU")
        plan = scenario("terrain|Landscape_00", "terrain|Landscape_01", link)
        steps = {step["id"]: step for step in plan["steps"]}
        self.assertFalse(plan["fullCookPrerequisite"])
        self.assertEqual(steps["boundary_paint"]["operations"][1]["u"], 0.995)
        self.assertEqual(steps["cross_surface"]["operations"][0]["surface"], plan["surfaceB"])
        for step in ("paint_pending_cancel", "paint_cancel", "sculpt_pending_cancel",
                     "sculpt_cancel", "hotreload", "hotreload_restore", "preview_published",
                     "save_a", "save_b", "saved_preview", "reload_inflight", "old_world_action"):
            self.assertIn(step, steps)

    def test_v2_closure_does_not_request_v1_dependency_sidecar(self):
        with tempfile.TemporaryDirectory(prefix="vl-s4v2-checker-") as temporary:
            root = Path(temporary).resolve()
            write_json(root / "scene.json", {"objects": [], "publication": "world.json"})
            write_json(root / "world.json", {"kind": "terrain-composited-vt", "version": 2,
                "surfaces": [{"surfaceId": "terrain|Landscape_00", "dependencyHash": "a" * 64}],
                "package": "coarse.bin", "production": {"objects": [
                    {"path": "objects/frame.bin"}]}})
            (root / "coarse.bin").write_bytes(b"coarse")
            (root / "objects").mkdir()
            (root / "objects/frame.bin").write_bytes(b"frame")
            files, _ = closure(root, "scene.json", project=root / "unused-project")
            self.assertEqual({path.relative_to(root).as_posix() for path in files},
                             {"scene.json", "world.json", "coarse.bin", "objects/frame.bin"})


if __name__ == "__main__":
    unittest.main()
